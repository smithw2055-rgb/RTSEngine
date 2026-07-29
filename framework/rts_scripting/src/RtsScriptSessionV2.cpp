#include <RTSEngine/RtsScripting/RtsScriptSession.h>

#include <realscript/game/GameProductization.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace rts::gameplay::scripting {
namespace detail {

struct RtsScriptIntent final {
    CommandType type{CommandType::Move};
    ecs::Entity subject{};
    std::int32_t targetX{};
    std::int32_t targetY{};
    std::uint32_t definitionId{};
    ecs::Entity targetEntity{};
    std::uint32_t ordinal{};
};

struct RtsScriptBindingState final {
    const RtsScriptReadView* view{};
    const DomainEvent* event{};
    std::vector<RtsScriptIntent>* intents{};
    std::size_t maximumIntents{};
    std::uint32_t teamId{};
    std::uint64_t targetTick{};
};

} // namespace detail

namespace {

using realscript::runtime::ExecutionOptions;
using realscript::runtime::RuntimeError;
using realscript::runtime::Value;

constexpr std::uint32_t kStateMagic = 0x33535452u; // RTS3
constexpr std::uint32_t kStateVersion = 1u;
constexpr std::uint32_t kMaximumTeams = 4096u;
constexpr std::uint32_t kMaximumStringBytes = 4096u;
constexpr std::uint32_t kMaximumObjectStateBytes = 16u * 1024u * 1024u;

struct PersistedTeam final {
    RtsTeamScriptDefinition definition;
    std::uint32_t nextSequence{1};
    bool enabled{true};
    bool started{};
    std::vector<std::uint8_t> objectState;
};

RuntimeError adapterError(
    realscript::runtime::ErrorCode code,
    std::string message) {
    RuntimeError error;
    error.code = code;
    error.message = std::move(message);
    return error;
}

void fail(RuntimeError& error, std::string message) {
    error.code = realscript::runtime::ErrorCode::InvalidProgram;
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

bool validDefinition(const RtsTeamScriptDefinition& definition) {
    return definition.teamId != 0 && !definition.scriptType.empty() &&
           definition.scriptType.size() <= kMaximumStringBytes &&
           definition.thinkIntervalTicks != 0 &&
           definition.maximumIntentsPerTick != 0;
}

bool builtInAiOwnsTeam(const RtsGameSession& session, std::uint32_t teamId) {
    const auto& teams = session.ai().teams();
    return std::any_of(
        teams.begin(), teams.end(),
        [teamId](const AiTeamState& value) { return value.teamId == teamId; });
}

bool visibleAt(
    const WorldSnapshot& snapshot,
    std::uint32_t teamId,
    std::int32_t x,
    std::int32_t y) {
    if (x < 0 || y < 0 || x >= snapshot.visibilityWidth ||
        y >= snapshot.visibilityHeight) {
        return false;
    }
    const TeamVisibilitySnapshot* teamVisibility = nullptr;
    for (const auto& item : snapshot.visibility) {
        if (item.teamId == teamId) {
            teamVisibility = &item;
            break;
        }
    }
    if (!teamVisibility) return false;
    const auto index = static_cast<std::size_t>(y) *
                           static_cast<std::size_t>(snapshot.visibilityWidth) +
                       static_cast<std::size_t>(x);
    return index < teamVisibility->current.size() &&
           teamVisibility->current[index] != 0;
}

const RtsScriptEntityView* findEntity(
    const std::vector<RtsScriptEntityView>& entities,
    ScriptEntityId id) noexcept {
    const auto found = std::lower_bound(
        entities.begin(), entities.end(), id,
        [](const RtsScriptEntityView& value, ScriptEntityId candidate) {
            return value.id < candidate;
        });
    return found != entities.end() && found->id == id ? &*found : nullptr;
}

bool eventRelevant(const RtsScriptReadView& view, const DomainEvent& event) {
    if (event.entity.valid()) {
        const auto* entity = view.entity(packScriptEntity(event.entity));
        if (entity && (entity->teamId == view.teamId() || entity->visible)) {
            return true;
        }
    }
    if (event.secondary.valid()) {
        const auto* entity = view.entity(packScriptEntity(event.secondary));
        if (entity && (entity->teamId == view.teamId() || entity->visible)) {
            return true;
        }
    }
    return false;
}

bool validOwnedSubject(
    const detail::RtsScriptBindingState& state,
    ScriptEntityId id,
    const RtsScriptEntityView*& output) {
    output = state.view ? state.view->entity(id) : nullptr;
    return output && output->teamId == state.teamId;
}

bool appendIntent(
    const std::shared_ptr<detail::RtsScriptBindingState>& state,
    detail::RtsScriptIntent intent) {
    if (!state || !state->view || !state->intents ||
        state->maximumIntents == 0 ||
        state->intents->size() >= state->maximumIntents) {
        return false;
    }
    intent.ordinal = static_cast<std::uint32_t>(state->intents->size() + 1u);
    state->intents->push_back(std::move(intent));
    return true;
}

bool queuePointIntent(
    const std::shared_ptr<detail::RtsScriptBindingState>& state,
    CommandType type,
    ScriptEntityId subjectId,
    int x,
    int y) {
    if (!state || !state->view || !state->view->containsCell(x, y)) {
        return false;
    }
    const RtsScriptEntityView* subject = nullptr;
    if (!validOwnedSubject(*state, subjectId, subject)) return false;
    detail::RtsScriptIntent intent;
    intent.type = type;
    intent.subject = unpackScriptEntity(subjectId);
    intent.targetX = x;
    intent.targetY = y;
    return appendIntent(state, std::move(intent));
}

bool queueSimpleIntent(
    const std::shared_ptr<detail::RtsScriptBindingState>& state,
    CommandType type,
    ScriptEntityId subjectId) {
    if (!state) return false;
    const RtsScriptEntityView* subject = nullptr;
    if (!validOwnedSubject(*state, subjectId, subject)) return false;
    detail::RtsScriptIntent intent;
    intent.type = type;
    intent.subject = unpackScriptEntity(subjectId);
    return appendIntent(state, std::move(intent));
}

bool queueTargetIntent(
    const std::shared_ptr<detail::RtsScriptBindingState>& state,
    CommandType type,
    ScriptEntityId subjectId,
    ScriptEntityId targetId) {
    if (!state || !state->view) return false;
    const RtsScriptEntityView* subject = nullptr;
    if (!validOwnedSubject(*state, subjectId, subject)) return false;
    const auto* target = state->view->entity(targetId);
    if (!target) return false;
    if (type == CommandType::Attack && (!target->hostile || !target->visible)) {
        return false;
    }
    if (type == CommandType::Gather &&
        target->kind != SnapshotKind::ResourceNode) {
        return false;
    }
    detail::RtsScriptIntent intent;
    intent.type = type;
    intent.subject = unpackScriptEntity(subjectId);
    intent.targetEntity = unpackScriptEntity(targetId);
    return appendIntent(state, std::move(intent));
}

bool queueDefinitionIntent(
    const std::shared_ptr<detail::RtsScriptBindingState>& state,
    CommandType type,
    ScriptEntityId subjectId,
    int definitionId) {
    if (!state || definitionId <= 0) return false;
    const RtsScriptEntityView* subject = nullptr;
    if (!validOwnedSubject(*state, subjectId, subject) ||
        subject->kind != SnapshotKind::Building) {
        return false;
    }
    detail::RtsScriptIntent intent;
    intent.type = type;
    intent.subject = unpackScriptEntity(subjectId);
    intent.definitionId = static_cast<std::uint32_t>(definitionId);
    return appendIntent(state, std::move(intent));
}

class BindingScope final {
public:
    BindingScope(
        const std::shared_ptr<detail::RtsScriptBindingState>& state,
        const RtsScriptReadView& view,
        std::vector<detail::RtsScriptIntent>& intents,
        std::size_t maximumIntents,
        std::uint32_t teamId,
        std::uint64_t targetTick)
        : state_(state) {
        state_->view = &view;
        state_->event = nullptr;
        state_->intents = &intents;
        state_->maximumIntents = maximumIntents;
        state_->teamId = teamId;
        state_->targetTick = targetTick;
    }

    ~BindingScope() {
        state_->view = nullptr;
        state_->event = nullptr;
        state_->intents = nullptr;
        state_->maximumIntents = 0;
        state_->teamId = 0;
        state_->targetTick = 0;
    }

private:
    std::shared_ptr<detail::RtsScriptBindingState> state_;
};

class EventScope final {
public:
    EventScope(
        const std::shared_ptr<detail::RtsScriptBindingState>& state,
        const DomainEvent& event)
        : state_(state) {
        state_->event = &event;
    }

    ~EventScope() { state_->event = nullptr; }

private:
    std::shared_ptr<detail::RtsScriptBindingState> state_;
};

bool readPersistedTeams(
    const std::vector<std::uint8_t>& bytes,
    std::vector<PersistedTeam>& output,
    RuntimeError& error) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    if (!reader.readU32(magic) || !reader.readU32(version) ||
        !reader.readU32(count) || magic != kStateMagic ||
        version != kStateVersion || count > kMaximumTeams) {
        fail(error, "invalid RTS script session state header");
        return false;
    }

    std::vector<PersistedTeam> candidate;
    candidate.resize(count);
    std::uint32_t previousTeam = 0;
    for (auto& entry : candidate) {
        std::uint64_t maximumIntents = 0;
        std::uint64_t recursionLimit = 0;
        std::uint64_t gcWorkBudget = 0;
        std::uint8_t strict = 0;
        std::uint8_t enabled = 0;
        std::uint8_t started = 0;
        std::uint32_t objectBytes = 0;
        if (!reader.readU32(entry.definition.teamId) ||
            !reader.readString(
                entry.definition.scriptType, kMaximumStringBytes) ||
            !reader.readU32(entry.definition.thinkIntervalTicks) ||
            !reader.readU64(maximumIntents) || maximumIntents == 0 ||
            maximumIntents >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max()) ||
            !reader.readU64(
                entry.definition.executionPolicy.instructionBudget) ||
            !reader.readU64(recursionLimit) ||
            recursionLimit >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max()) ||
            !reader.readU64(gcWorkBudget) ||
            gcWorkBudget >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max()) ||
            !reader.readU8(strict) || strict > 1 ||
            !reader.readU32(entry.nextSequence) || entry.nextSequence == 0 ||
            !reader.readU8(enabled) || enabled > 1 ||
            !reader.readU8(started) || started > 1 ||
            !reader.readU32(objectBytes) || objectBytes == 0 ||
            objectBytes > kMaximumObjectStateBytes ||
            !reader.readBytes(
                objectBytes, entry.objectState, kMaximumObjectStateBytes)) {
            fail(error, "invalid RTS script session state entry");
            return false;
        }
        entry.definition.maximumIntentsPerTick =
            static_cast<std::size_t>(maximumIntents);
        entry.definition.executionPolicy.recursionLimit =
            static_cast<std::size_t>(recursionLimit);
        entry.definition.executionPolicy.gcWorkBudget =
            static_cast<std::size_t>(gcWorkBudget);
        entry.definition.executionPolicy.strictDeterminism = strict != 0;
        entry.enabled = enabled != 0;
        entry.started = started != 0;
        if (!validDefinition(entry.definition) ||
            (previousTeam != 0 && entry.definition.teamId <= previousTeam)) {
            fail(error, "invalid or unsorted RTS script Team state");
            return false;
        }
        previousTeam = entry.definition.teamId;
    }
    if (!reader.atEnd()) {
        fail(error, "RTS script session state has trailing bytes");
        return false;
    }
    output = std::move(candidate);
    return true;
}

} // namespace

