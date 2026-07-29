#include <RTSEngine/RtsScripting/RtsEntityBehaviorRuntime.h>

#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <realscript/game/GameProductization.h>

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace rts::gameplay::scripting {
namespace detail {

struct RtsEntityBehaviorIntent final {
    CommandType type{CommandType::Move};
    std::int32_t x{};
    std::int32_t y{};
    ecs::Entity target{};
    std::uint32_t ordinal{};
};

struct RtsEntityBehaviorBindingState final {
    const RtsScriptReadView* view{};
    const DomainEvent* event{};
    const RtsScriptTriggerDefinition* trigger{};
    std::vector<RtsEntityBehaviorIntent>* intents{};
    std::size_t maximumIntents{};
    ecs::Entity self{};
    std::uint32_t teamId{};
    std::uint64_t completedTick{};
    std::uint64_t targetTick{};
};

} // namespace detail

namespace {

using realscript::runtime::ExecutionOptions;
using realscript::runtime::RuntimeError;
using realscript::runtime::Value;

constexpr std::uint32_t kStateMagic = 0x34424552u; // REB4
constexpr std::uint16_t kStateVersion = 1u;
constexpr std::uint32_t kMaximumBehaviors = 65536u;
constexpr std::uint32_t kMaximumTriggersPerBehavior = 4096u;
constexpr std::uint32_t kMaximumSequences = 4096u;
constexpr std::uint32_t kMaximumStringBytes = 4096u;
constexpr std::uint32_t kMaximumObjectBytes = 16u * 1024u * 1024u;

void fail(RuntimeError& error,
          realscript::runtime::ErrorCode code,
          std::string message) {
    error.code = code;
    error.message = std::move(message);
    error.stackTrace.clear();
}

ExecutionOptions executionOptions(
    const rts::scripting::ScriptExecutionPolicy& policy) {
    ExecutionOptions options;
    options.limits.instructionBudget =
        std::max<std::uint64_t>(1u, policy.instructionBudget);
    options.limits.recursionLimit =
        std::max<std::size_t>(1u, policy.recursionLimit);
    options.limits.gcWorkBudget = policy.gcWorkBudget;
    options.determinism.mode = policy.strictDeterminism
        ? realscript::runtime::DeterminismMode::Strict
        : realscript::runtime::DeterminismMode::Off;
    return options;
}

bool sameIdentity(
    const rts::scripting::ScriptProgramIdentity& first,
    const rts::scripting::ScriptProgramIdentity& second) noexcept {
    return first.bundle == second.bundle &&
           first.bundlePayloadHash == second.bundlePayloadHash &&
           first.script.sdkCompatibilityVersion ==
               second.script.sdkCompatibilityVersion &&
           first.script.gameSdkPackageVersion ==
               second.script.gameSdkPackageVersion &&
           first.script.hostApiHash == second.script.hostApiHash &&
           first.script.programContentHash == second.script.programContentHash;
}

void writeIdentity(
    foundation::BinaryWriter& writer,
    const rts::scripting::ScriptProgramIdentity& identity) {
    writer.writeU16(static_cast<std::uint16_t>(identity.bundle.type));
    writer.writeU64(identity.bundle.id);
    writer.writeU64(identity.bundlePayloadHash);
    writer.writeU32(identity.script.sdkCompatibilityVersion);
    writer.writeU32(identity.script.gameSdkPackageVersion);
    writer.writeU64(identity.script.hostApiHash);
    writer.writeU64(identity.script.programContentHash);
}

bool readIdentity(
    foundation::BinaryReader& reader,
    rts::scripting::ScriptProgramIdentity& identity) {
    std::uint16_t type = 0;
    if (!reader.readU16(type) ||
        type != static_cast<std::uint16_t>(assets::AssetType::ScriptBundle) ||
        !reader.readU64(identity.bundle.id) || identity.bundle.id == 0 ||
        !reader.readU64(identity.bundlePayloadHash) ||
        identity.bundlePayloadHash == 0 ||
        !reader.readU32(identity.script.sdkCompatibilityVersion) ||
        !reader.readU32(identity.script.gameSdkPackageVersion) ||
        !reader.readU64(identity.script.hostApiHash) ||
        !reader.readU64(identity.script.programContentHash)) {
        return false;
    }
    identity.bundle.type = static_cast<assets::AssetType>(type);
    return identity.script.valid();
}

bool validTrigger(const RtsScriptTriggerDefinition& trigger) noexcept {
    if (trigger.id == 0 ||
        trigger.id > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    switch (trigger.kind) {
        case RtsScriptTriggerKind::Tick:
            return trigger.intervalTicks != 0 || trigger.once;
        case RtsScriptTriggerKind::HealthAtMost:
        case RtsScriptTriggerKind::VisibleEnemy:
        case RtsScriptTriggerKind::EventType:
            return true;
    }
    return false;
}

bool validDefinition(const RtsEntityBehaviorDefinition& definition) noexcept {
    return definition.entity.valid() && !definition.scriptType.empty() &&
           definition.scriptType.size() <= kMaximumStringBytes &&
           definition.maximumIntentsPerTick != 0;
}

const SnapshotEntity* snapshotEntity(
    const RtsGameSession& session,
    ecs::Entity entity) noexcept {
    const auto& entities = session.simulation().snapshot().entities;
    const auto found = std::lower_bound(
        entities.begin(), entities.end(), entity,
        [](const SnapshotEntity& value, ecs::Entity candidate) {
            return value.entity < candidate;
        });
    return found != entities.end() && found->entity == entity
        ? &*found
        : nullptr;
}

bool relevantEvent(ecs::Entity self, const DomainEvent& event) noexcept {
    return event.entity == self || event.secondary == self;
}

bool appendIntent(
    const std::shared_ptr<detail::RtsEntityBehaviorBindingState>& state,
    detail::RtsEntityBehaviorIntent intent) {
    if (!state || !state->view || !state->intents || !state->self.valid() ||
        state->maximumIntents == 0 ||
        state->intents->size() >= state->maximumIntents) {
        return false;
    }
    intent.ordinal = static_cast<std::uint32_t>(state->intents->size() + 1u);
    state->intents->push_back(std::move(intent));
    return true;
}

class BindingScope final {
public:
    BindingScope(
        const std::shared_ptr<detail::RtsEntityBehaviorBindingState>& state,
        const RtsScriptReadView& view,
        std::vector<detail::RtsEntityBehaviorIntent>& intents,
        std::size_t maximumIntents,
        ecs::Entity self,
        std::uint32_t teamId,
        std::uint64_t completedTick,
        std::uint64_t targetTick)
        : state_(state) {
        state_->view = &view;
        state_->event = nullptr;
        state_->trigger = nullptr;
        state_->intents = &intents;
        state_->maximumIntents = maximumIntents;
        state_->self = self;
        state_->teamId = teamId;
        state_->completedTick = completedTick;
        state_->targetTick = targetTick;
    }

    ~BindingScope() {
        state_->view = nullptr;
        state_->event = nullptr;
        state_->trigger = nullptr;
        state_->intents = nullptr;
        state_->maximumIntents = 0;
        state_->self = {};
        state_->teamId = 0;
        state_->completedTick = 0;
        state_->targetTick = 0;
    }

private:
    std::shared_ptr<detail::RtsEntityBehaviorBindingState> state_;
};

class EventScope final {
public:
    EventScope(
        const std::shared_ptr<detail::RtsEntityBehaviorBindingState>& state,
        const DomainEvent* event)
        : state_(state) {
        state_->event = event;
    }
    ~EventScope() { state_->event = nullptr; }
private:
    std::shared_ptr<detail::RtsEntityBehaviorBindingState> state_;
};

class TriggerScope final {
public:
    TriggerScope(
        const std::shared_ptr<detail::RtsEntityBehaviorBindingState>& state,
        const RtsScriptTriggerDefinition* trigger)
        : state_(state) {
        state_->trigger = trigger;
    }
    ~TriggerScope() { state_->trigger = nullptr; }
private:
    std::shared_ptr<detail::RtsEntityBehaviorBindingState> state_;
};

} // namespace

RtsEntityBehaviorApi::RtsEntityBehaviorApi(RtsScriptApi& baseApi)
    : baseApi_(baseApi),
      state_(std::make_shared<detail::RtsEntityBehaviorBindingState>()) {
    auto& api = baseApi_.mutableGameApi();
    const auto state = state_;

    (void)api.function("Engine.RtsBehavior", "Self", [state]() -> std::int64_t {
        return packScriptEntity(state->self);
    });
    (void)api.function("Engine.RtsBehavior", "Team", [state]() -> int {
        return static_cast<int>(state->teamId);
    });
    (void)api.function("Engine.RtsBehavior", "CompletedTick", [state]() -> std::int64_t {
        return static_cast<std::int64_t>(state->completedTick);
    });
    (void)api.function("Engine.RtsBehavior", "TargetTick", [state]() -> std::int64_t {
        return static_cast<std::int64_t>(state->targetTick);
    });
    (void)api.function("Engine.RtsBehavior", "EventType", [state]() -> int {
        return state->event ? static_cast<int>(state->event->type) : -1;
    });
    (void)api.function("Engine.RtsBehavior", "EventEntity", [state]() -> std::int64_t {
        return state->event ? packScriptEntity(state->event->entity) : 0;
    });
    (void)api.function("Engine.RtsBehavior", "EventSecondary", [state]() -> std::int64_t {
        return state->event ? packScriptEntity(state->event->secondary) : 0;
    });
    (void)api.function("Engine.RtsBehavior", "EventValue", [state]() -> int {
        return state->event ? state->event->value : 0;
    });
    (void)api.function("Engine.RtsBehavior", "EventReason", [state]() -> int {
        return state->event ? static_cast<int>(state->event->reason) : 0;
    });
    (void)api.function("Engine.RtsBehavior", "TriggerId", [state]() -> std::int64_t {
        return state->trigger
            ? static_cast<std::int64_t>(state->trigger->id)
            : 0;
    });
    (void)api.function("Engine.RtsBehavior", "TriggerKind", [state]() -> int {
        return state->trigger ? static_cast<int>(state->trigger->kind) : -1;
    });
    (void)api.function("Engine.RtsBehavior", "TriggerValue", [state]() -> std::int64_t {
        if (!state->trigger) return 0;
        if (state->trigger->kind == RtsScriptTriggerKind::HealthAtMost) {
            return static_cast<std::int64_t>(state->trigger->threshold);
        }
        return static_cast<std::int64_t>(state->trigger->firstTick);
    });
    (void)api.function("Engine.RtsBehavior", "IsAlive", [state](std::int64_t id) {
        return state->view && state->view->entity(id) != nullptr;
    });
    (void)api.function("Engine.RtsBehavior", "EntityX", [state](std::int64_t id) -> int {
        const auto* value = state->view ? state->view->entity(id) : nullptr;
        return value ? value->x : 0;
    });
    (void)api.function("Engine.RtsBehavior", "EntityY", [state](std::int64_t id) -> int {
        const auto* value = state->view ? state->view->entity(id) : nullptr;
        return value ? value->y : 0;
    });
    (void)api.function("Engine.RtsBehavior", "Health", [state](std::int64_t id) -> int {
        const auto* value = state->view ? state->view->entity(id) : nullptr;
        return value ? value->health : 0;
    });
    (void)api.function("Engine.RtsBehavior", "FindNearestVisibleEnemy", [state]() -> std::int64_t {
        return state->view
            ? state->view->findNearestVisibleEnemy(packScriptEntity(state->self))
            : 0;
    });
    (void)api.function("Engine.RtsBehavior", "Move", [state](int x, int y) {
        if (!state->view || !state->view->containsCell(x, y)) return false;
        detail::RtsEntityBehaviorIntent intent;
        intent.type = CommandType::Move;
        intent.x = x;
        intent.y = y;
        return appendIntent(state, std::move(intent));
    });
    (void)api.function("Engine.RtsBehavior", "AttackMove", [state](int x, int y) {
        if (!state->view || !state->view->containsCell(x, y)) return false;
        detail::RtsEntityBehaviorIntent intent;
        intent.type = CommandType::AttackMove;
        intent.x = x;
        intent.y = y;
        return appendIntent(state, std::move(intent));
    });
    (void)api.function("Engine.RtsBehavior", "Stop", [state]() {
        detail::RtsEntityBehaviorIntent intent;
        intent.type = CommandType::Stop;
        return appendIntent(state, std::move(intent));
    });
    (void)api.function("Engine.RtsBehavior", "Attack", [state](std::int64_t targetId) {
        if (!state->view) return false;
        const auto* target = state->view->entity(targetId);
        if (!target || !target->visible || !target->hostile) return false;
        detail::RtsEntityBehaviorIntent intent;
        intent.type = CommandType::Attack;
        intent.target = unpackScriptEntity(targetId);
        return appendIntent(state, std::move(intent));
    });
}

RtsEntityBehaviorApi::~RtsEntityBehaviorApi() = default;

struct RtsEntityBehaviorRuntime::Impl final {
    struct TriggerState final {
        RtsScriptTriggerDefinition definition;
        bool fired{};
        bool latched{};
        std::uint64_t nextTick{};
    };

    struct Instance final {
        RtsEntityBehaviorDefinition definition;
        std::uint32_t teamId{};
        realscript::game::ScriptObject object;
        std::optional<realscript::game::ScriptMethod> onCreate;
        std::optional<realscript::game::ScriptMethod> onStart;
        std::optional<realscript::game::ScriptMethod> onEvent;
        std::optional<realscript::game::ScriptMethod> onTrigger;
        std::optional<realscript::game::ScriptMethod> onThink;
        std::optional<realscript::game::ScriptMethod> onDestroy;
        std::vector<TriggerState> triggers;
        bool enabled{true};
        bool started{};
    };

    struct EncodedInstance final {
        RtsEntityBehaviorDefinition definition;
        std::uint32_t teamId{};
        bool enabled{};
        bool started{};
        std::vector<std::uint8_t> objectState;
        std::vector<TriggerState> triggers;
    };

    RtsGameSession& session;
    std::shared_ptr<rts::scripting::ScriptProgram> program;
    RtsEntityBehaviorApi& api;
    std::unique_ptr<realscript::game::ScriptRuntime> runtime;
    std::vector<Instance> instances;
    std::map<std::uint32_t, std::uint32_t> nextSequenceByTeam;
    std::vector<RtsEntityBehaviorCommandOutcome> outcomes;
    std::vector<RtsEntityBehaviorError> errors;
    RtsEntityBehaviorReport report;

    Impl(
        RtsGameSession& value,
        std::shared_ptr<rts::scripting::ScriptProgram> scriptProgram,
        RtsEntityBehaviorApi& behaviorApi)
        : session(value), program(std::move(scriptProgram)), api(behaviorApi) {
        if (!program || !program->valid()) return;
        if (program->identity().script.hostApiHash !=
            realscript::game::stableGameApiHash(api.gameApi())) {
            return;
        }
        runtime = std::make_unique<realscript::game::ScriptRuntime>(
            program->createObjectRuntime());
    }

    void recordError(
        std::uint64_t tick,
        ecs::Entity entity,
        std::string callback,
        RuntimeError error) {
        errors.push_back({tick, entity, std::move(callback), std::move(error)});
    }

    bool invoke(
        Instance& instance,
        const std::optional<realscript::game::ScriptMethod>& method,
        const char* callback,
        std::uint64_t tick) {
        if (!method) return true;
        ++report.callbacks;
        const auto result = runtime->invoke(
            instance.object,
            *method,
            {},
            executionOptions(instance.definition.executionPolicy));
        if (result.succeeded) return true;
        recordError(tick, instance.definition.entity, callback, result.error);
        return false;
    }

    std::vector<Instance>::iterator find(ecs::Entity entity) {
        return std::lower_bound(
            instances.begin(), instances.end(), packScriptEntity(entity),
            [](const Instance& value, ScriptEntityId id) {
                return packScriptEntity(value.definition.entity) < id;
            });
    }

    std::vector<Instance>::const_iterator find(ecs::Entity entity) const {
        return std::lower_bound(
            instances.begin(), instances.end(), packScriptEntity(entity),
            [](const Instance& value, ScriptEntityId id) {
                return packScriptEntity(value.definition.entity) < id;
            });
    }

    bool buildInstance(
        const RtsEntityBehaviorDefinition& definition,
        Instance& output,
        RuntimeError& error,
        bool invokeCreate) {
        if (!runtime || !validDefinition(definition) ||
            !session.simulation().world().alive(definition.entity)) {
            fail(error, realscript::runtime::ErrorCode::InvalidArguments,
                 "invalid RTS entity behavior definition");
            return false;
        }
        const auto* team = session.simulation().world().try_get<Team>(
            definition.entity);
        if (!team || team->id == 0) {
            fail(error, realscript::runtime::ErrorCode::InvalidArguments,
                 "RTS entity behavior requires a non-neutral Team entity");
            return false;
        }
        const auto type = runtime->findType(definition.scriptType);
        if (!type) {
            fail(error, realscript::runtime::ErrorCode::FunctionNotFound,
                 "entity behavior script type was not found: " +
                     definition.scriptType);
            return false;
        }
        auto object = runtime->createObject(*type, {}, error);
        if (!object) return false;

        Instance instance;
        instance.definition = definition;
        instance.teamId = team->id;
        instance.object = std::move(*object);
        instance.onCreate = runtime->findMethod(*type, "OnCreate", 0);
        instance.onStart = runtime->findMethod(*type, "OnStart", 0);
        instance.onEvent = runtime->findMethod(*type, "OnEvent", 0);
        instance.onTrigger = runtime->findMethod(*type, "OnTrigger", 0);
        instance.onThink = runtime->findMethod(*type, "OnThink", 0);
        instance.onDestroy = runtime->findMethod(*type, "OnDestroy", 0);

        if (invokeCreate && instance.onCreate) {
            auto view = RtsScriptReadView::capture(session, instance.teamId);
            std::vector<detail::RtsEntityBehaviorIntent> ignored;
            BindingScope scope(
                api.state_, view, ignored,
                instance.definition.maximumIntentsPerTick,
                instance.definition.entity, instance.teamId,
                session.simulation().snapshot().tick,
                session.simulation().nextExpectedTick());
            if (!invoke(
                    instance, instance.onCreate, "OnCreate",
                    session.simulation().nextExpectedTick())) {
                return false;
            }
        }
        output = std::move(instance);
        return true;
    }

    bool fireTrigger(
        Instance& instance,
        TriggerState& trigger,
        const DomainEvent* event,
        std::uint64_t completedTick) {
        ++report.triggersFired;
        EventScope eventScope(api.state_, event);
        TriggerScope triggerScope(api.state_, &trigger.definition);
        if (!invoke(instance, instance.onTrigger, "OnTrigger", completedTick)) {
            return false;
        }
        trigger.fired = true;
        if (trigger.definition.once) trigger.definition.enabled = false;
        return true;
    }

    bool evaluateStateTrigger(
        Instance& instance,
        TriggerState& trigger,
        const RtsScriptReadView& view,
        std::uint64_t completedTick) {
        if (!trigger.definition.enabled ||
            (trigger.definition.once && trigger.fired) ||
            trigger.definition.kind == RtsScriptTriggerKind::EventType) {
            return true;
        }

        bool condition = false;
        if (trigger.definition.kind == RtsScriptTriggerKind::Tick) {
            condition = completedTick >= trigger.nextTick;
            if (condition && trigger.definition.intervalTicks != 0) {
                do {
                    const auto previous = trigger.nextTick;
                    trigger.nextTick += trigger.definition.intervalTicks;
                    if (trigger.nextTick <= previous) {
                        trigger.definition.enabled = false;
                        break;
                    }
                } while (trigger.nextTick <= completedTick);
            }
        } else if (trigger.definition.kind ==
                   RtsScriptTriggerKind::HealthAtMost) {
            const auto* self = view.entity(packScriptEntity(instance.definition.entity));
            condition = self && self->health <= trigger.definition.threshold;
        } else if (trigger.definition.kind ==
                   RtsScriptTriggerKind::VisibleEnemy) {
            condition = view.findNearestVisibleEnemy(
                packScriptEntity(instance.definition.entity)) != 0;
        }

        if (!condition) {
            trigger.latched = false;
            return true;
        }
        if (trigger.definition.kind != RtsScriptTriggerKind::Tick &&
            trigger.latched) {
            return true;
        }
        trigger.latched = true;
        return fireTrigger(instance, trigger, nullptr, completedTick);
    }

    bool flush(
        Instance& instance,
        const std::vector<detail::RtsEntityBehaviorIntent>& intents,
        std::uint64_t targetTick) {
        auto& next = nextSequenceByTeam[instance.teamId];
        if (next == 0) next = RtsEntityBehaviorRuntime::kFirstBehaviorSequence;
        for (const auto& intent : intents) {
            if (next == std::numeric_limits<std::uint32_t>::max()) {
                RuntimeError error;
                fail(error, realscript::runtime::ErrorCode::ExecutionTerminated,
                     "entity behavior command sequence exhausted");
                recordError(targetTick, instance.definition.entity,
                            "SubmitIntent", std::move(error));
                return false;
            }
            TickCommand command;
            command.targetTick = targetTick;
            command.issuer = instance.teamId;
            command.sequence = next++;
            command.type = intent.type;
            command.subject = instance.definition.entity;
            command.targetX = intent.x;
            command.targetY = intent.y;
            command.targetEntity = intent.target;
            const auto result = session.submitDetailed(command);
            outcomes.push_back({
                targetTick,
                instance.definition.entity,
                instance.teamId,
                command.sequence,
                intent.ordinal,
                intent.type,
                result});
            ++report.intents;
            if (result == SessionCommandResult::Accepted) {
                ++report.accepted;
            } else {
                ++report.rejected;
            }
        }
        return true;
    }
};

RtsEntityBehaviorRuntime::RtsEntityBehaviorRuntime(
    RtsGameSession& session,
    std::shared_ptr<rts::scripting::ScriptProgram> program,
    RtsEntityBehaviorApi& api)
    : impl_(std::make_unique<Impl>(session, std::move(program), api)) {}

RtsEntityBehaviorRuntime::~RtsEntityBehaviorRuntime() = default;
RtsEntityBehaviorRuntime::RtsEntityBehaviorRuntime(
    RtsEntityBehaviorRuntime&&) noexcept = default;
RtsEntityBehaviorRuntime& RtsEntityBehaviorRuntime::operator=(
    RtsEntityBehaviorRuntime&&) noexcept = default;

bool RtsEntityBehaviorRuntime::valid() const noexcept {
    return impl_ && impl_->runtime && impl_->api.gameApi().valid();
}

bool RtsEntityBehaviorRuntime::attach(
    RtsEntityBehaviorDefinition definition) {
    if (!valid() || !validDefinition(definition)) return false;
    const auto found = impl_->find(definition.entity);
    if (found != impl_->instances.end() &&
        found->definition.entity == definition.entity) {
        return false;
    }
    Impl::Instance instance;
    RuntimeError error;
    if (!impl_->buildInstance(definition, instance, error, true)) {
        impl_->recordError(
            impl_->session.simulation().nextExpectedTick(),
            definition.entity, "Attach", std::move(error));
        return false;
    }
    impl_->nextSequenceByTeam.emplace(
        instance.teamId, kFirstBehaviorSequence);
    impl_->instances.insert(found, std::move(instance));
    return true;
}

bool RtsEntityBehaviorRuntime::detach(
    ecs::Entity entity,
    bool invokeDestroy) {
    if (!impl_) return false;
    const auto found = impl_->find(entity);
    if (found == impl_->instances.end() || found->definition.entity != entity) {
        return false;
    }
    if (invokeDestroy && valid() && found->onDestroy) {
        auto view = RtsScriptReadView::capture(impl_->session, found->teamId);
        std::vector<detail::RtsEntityBehaviorIntent> ignored;
        BindingScope scope(
            impl_->api.state_, view, ignored,
            found->definition.maximumIntentsPerTick,
            found->definition.entity, found->teamId,
            impl_->session.simulation().snapshot().tick,
            impl_->session.simulation().nextExpectedTick());
        (void)impl_->invoke(
            *found, found->onDestroy, "OnDestroy",
            impl_->session.simulation().snapshot().tick);
    }
    impl_->instances.erase(found);
    return true;
}

bool RtsEntityBehaviorRuntime::setEnabled(
    ecs::Entity entity,
    bool enabled) {
    if (!impl_) return false;
    const auto found = impl_->find(entity);
    if (found == impl_->instances.end() || found->definition.entity != entity) {
        return false;
    }
    found->enabled = enabled;
    return true;
}

bool RtsEntityBehaviorRuntime::addTrigger(
    ecs::Entity entity,
    RtsScriptTriggerDefinition trigger) {
    if (!impl_ || !validTrigger(trigger)) return false;
    const auto found = impl_->find(entity);
    if (found == impl_->instances.end() || found->definition.entity != entity) {
        return false;
    }
    const auto position = std::lower_bound(
        found->triggers.begin(), found->triggers.end(), trigger.id,
        [](const Impl::TriggerState& value, std::uint64_t id) {
            return value.definition.id < id;
        });
    if (position != found->triggers.end() &&
        position->definition.id == trigger.id) {
        return false;
    }
    Impl::TriggerState state;
    state.definition = std::move(trigger);
    state.nextTick = state.definition.firstTick;
    found->triggers.insert(position, std::move(state));
    return true;
}

bool RtsEntityBehaviorRuntime::removeTrigger(
    ecs::Entity entity,
    std::uint64_t triggerId) {
    if (!impl_ || triggerId == 0) return false;
    const auto found = impl_->find(entity);
    if (found == impl_->instances.end() || found->definition.entity != entity) {
        return false;
    }
    const auto position = std::lower_bound(
        found->triggers.begin(), found->triggers.end(), triggerId,
        [](const Impl::TriggerState& value, std::uint64_t id) {
            return value.definition.id < id;
        });
    if (position == found->triggers.end() ||
        position->definition.id != triggerId) {
        return false;
    }
    found->triggers.erase(position);
    return true;
}

RtsEntityBehaviorTickResult RtsEntityBehaviorRuntime::processCompletedTick(
    std::uint64_t completedTick) {
    if (!valid()) return RtsEntityBehaviorTickResult::InvalidRuntime;
    if (completedTick == std::numeric_limits<std::uint64_t>::max() ||
        impl_->session.simulation().snapshot().tick != completedTick ||
        impl_->session.simulation().nextExpectedTick() != completedTick + 1u) {
        return RtsEntityBehaviorTickResult::TimelineMismatch;
    }

    impl_->outcomes.clear();
    impl_->report = {};
    impl_->report.completedTick = completedTick;
    impl_->report.targetTick = completedTick + 1u;
    std::vector<ecs::Entity> detached;
    const auto& events = impl_->session.simulation().events();

    for (auto& instance : impl_->instances) {
        if (!impl_->session.simulation().world().alive(
                instance.definition.entity)) {
            auto view = RtsScriptReadView::capture(
                impl_->session, instance.teamId);
            std::vector<detail::RtsEntityBehaviorIntent> ignored;
            BindingScope scope(
                impl_->api.state_, view, ignored,
                instance.definition.maximumIntentsPerTick,
                instance.definition.entity, instance.teamId,
                completedTick, impl_->report.targetTick);
            (void)impl_->invoke(
                instance, instance.onDestroy, "OnDestroy", completedTick);
            detached.push_back(instance.definition.entity);
            ++impl_->report.detached;
            continue;
        }
        if (!instance.enabled) continue;

        auto view = RtsScriptReadView::capture(
            impl_->session, instance.teamId);
        std::vector<detail::RtsEntityBehaviorIntent> intents;
        intents.reserve(std::min<std::size_t>(
            instance.definition.maximumIntentsPerTick, 32u));
        BindingScope scope(
            impl_->api.state_, view, intents,
            instance.definition.maximumIntentsPerTick,
            instance.definition.entity, instance.teamId,
            completedTick, impl_->report.targetTick);

        bool succeeded = true;
        if (!instance.started) {
            succeeded = impl_->invoke(
                instance, instance.onStart, "OnStart", completedTick);
            if (succeeded) instance.started = true;
        }

        if (succeeded) {
            for (const auto& event : events) {
                if (!relevantEvent(instance.definition.entity, event)) continue;
                EventScope eventScope(impl_->api.state_, &event);
                if (!impl_->invoke(
                        instance, instance.onEvent, "OnEvent", completedTick)) {
                    succeeded = false;
                    break;
                }
                for (auto& trigger : instance.triggers) {
                    if (!trigger.definition.enabled ||
                        (trigger.definition.once && trigger.fired) ||
                        trigger.definition.kind !=
                            RtsScriptTriggerKind::EventType ||
                        trigger.definition.eventType != event.type) {
                        continue;
                    }
                    if (!impl_->fireTrigger(
                            instance, trigger, &event, completedTick)) {
                        succeeded = false;
                        break;
                    }
                }
                if (!succeeded) break;
            }
        }

        if (succeeded) {
            for (auto& trigger : instance.triggers) {
                if (!impl_->evaluateStateTrigger(
                        instance, trigger, view, completedTick)) {
                    succeeded = false;
                    break;
                }
            }
        }

        if (succeeded && instance.definition.thinkIntervalTicks != 0 &&
            impl_->report.targetTick %
                    instance.definition.thinkIntervalTicks == 0) {
            succeeded = impl_->invoke(
                instance, instance.onThink, "OnThink", completedTick);
        }

        if (succeeded && !impl_->flush(
                instance, intents, impl_->report.targetTick)) {
            succeeded = false;
        }
        (void)succeeded;
    }

    for (const auto entity : detached) {
        const auto found = impl_->find(entity);
        if (found != impl_->instances.end() &&
            found->definition.entity == entity) {
            impl_->instances.erase(found);
        }
    }

    return impl_->report.callbacks == 0 && impl_->report.triggersFired == 0
        ? RtsEntityBehaviorTickResult::NoCallbacks
        : RtsEntityBehaviorTickResult::Processed;
}

const rts::scripting::ScriptProgramIdentity*
RtsEntityBehaviorRuntime::programIdentity() const noexcept {
    return valid() ? &impl_->program->identity() : nullptr;
}

bool RtsEntityBehaviorRuntime::authoritativeHash(
    std::uint64_t& output,
    RuntimeError& error) const {
    if (!valid() || impl_->instances.size() > kMaximumBehaviors ||
        impl_->nextSequenceByTeam.size() > kMaximumSequences) {
        fail(error, realscript::runtime::ErrorCode::InvalidProgram,
             "entity behavior runtime is not hash-ready");
        return false;
    }

    foundation::CanonicalHash hash;
    hash.WriteU16(kStateVersion);
    const auto& identity = impl_->program->identity();
    hash.WriteU16(static_cast<std::uint16_t>(identity.bundle.type));
    hash.WriteU64(identity.bundle.id);
    hash.WriteU64(identity.bundlePayloadHash);
    hash.WriteU32(identity.script.sdkCompatibilityVersion);
    hash.WriteU32(identity.script.gameSdkPackageVersion);
    hash.WriteU64(identity.script.hostApiHash);
    hash.WriteU64(identity.script.programContentHash);

    hash.WriteU32(static_cast<std::uint32_t>(
        impl_->nextSequenceByTeam.size()));
    for (const auto& entry : impl_->nextSequenceByTeam) {
        hash.WriteU32(entry.first);
        hash.WriteU32(entry.second);
    }

    hash.WriteU32(static_cast<std::uint32_t>(impl_->instances.size()));
    for (const auto& instance : impl_->instances) {
        const auto state = realscript::game::snapshotScriptObject(
            *impl_->runtime, instance.object, error);
        if (!state) return false;
        hash.WriteU32(instance.definition.entity.index);
        hash.WriteU32(instance.definition.entity.generation);
        hash.WriteU32(instance.teamId);
        hash.WriteString(instance.definition.scriptType);
        hash.WriteU32(instance.definition.thinkIntervalTicks);
        hash.WriteU64(static_cast<std::uint64_t>(
            instance.definition.maximumIntentsPerTick));
        hash.WriteU64(instance.definition.executionPolicy.instructionBudget);
        hash.WriteU64(static_cast<std::uint64_t>(
            instance.definition.executionPolicy.recursionLimit));
        hash.WriteU64(static_cast<std::uint64_t>(
            instance.definition.executionPolicy.gcWorkBudget));
        hash.WriteBool(instance.definition.executionPolicy.strictDeterminism);
        hash.WriteBool(instance.enabled);
        hash.WriteBool(instance.started);
        hash.WriteU64(state->canonicalHash());
        hash.WriteU32(static_cast<std::uint32_t>(instance.triggers.size()));
        for (const auto& trigger : instance.triggers) {
            hash.WriteU64(trigger.definition.id);
            hash.WriteU8(static_cast<std::uint8_t>(trigger.definition.kind));
            hash.WriteU64(trigger.definition.firstTick);
            hash.WriteU32(trigger.definition.intervalTicks);
            hash.WriteI32(trigger.definition.threshold);
            hash.WriteU8(static_cast<std::uint8_t>(
                trigger.definition.eventType));
            hash.WriteBool(trigger.definition.once);
            hash.WriteBool(trigger.definition.enabled);
            hash.WriteBool(trigger.fired);
            hash.WriteBool(trigger.latched);
            hash.WriteU64(trigger.nextTick);
        }
    }
    output = hash.Value();
    return true;
}

std::vector<std::uint8_t> RtsEntityBehaviorRuntime::encodeState(
    RuntimeError& error) const {
    std::uint64_t stateHash = 0;
    if (!authoritativeHash(stateHash, error)) return {};

    foundation::BinaryWriter writer;
    writer.writeU32(kStateMagic);
    writer.writeU16(kStateVersion);
    writeIdentity(writer, impl_->program->identity());
    writer.writeU64(stateHash);
    writer.writeU32(static_cast<std::uint32_t>(
        impl_->nextSequenceByTeam.size()));
    for (const auto& entry : impl_->nextSequenceByTeam) {
        writer.writeU32(entry.first);
        writer.writeU32(entry.second);
    }
    writer.writeU32(static_cast<std::uint32_t>(impl_->instances.size()));
    for (const auto& instance : impl_->instances) {
        const auto state = realscript::game::snapshotScriptObject(
            *impl_->runtime, instance.object, error);
        if (!state) return {};
        const auto objectBytes = realscript::game::encodeScriptObjectState(
            *state, error);
        if (objectBytes.empty() || objectBytes.size() > kMaximumObjectBytes) {
            if (error.code == realscript::runtime::ErrorCode::None) {
                fail(error, realscript::runtime::ErrorCode::InvalidProgram,
                     "entity behavior object state exceeds the archive limit");
            }
            return {};
        }
        writer.writeU32(instance.definition.entity.index);
        writer.writeU32(instance.definition.entity.generation);
        writer.writeU32(instance.teamId);
        writer.writeString(instance.definition.scriptType);
        writer.writeU32(instance.definition.thinkIntervalTicks);
        writer.writeU64(static_cast<std::uint64_t>(
            instance.definition.maximumIntentsPerTick));
        writer.writeU64(instance.definition.executionPolicy.instructionBudget);
        writer.writeU64(static_cast<std::uint64_t>(
            instance.definition.executionPolicy.recursionLimit));
        writer.writeU64(static_cast<std::uint64_t>(
            instance.definition.executionPolicy.gcWorkBudget));
        writer.writeBool(instance.definition.executionPolicy.strictDeterminism);
        writer.writeBool(instance.enabled);
        writer.writeBool(instance.started);
        writer.writeU32(static_cast<std::uint32_t>(objectBytes.size()));
        writer.writeBytes(objectBytes);
        writer.writeU32(static_cast<std::uint32_t>(instance.triggers.size()));
        for (const auto& trigger : instance.triggers) {
            writer.writeU64(trigger.definition.id);
            writer.writeU8(static_cast<std::uint8_t>(trigger.definition.kind));
            writer.writeU64(trigger.definition.firstTick);
            writer.writeU32(trigger.definition.intervalTicks);
            writer.writeI32(trigger.definition.threshold);
            writer.writeU8(static_cast<std::uint8_t>(
                trigger.definition.eventType));
            writer.writeBool(trigger.definition.once);
            writer.writeBool(trigger.definition.enabled);
            writer.writeBool(trigger.fired);
            writer.writeBool(trigger.latched);
            writer.writeU64(trigger.nextTick);
        }
    }
    return writer.take();
}

bool RtsEntityBehaviorRuntime::restoreEncodedState(
    const std::vector<std::uint8_t>& bytes,
    RuntimeError& error) {
    if (!valid()) {
        fail(error, realscript::runtime::ErrorCode::InvalidProgram,
             "entity behavior runtime is unavailable");
        return false;
    }

    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint64_t storedHash = 0;
    rts::scripting::ScriptProgramIdentity identity;
    std::uint32_t sequenceCount = 0;
    std::uint32_t instanceCount = 0;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        magic != kStateMagic || version != kStateVersion ||
        !readIdentity(reader, identity) ||
        !sameIdentity(identity, impl_->program->identity()) ||
        !reader.readU64(storedHash) || storedHash == 0 ||
        !reader.readU32(sequenceCount) ||
        sequenceCount > kMaximumSequences) {
        fail(error, realscript::runtime::ErrorCode::InvalidProgram,
             "invalid or incompatible entity behavior archive header");
        return false;
    }

    std::map<std::uint32_t, std::uint32_t> sequences;
    std::uint32_t previousTeam = 0;
    for (std::uint32_t index = 0; index < sequenceCount; ++index) {
        std::uint32_t teamId = 0;
        std::uint32_t sequence = 0;
        if (!reader.readU32(teamId) || teamId == 0 ||
            !reader.readU32(sequence) ||
            sequence < kFirstBehaviorSequence ||
            (previousTeam != 0 && teamId <= previousTeam)) {
            fail(error, realscript::runtime::ErrorCode::InvalidProgram,
                 "invalid entity behavior sequence state");
            return false;
        }
        sequences.emplace(teamId, sequence);
        previousTeam = teamId;
    }

    if (!reader.readU32(instanceCount) ||
        instanceCount > kMaximumBehaviors) {
        fail(error, realscript::runtime::ErrorCode::InvalidProgram,
             "invalid entity behavior instance count");
        return false;
    }

    std::vector<Impl::EncodedInstance> encoded;
    encoded.reserve(instanceCount);
    ScriptEntityId previousEntity = 0;
    for (std::uint32_t index = 0; index < instanceCount; ++index) {
        Impl::EncodedInstance entry;
        std::uint64_t maximumIntents = 0;
        std::uint64_t recursionLimit = 0;
        std::uint64_t gcWorkBudget = 0;
        std::uint32_t objectSize = 0;
        std::uint32_t triggerCount = 0;
        if (!reader.readU32(entry.definition.entity.index) ||
            !reader.readU32(entry.definition.entity.generation) ||
            !entry.definition.entity.valid() ||
            !reader.readU32(entry.teamId) || entry.teamId == 0 ||
            !reader.readString(
                entry.definition.scriptType, kMaximumStringBytes) ||
            !reader.readU32(entry.definition.thinkIntervalTicks) ||
            !reader.readU64(maximumIntents) || maximumIntents == 0 ||
            maximumIntents > std::numeric_limits<std::size_t>::max() ||
            !reader.readU64(
                entry.definition.executionPolicy.instructionBudget) ||
            !reader.readU64(recursionLimit) ||
            recursionLimit > std::numeric_limits<std::size_t>::max() ||
            !reader.readU64(gcWorkBudget) ||
            gcWorkBudget > std::numeric_limits<std::size_t>::max() ||
            !reader.readBool(
                entry.definition.executionPolicy.strictDeterminism) ||
            !reader.readBool(entry.enabled) ||
            !reader.readBool(entry.started) ||
            !reader.readU32(objectSize) || objectSize == 0 ||
            objectSize > kMaximumObjectBytes ||
            !reader.readBytes(
                objectSize, entry.objectState, kMaximumObjectBytes) ||
            !reader.readU32(triggerCount) ||
            triggerCount > kMaximumTriggersPerBehavior) {
            fail(error, realscript::runtime::ErrorCode::InvalidProgram,
                 "invalid entity behavior instance state");
            return false;
        }
        entry.definition.maximumIntentsPerTick =
            static_cast<std::size_t>(maximumIntents);
        entry.definition.executionPolicy.recursionLimit =
            static_cast<std::size_t>(recursionLimit);
        entry.definition.executionPolicy.gcWorkBudget =
            static_cast<std::size_t>(gcWorkBudget);
        const auto packed = packScriptEntity(entry.definition.entity);
        if (!validDefinition(entry.definition) ||
            (previousEntity != 0 && packed <= previousEntity)) {
            fail(error, realscript::runtime::ErrorCode::InvalidProgram,
                 "entity behavior instances are not canonically ordered");
            return false;
        }
        previousEntity = packed;

        entry.triggers.reserve(triggerCount);
        std::uint64_t previousTrigger = 0;
        for (std::uint32_t triggerIndex = 0;
             triggerIndex < triggerCount; ++triggerIndex) {
            Impl::TriggerState trigger;
            std::uint8_t kind = 0;
            std::uint8_t eventType = 0;
            if (!reader.readU64(trigger.definition.id) ||
                !reader.readU8(kind) ||
                kind > static_cast<std::uint8_t>(
                    RtsScriptTriggerKind::EventType) ||
                !reader.readU64(trigger.definition.firstTick) ||
                !reader.readU32(trigger.definition.intervalTicks) ||
                !reader.readI32(trigger.definition.threshold) ||
                !reader.readU8(eventType) ||
                eventType > static_cast<std::uint8_t>(
                    DomainEventType::ResearchCompleted) ||
                !reader.readBool(trigger.definition.once) ||
                !reader.readBool(trigger.definition.enabled) ||
                !reader.readBool(trigger.fired) ||
                !reader.readBool(trigger.latched) ||
                !reader.readU64(trigger.nextTick)) {
                fail(error, realscript::runtime::ErrorCode::InvalidProgram,
                     "invalid entity behavior trigger state");
                return false;
            }
            trigger.definition.kind =
                static_cast<RtsScriptTriggerKind>(kind);
            trigger.definition.eventType =
                static_cast<DomainEventType>(eventType);
            if (!validTrigger(trigger.definition) ||
                (previousTrigger != 0 &&
                 trigger.definition.id <= previousTrigger)) {
                fail(error, realscript::runtime::ErrorCode::InvalidProgram,
                     "entity behavior triggers are not canonically ordered");
                return false;
            }
            previousTrigger = trigger.definition.id;
            entry.triggers.push_back(std::move(trigger));
        }
        encoded.push_back(std::move(entry));
    }
    if (!reader.atEnd()) {
        fail(error, realscript::runtime::ErrorCode::InvalidProgram,
             "entity behavior archive has trailing bytes");
        return false;
    }

    std::vector<Impl::Instance> instances;
    instances.reserve(encoded.size());
    for (const auto& entry : encoded) {
        const auto* team = impl_->session.simulation().world().try_get<Team>(
            entry.definition.entity);
        if (!team || team->id != entry.teamId) {
            fail(error, realscript::runtime::ErrorCode::InvalidProgram,
                 "entity behavior owner does not match the restored world");
            return false;
        }
        Impl::Instance instance;
        if (!impl_->buildInstance(
                entry.definition, instance, error, false)) {
            return false;
        }
        const auto objectState = realscript::game::decodeScriptObjectState(
            entry.objectState, error);
        if (!objectState || !realscript::game::restoreScriptObject(
                *impl_->runtime, instance.object, *objectState, error)) {
            return false;
        }
        instance.enabled = entry.enabled;
        instance.started = entry.started;
        instance.triggers = entry.triggers;
        instances.push_back(std::move(instance));
    }

    auto previousInstances = std::move(impl_->instances);
    auto previousSequences = std::move(impl_->nextSequenceByTeam);
    impl_->instances = std::move(instances);
    impl_->nextSequenceByTeam = std::move(sequences);
    impl_->outcomes.clear();
    impl_->errors.clear();
    impl_->report = {};

    std::uint64_t restoredHash = 0;
    if (!authoritativeHash(restoredHash, error) || restoredHash != storedHash) {
        impl_->instances = std::move(previousInstances);
        impl_->nextSequenceByTeam = std::move(previousSequences);
        if (error.code == realscript::runtime::ErrorCode::None) {
            fail(error, realscript::runtime::ErrorCode::InvalidProgram,
                 "entity behavior authoritative hash mismatch");
        }
        return false;
    }
    return true;
}

std::optional<Value> RtsEntityBehaviorRuntime::member(
    ecs::Entity entity,
    const std::string& name,
    RuntimeError& error) const {
    if (!valid()) return std::nullopt;
    const auto found = impl_->find(entity);
    if (found == impl_->instances.end() || found->definition.entity != entity) {
        fail(error, realscript::runtime::ErrorCode::InvalidArguments,
             "entity behavior is not attached");
        return std::nullopt;
    }
    return impl_->runtime->getMember(found->object, name, error);
}

const RtsEntityBehaviorReport&
RtsEntityBehaviorRuntime::lastReport() const noexcept {
    return impl_->report;
}

const std::vector<RtsEntityBehaviorCommandOutcome>&
RtsEntityBehaviorRuntime::outcomes() const noexcept {
    return impl_->outcomes;
}

const std::vector<RtsEntityBehaviorError>&
RtsEntityBehaviorRuntime::errors() const noexcept {
    return impl_->errors;
}

void RtsEntityBehaviorRuntime::clearErrors() {
    if (impl_) impl_->errors.clear();
}

} // namespace rts::gameplay::scripting