ScriptEntityId packScriptEntity(ecs::Entity entity) noexcept {
    if (!entity.valid()) return 0;
    const std::uint64_t bits =
        static_cast<std::uint64_t>(entity.index) |
        (static_cast<std::uint64_t>(entity.generation) << 32u);
    ScriptEntityId value = 0;
    static_assert(sizeof(value) == sizeof(bits), "script entity width changed");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

ecs::Entity unpackScriptEntity(ScriptEntityId value) noexcept {
    if (value == 0) return {};
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return {
        static_cast<std::uint32_t>(bits & 0xffffffffu),
        static_cast<std::uint32_t>(bits >> 32u)};
}

RtsScriptReadView RtsScriptReadView::capture(
    const RtsGameSession& session,
    std::uint32_t teamId) {
    RtsScriptReadView result;
    const auto& snapshot = session.simulation().snapshot();
    result.tick_ = snapshot.tick;
    result.teamId_ = teamId;
    result.usedSupply_ = session.usedSupply(teamId);
    result.supplyCapacity_ = session.supplyCapacity(teamId);
    result.width_ = snapshot.visibilityWidth;
    result.height_ = snapshot.visibilityHeight;

    for (const auto& resource : snapshot.teamResources) {
        if (resource.teamId == teamId) result.resources_.push_back(resource);
    }
    std::sort(
        result.resources_.begin(), result.resources_.end(),
        [](const TeamResourceAccount& first,
           const TeamResourceAccount& second) {
            return first.resourceType < second.resourceType;
        });

    result.entities_.reserve(snapshot.entities.size());
    for (const auto& entity : snapshot.entities) {
        const bool own = entity.teamId == teamId && teamId != 0;
        const bool visible = own || visibleAt(
            snapshot, teamId, entity.x, entity.y);
        if (!own && !visible) continue;

        RtsScriptEntityView view;
        view.id = packScriptEntity(entity.entity);
        view.teamId = entity.teamId;
        view.kind = entity.kind;
        view.x = entity.x;
        view.y = entity.y;
        view.health = entity.healthCurrent;
        view.queuedOrders = entity.queuedOrders;
        view.productionQueueSize = entity.productionQueueSize;
        view.moving = entity.moving;
        view.visible = visible;
        view.hostile = entity.teamId != 0 && teamId != 0 &&
            session.diplomacy().hostile(teamId, entity.teamId);
        result.entities_.push_back(view);
    }
    std::sort(
        result.entities_.begin(), result.entities_.end(),
        [](const RtsScriptEntityView& first,
           const RtsScriptEntityView& second) {
            return first.id < second.id;
        });
    return result;
}

ResourceAmount RtsScriptReadView::resourceAvailable(
    ResourceTypeId resourceType) const noexcept {
    const auto found = std::lower_bound(
        resources_.begin(), resources_.end(), resourceType,
        [](const TeamResourceAccount& value, ResourceTypeId id) {
            return value.resourceType < id;
        });
    return found != resources_.end() && found->resourceType == resourceType
        ? found->available
        : 0;
}

const RtsScriptEntityView* RtsScriptReadView::entity(
    ScriptEntityId id) const noexcept {
    return findEntity(entities_, id);
}

bool RtsScriptReadView::containsCell(
    std::int32_t x,
    std::int32_t y) const noexcept {
    return x >= 0 && y >= 0 && x < width_ && y < height_;
}

ScriptEntityId RtsScriptReadView::findIdleUnit() const noexcept {
    for (const auto& value : entities_) {
        if (value.teamId == teamId_ && value.kind == SnapshotKind::Unit &&
            value.health > 0 && value.idle()) {
            return value.id;
        }
    }
    return 0;
}

ScriptEntityId RtsScriptReadView::findIdleProducer() const noexcept {
    for (const auto& value : entities_) {
        if (value.teamId == teamId_ &&
            value.kind == SnapshotKind::Building &&
            value.productionQueueSize == 0) {
            return value.id;
        }
    }
    return 0;
}

ScriptEntityId RtsScriptReadView::findNearestVisibleEnemy(
    ScriptEntityId from) const noexcept {
    const auto* origin = entity(from);
    if (!origin || origin->teamId != teamId_) return 0;
    ScriptEntityId best = 0;
    std::int64_t bestDistance = std::numeric_limits<std::int64_t>::max();
    for (const auto& candidate : entities_) {
        if (!candidate.hostile || !candidate.visible || candidate.health <= 0) {
            continue;
        }
        const auto dx = static_cast<std::int64_t>(candidate.x) - origin->x;
        const auto dy = static_cast<std::int64_t>(candidate.y) - origin->y;
        const auto distance = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (distance < bestDistance ||
            (distance == bestDistance && (best == 0 || candidate.id < best))) {
            best = candidate.id;
            bestDistance = distance;
        }
    }
    return best;
}

RtsScriptApi::RtsScriptApi()
    : state_(std::make_shared<detail::RtsScriptBindingState>()) {
    const auto state = state_;
    (void)api_.function("Engine.Rts", "CompletedTick", [state]() -> std::int64_t {
        return state->view ? static_cast<std::int64_t>(state->view->tick()) : 0;
    });
    (void)api_.function("Engine.Rts", "TargetTick", [state]() -> std::int64_t {
        return static_cast<std::int64_t>(state->targetTick);
    });
    (void)api_.function("Engine.Rts", "Team", [state]() -> int {
        return static_cast<int>(state->teamId);
    });
    (void)api_.function("Engine.Rts", "EventType", [state]() -> int {
        return state->event ? static_cast<int>(state->event->type) : -1;
    });
    (void)api_.function("Engine.Rts", "EventEntity", [state]() -> std::int64_t {
        return state->event ? packScriptEntity(state->event->entity) : 0;
    });
    (void)api_.function("Engine.Rts", "EventSecondary", [state]() -> std::int64_t {
        return state->event ? packScriptEntity(state->event->secondary) : 0;
    });
    (void)api_.function("Engine.Rts", "EventValue", [state]() -> int {
        return state->event ? state->event->value : 0;
    });
    (void)api_.function("Engine.Rts", "ResourceAvailable", [state](int type) -> std::int64_t {
        return state->view && type > 0
            ? state->view->resourceAvailable(static_cast<ResourceTypeId>(type))
            : 0;
    });
    (void)api_.function("Engine.Rts", "UsedSupply", [state]() -> int {
        return state->view ? static_cast<int>(state->view->usedSupply()) : 0;
    });
    (void)api_.function("Engine.Rts", "SupplyCapacity", [state]() -> int {
        if (!state->view) return 0;
        return state->view->supplyCapacity() >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(state->view->supplyCapacity());
    });
    (void)api_.function("Engine.Rts", "IsAlive", [state](std::int64_t id) {
        return state->view && state->view->entity(id) != nullptr;
    });
    (void)api_.function("Engine.Rts", "EntityTeam", [state](std::int64_t id) -> int {
        const auto* value = state->view ? state->view->entity(id) : nullptr;
        return value ? static_cast<int>(value->teamId) : 0;
    });
    (void)api_.function("Engine.Rts", "EntityX", [state](std::int64_t id) -> int {
        const auto* value = state->view ? state->view->entity(id) : nullptr;
        return value ? value->x : 0;
    });
    (void)api_.function("Engine.Rts", "EntityY", [state](std::int64_t id) -> int {
        const auto* value = state->view ? state->view->entity(id) : nullptr;
        return value ? value->y : 0;
    });
    (void)api_.function("Engine.Rts", "Health", [state](std::int64_t id) -> int {
        const auto* value = state->view ? state->view->entity(id) : nullptr;
        return value ? value->health : 0;
    });
    (void)api_.function("Engine.Rts", "IsIdle", [state](std::int64_t id) {
        const auto* value = state->view ? state->view->entity(id) : nullptr;
        return value && value->teamId == state->teamId && value->idle();
    });
    (void)api_.function("Engine.Rts", "FindIdleUnit", [state]() -> std::int64_t {
        return state->view ? state->view->findIdleUnit() : 0;
    });
    (void)api_.function("Engine.Rts", "FindIdleProducer", [state]() -> std::int64_t {
        return state->view ? state->view->findIdleProducer() : 0;
    });
    (void)api_.function("Engine.Rts", "FindNearestVisibleEnemy", [state](std::int64_t from) -> std::int64_t {
        return state->view ? state->view->findNearestVisibleEnemy(from) : 0;
    });
    (void)api_.function("Engine.Rts", "Move", [state](std::int64_t subject, int x, int y) {
        return queuePointIntent(state, CommandType::Move, subject, x, y);
    });
    (void)api_.function("Engine.Rts", "AttackMove", [state](std::int64_t subject, int x, int y) {
        return queuePointIntent(state, CommandType::AttackMove, subject, x, y);
    });
    (void)api_.function("Engine.Rts", "Stop", [state](std::int64_t subject) {
        return queueSimpleIntent(state, CommandType::Stop, subject);
    });
    (void)api_.function("Engine.Rts", "HoldPosition", [state](std::int64_t subject) {
        return queueSimpleIntent(state, CommandType::HoldPosition, subject);
    });
    (void)api_.function("Engine.Rts", "Attack", [state](std::int64_t subject, std::int64_t target) {
        return queueTargetIntent(state, CommandType::Attack, subject, target);
    });
    (void)api_.function("Engine.Rts", "Gather", [state](std::int64_t subject, std::int64_t target) {
        return queueTargetIntent(state, CommandType::Gather, subject, target);
    });
    (void)api_.function("Engine.Rts", "Train", [state](std::int64_t producer, int definitionId) {
        return queueDefinitionIntent(state, CommandType::Train, producer, definitionId);
    });
    (void)api_.function("Engine.Rts", "Research", [state](std::int64_t facility, int definitionId) {
        return queueDefinitionIntent(state, CommandType::Research, facility, definitionId);
    });
}

RtsScriptApi::~RtsScriptApi() = default;

struct RtsScriptSession::Impl final {
    struct TeamInstance final {
        RtsTeamScriptDefinition definition;
        realscript::game::ScriptObject object;
        std::optional<realscript::game::ScriptMethod> onStart;
        std::optional<realscript::game::ScriptMethod> onEvent;
        realscript::game::ScriptMethod onThink;
        std::uint32_t nextSequence{1};
        bool enabled{true};
        bool started{};
    };

    RtsGameSession& session;
    std::shared_ptr<rts::scripting::ScriptProgram> program;
    RtsScriptApi& api;
    std::unique_ptr<realscript::game::ScriptRuntime> runtime;
    std::vector<TeamInstance> teams;
    std::vector<RtsScriptCommandOutcome> outcomes;
    std::vector<RtsScriptError> errors;
    RtsScriptTickReport report;

    Impl(
        RtsGameSession& value,
        std::shared_ptr<rts::scripting::ScriptProgram> scriptProgram,
        RtsScriptApi& scriptApi)
        : session(value), program(std::move(scriptProgram)), api(scriptApi) {
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
        std::uint32_t teamId,
        std::string callback,
        RuntimeError error) {
        errors.push_back({tick, teamId, std::move(callback), std::move(error)});
    }

    bool invoke(
        TeamInstance& team,
        const realscript::game::ScriptMethod& method,
        const std::vector<Value>& arguments,
        const char* callback,
        std::uint64_t tick) {
        ++report.callbacks;
        const auto result = runtime->invoke(
            team.object,
            method,
            arguments,
            executionOptions(team.definition.executionPolicy));
        if (result.succeeded) return true;
        recordError(tick, team.definition.teamId, callback, result.error);
        return false;
    }

    bool buildTeam(
        RtsTeamScriptDefinition definition,
        bool runCreate,
        const std::vector<std::uint8_t>* objectState,
        std::uint32_t nextSequence,
        bool enabled,
        bool started,
        TeamInstance& output,
        RuntimeError& error) {
        if (!runtime || !validDefinition(definition) || nextSequence == 0 ||
            builtInAiOwnsTeam(session, definition.teamId)) {
            fail(error, "invalid RTS script Team definition");
            return false;
        }
        const auto type = runtime->findType(definition.scriptType);
        if (!type) {
            fail(error, "script Team type was not found: " + definition.scriptType);
            return false;
        }
        auto object = runtime->createObject(*type, {}, error);
        if (!object) return false;
        const auto onThink = runtime->findMethod(*type, "OnThink", 0);
        if (!onThink || !onThink->instance()) {
            fail(error, "Team script requires void OnThink()");
            return false;
        }

        TeamInstance team;
        team.definition = std::move(definition);
        team.object = std::move(*object);
        team.onStart = runtime->findMethod(*type, "OnStart", 0);
        team.onEvent = runtime->findMethod(*type, "OnEvent", 0);
        team.onThink = *onThink;
        team.nextSequence = nextSequence;
        team.enabled = enabled;
        team.started = started;

        if (objectState) {
            const auto decoded = realscript::game::decodeScriptObjectState(
                *objectState, error);
            if (!decoded || !realscript::game::restoreScriptObject(
                    *runtime, team.object, *decoded, error)) {
                return false;
            }
        } else if (runCreate) {
            const auto onCreate = runtime->findMethod(*type, "OnCreate", 1);
            if (onCreate) {
                auto view = RtsScriptReadView::capture(
                    session, team.definition.teamId);
                std::vector<detail::RtsScriptIntent> ignored;
                BindingScope scope(
                    api.state_,
                    view,
                    ignored,
                    team.definition.maximumIntentsPerTick,
                    team.definition.teamId,
                    session.simulation().nextExpectedTick());
                report = {};
                if (!invoke(
                        team,
                        *onCreate,
                        {static_cast<std::int64_t>(team.definition.teamId)},
                        "OnCreate",
                        session.simulation().nextExpectedTick())) {
                    error = errors.back().error;
                    return false;
                }
            }
        }
        output = std::move(team);
        return true;
    }

    void flush(
        TeamInstance& team,
        const std::vector<detail::RtsScriptIntent>& intents,
        std::uint64_t targetTick) {
        for (const auto& intent : intents) {
            if (team.nextSequence == std::numeric_limits<std::uint32_t>::max()) {
                recordError(
                    targetTick,
                    team.definition.teamId,
                    "SubmitIntent",
                    adapterError(
                        realscript::runtime::ErrorCode::ExecutionTerminated,
                        "script command sequence exhausted"));
                break;
            }
            TickCommand command;
            command.targetTick = targetTick;
            command.issuer = team.definition.teamId;
            command.sequence = team.nextSequence++;
            command.type = intent.type;
            command.subject = intent.subject;
            command.targetX = intent.targetX;
            command.targetY = intent.targetY;
            command.definitionId = intent.definitionId;
            command.targetEntity = intent.targetEntity;
            const auto result = session.submitDetailed(command);
            outcomes.push_back({
                targetTick,
                team.definition.teamId,
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
    }
};

RtsScriptSession::RtsScriptSession(
    RtsGameSession& session,
    std::shared_ptr<rts::scripting::ScriptProgram> program,
    RtsScriptApi& api)
    : impl_(std::make_unique<Impl>(session, std::move(program), api)) {}

RtsScriptSession::~RtsScriptSession() = default;
RtsScriptSession::RtsScriptSession(RtsScriptSession&&) noexcept = default;
RtsScriptSession& RtsScriptSession::operator=(RtsScriptSession&&) noexcept = default;

bool RtsScriptSession::valid() const noexcept {
    return impl_ && impl_->runtime != nullptr && impl_->api.gameApi().valid();
}

bool RtsScriptSession::registerTeam(RtsTeamScriptDefinition definition) {
    if (!valid() || !validDefinition(definition)) return false;
    const auto existing = std::lower_bound(
        impl_->teams.begin(), impl_->teams.end(), definition.teamId,
        [](const Impl::TeamInstance& value, std::uint32_t teamId) {
            return value.definition.teamId < teamId;
        });
    if (existing != impl_->teams.end() &&
        existing->definition.teamId == definition.teamId) {
        return false;
    }

    Impl::TeamInstance team;
    RuntimeError error;
    if (!impl_->buildTeam(
            std::move(definition), true, nullptr, 1, true, false, team, error)) {
        impl_->recordError(
            impl_->session.simulation().nextExpectedTick(),
            team.definition.teamId,
            "RegisterTeam",
            std::move(error));
        return false;
    }
    impl_->teams.insert(existing, std::move(team));
    impl_->report = {};
    return true;
}

bool RtsScriptSession::setTeamEnabled(
    std::uint32_t teamId,
    bool enabled) {
    if (!impl_) return false;
    const auto found = std::lower_bound(
        impl_->teams.begin(), impl_->teams.end(), teamId,
        [](const Impl::TeamInstance& value, std::uint32_t id) {
            return value.definition.teamId < id;
        });
    if (found == impl_->teams.end() || found->definition.teamId != teamId) {
        return false;
    }
    found->enabled = enabled;
    return true;
}

RtsScriptTickResult RtsScriptSession::processCompletedTick(
    std::uint64_t completedTick) {
    if (!valid()) return RtsScriptTickResult::InvalidRuntime;
    if (completedTick == std::numeric_limits<std::uint64_t>::max() ||
        impl_->session.simulation().snapshot().tick != completedTick ||
        impl_->session.simulation().nextExpectedTick() != completedTick + 1u) {
        return RtsScriptTickResult::TimelineMismatch;
    }

    impl_->outcomes.clear();
    impl_->report = {};
    impl_->report.completedTick = completedTick;
    impl_->report.targetTick = completedTick + 1u;
    const auto& events = impl_->session.simulation().events();

    for (auto& team : impl_->teams) {
        if (!team.enabled) continue;
        auto view = RtsScriptReadView::capture(
            impl_->session, team.definition.teamId);
        std::vector<detail::RtsScriptIntent> intents;
        intents.reserve(std::min<std::size_t>(
            team.definition.maximumIntentsPerTick, 64u));
        BindingScope scope(
            impl_->api.state_,
            view,
            intents,
            team.definition.maximumIntentsPerTick,
            team.definition.teamId,
            impl_->report.targetTick);

        bool succeeded = true;
        if (!team.started && team.onStart) {
            succeeded = impl_->invoke(
                team, *team.onStart, {}, "OnStart", completedTick);
            if (succeeded) team.started = true;
        } else if (!team.started) {
            team.started = true;
        }

        if (succeeded && team.onEvent) {
            for (const auto& event : events) {
                if (!eventRelevant(view, event)) continue;
                EventScope eventScope(impl_->api.state_, event);
                if (!impl_->invoke(
                        team, *team.onEvent, {}, "OnEvent", completedTick)) {
                    succeeded = false;
                    break;
                }
            }
        }

        if (succeeded &&
            impl_->report.targetTick % team.definition.thinkIntervalTicks == 0) {
            succeeded = impl_->invoke(
                team, team.onThink, {}, "OnThink", completedTick);
        }
        if (succeeded) impl_->flush(team, intents, impl_->report.targetTick);
    }

    return impl_->report.callbacks == 0
        ? RtsScriptTickResult::NoCallbacks
        : RtsScriptTickResult::Processed;
}

const rts::scripting::ScriptProgramIdentity*
RtsScriptSession::programIdentity() const noexcept {
    return valid() ? &impl_->program->identity() : nullptr;
}

std::vector<std::uint8_t> RtsScriptSession::encodeState(
    RuntimeError& error) const {
    if (!valid() || impl_->teams.size() > kMaximumTeams) {
        fail(error, "RTS script session is not snapshot-ready");
        return {};
    }
    foundation::BinaryWriter writer;
    writer.writeU32(kStateMagic);
    writer.writeU32(kStateVersion);
    writer.writeU32(static_cast<std::uint32_t>(impl_->teams.size()));
    for (const auto& team : impl_->teams) {
        const auto state = realscript::game::snapshotScriptObject(
            *impl_->runtime, team.object, error);
        if (!state) return {};
        const auto objectBytes = realscript::game::encodeScriptObjectState(
            *state, error);
        if (objectBytes.empty() || objectBytes.size() > kMaximumObjectStateBytes) {
            if (error.code == realscript::runtime::ErrorCode::None) {
                fail(error, "RTS script object state is too large");
            }
            return {};
        }
        writer.writeU32(team.definition.teamId);
        writer.writeString(team.definition.scriptType);
        writer.writeU32(team.definition.thinkIntervalTicks);
        writer.writeU64(static_cast<std::uint64_t>(
            team.definition.maximumIntentsPerTick));
        writer.writeU64(team.definition.executionPolicy.instructionBudget);
        writer.writeU64(static_cast<std::uint64_t>(
            team.definition.executionPolicy.recursionLimit));
        writer.writeU64(static_cast<std::uint64_t>(
            team.definition.executionPolicy.gcWorkBudget));
        writer.writeU8(
            team.definition.executionPolicy.strictDeterminism ? 1u : 0u);
        writer.writeU32(team.nextSequence);
        writer.writeU8(team.enabled ? 1u : 0u);
        writer.writeU8(team.started ? 1u : 0u);
        writer.writeU32(static_cast<std::uint32_t>(objectBytes.size()));
        writer.writeBytes(objectBytes);
    }
    return writer.take();
}

bool RtsScriptSession::restoreEncodedState(
    const std::vector<std::uint8_t>& bytes,
    RuntimeError& error) {
    if (!valid()) {
        fail(error, "RTS script runtime is unavailable");
        return false;
    }
    std::vector<PersistedTeam> persisted;
    if (!readPersistedTeams(bytes, persisted, error)) return false;

    std::vector<Impl::TeamInstance> candidate;
    candidate.reserve(persisted.size());
    for (const auto& entry : persisted) {
        Impl::TeamInstance team;
        if (!impl_->buildTeam(
                entry.definition,
                false,
                &entry.objectState,
                entry.nextSequence,
                entry.enabled,
                entry.started,
                team,
                error)) {
            return false;
        }
        candidate.push_back(std::move(team));
    }
    impl_->teams.swap(candidate);
    impl_->outcomes.clear();
    impl_->errors.clear();
    impl_->report = {};
    return true;
}

bool RtsScriptSession::authoritativeHash(
    std::uint64_t& output,
    RuntimeError& error) const {
    if (!valid()) {
        fail(error, "RTS script session cannot be hashed");
        return false;
    }
    foundation::CanonicalHash hash;
    hash.WriteU32(kStateVersion);
    const auto& identity = impl_->program->identity();
    hash.WriteU16(static_cast<std::uint16_t>(identity.bundle.type));
    hash.WriteU64(identity.bundle.id);
    hash.WriteU64(identity.bundlePayloadHash);
    hash.WriteU32(identity.script.sdkCompatibilityVersion);
    hash.WriteU32(identity.script.gameSdkPackageVersion);
    hash.WriteU64(identity.script.hostApiHash);
    hash.WriteU64(identity.script.programContentHash);
    hash.WriteU32(static_cast<std::uint32_t>(impl_->teams.size()));
    for (const auto& team : impl_->teams) {
        const auto state = realscript::game::snapshotScriptObject(
            *impl_->runtime, team.object, error);
        if (!state) return false;
        hash.WriteU32(team.definition.teamId);
        hash.WriteString(team.definition.scriptType);
        hash.WriteU32(team.definition.thinkIntervalTicks);
        hash.WriteU64(static_cast<std::uint64_t>(
            team.definition.maximumIntentsPerTick));
        hash.WriteU64(team.definition.executionPolicy.instructionBudget);
        hash.WriteU64(static_cast<std::uint64_t>(
            team.definition.executionPolicy.recursionLimit));
        hash.WriteU64(static_cast<std::uint64_t>(
            team.definition.executionPolicy.gcWorkBudget));
        hash.WriteBool(team.definition.executionPolicy.strictDeterminism);
        hash.WriteU32(team.nextSequence);
        hash.WriteBool(team.enabled);
        hash.WriteBool(team.started);
        hash.WriteU64(state->canonicalHash());
    }
    output = hash.Value();
    return true;
}

const RtsScriptTickReport& RtsScriptSession::lastReport() const noexcept {
    return impl_->report;
}

const std::vector<RtsScriptCommandOutcome>&
RtsScriptSession::outcomes() const noexcept {
    return impl_->outcomes;
}

const std::vector<RtsScriptError>& RtsScriptSession::errors() const noexcept {
    return impl_->errors;
}

void RtsScriptSession::clearErrors() {
    if (impl_) impl_->errors.clear();
}

} // namespace rts::gameplay::scripting
