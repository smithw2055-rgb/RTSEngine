#include <RTSEngine/RtsScripting/G3ScriptExtension.h>

#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay::scripting {
namespace {

constexpr std::uint32_t kStateMagic = 0x31583347u; // G3X1
constexpr std::uint16_t kStateVersion = 1u;
constexpr std::uint32_t kMaximumTeams = 4096u;
constexpr std::uint32_t kInitialSequence = 0x70000000u;

struct TeamSequence final {
    std::uint32_t teamId{};
    std::uint32_t nextSequence{kInitialSequence};
};

struct G3ScriptIntent final {
    G3ScriptIntentKind kind{G3ScriptIntentKind::Self};
    ecs::Entity caster{};
    std::uint32_t abilityId{};
    ecs::Entity target{};
    GridPoint point{};
    std::uint32_t ordinal{};
};

} // namespace

struct RtsG3ScriptExtension::State final {
    explicit State(RtsG3GameSession& value)
        : session(&value) {}

    bool ownedAlive(ecs::Entity entity) const {
        if (!session || !active || !entity.valid()) return false;
        const auto& world = session->base().simulation().world();
        const auto* team = world.try_get<Team>(entity);
        const auto* health = world.try_get<Health>(entity);
        return team && health && health->current > 0 &&
               team->id == teamId;
    }

    bool append(G3ScriptIntent intent) {
        if (!session || !active || intents.size() >= maximumIntents ||
            !ownedAlive(intent.caster) || intent.abilityId == 0) {
            return false;
        }
        if (intent.kind == G3ScriptIntentKind::Entity) {
            if (!intent.target.valid() ||
                !session->base().simulation().world().alive(intent.target)) {
                return false;
            }
        }
        if (intent.kind == G3ScriptIntentKind::Point &&
            !session->base().simulation().navigation().contains(intent.point)) {
            return false;
        }
        intent.ordinal =
            static_cast<std::uint32_t>(intents.size() + 1u);
        intents.push_back(intent);
        return true;
    }

    TeamSequence& sequenceFor(std::uint32_t id) {
        const auto found = std::lower_bound(
            sequences.begin(), sequences.end(), id,
            [](const TeamSequence& value, std::uint32_t key) {
                return value.teamId < key;
            });
        if (found != sequences.end() && found->teamId == id) {
            return *found;
        }
        return *sequences.insert(found, {id, kInitialSequence});
    }

    RtsG3GameSession* session{};
    std::uint32_t teamId{};
    std::uint64_t targetTick{};
    std::size_t maximumIntents{};
    bool active{};
    std::vector<G3ScriptIntent> intents;
    std::vector<TeamSequence> sequences;
    std::vector<G3ScriptCommandOutcome> outcomes;
};

RtsG3ScriptExtension::RtsG3ScriptExtension(
    RtsG3GameSession& session,
    RtsScriptApi& api)
    : state_(std::make_shared<State>(session)) {
    const auto state = state_;

    (void)api.mutableGameApi().function(
        "Engine.G3", "HasStatus",
        [state](std::int64_t entity, int statusId) {
            if (!state || !state->session || statusId <= 0) return false;
            const auto target = unpackScriptEntity(entity);
            for (const auto& status : state->session->statuses()) {
                if (status.target == target &&
                    status.definitionId ==
                        static_cast<std::uint32_t>(statusId)) {
                    return true;
                }
            }
            return false;
        });

    (void)api.mutableGameApi().function(
        "Engine.G3", "StatusStacks",
        [state](std::int64_t entity, int statusId) -> int {
            if (!state || !state->session || statusId <= 0) return 0;
            const auto target = unpackScriptEntity(entity);
            std::uint64_t stacks = 0;
            for (const auto& status : state->session->statuses()) {
                if (status.target == target &&
                    status.definitionId ==
                        static_cast<std::uint32_t>(statusId)) {
                    stacks += status.stacks;
                }
            }
            return stacks >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<int>::max())
                ? std::numeric_limits<int>::max()
                : static_cast<int>(stacks);
        });

    (void)api.mutableGameApi().function(
        "Engine.G3", "CastSelf",
        [state](std::int64_t caster, int abilityId) {
            if (!state || abilityId <= 0) return false;
            G3ScriptIntent intent;
            intent.kind = G3ScriptIntentKind::Self;
            intent.caster = unpackScriptEntity(caster);
            intent.abilityId = static_cast<std::uint32_t>(abilityId);
            return state->append(intent);
        });

    (void)api.mutableGameApi().function(
        "Engine.G3", "CastEntity",
        [state](
            std::int64_t caster,
            int abilityId,
            std::int64_t target) {
            if (!state || abilityId <= 0) return false;
            G3ScriptIntent intent;
            intent.kind = G3ScriptIntentKind::Entity;
            intent.caster = unpackScriptEntity(caster);
            intent.abilityId = static_cast<std::uint32_t>(abilityId);
            intent.target = unpackScriptEntity(target);
            return state->append(intent);
        });

    (void)api.mutableGameApi().function(
        "Engine.G3", "CastPoint",
        [state](
            std::int64_t caster,
            int abilityId,
            int x,
            int y) {
            if (!state || abilityId <= 0) return false;
            G3ScriptIntent intent;
            intent.kind = G3ScriptIntentKind::Point;
            intent.caster = unpackScriptEntity(caster);
            intent.abilityId = static_cast<std::uint32_t>(abilityId);
            intent.point = {x, y};
            return state->append(intent);
        });
}

RtsG3ScriptExtension::~RtsG3ScriptExtension() {
    if (state_) {
        state_->active = false;
        state_->intents.clear();
        state_->session = nullptr;
    }
}

bool RtsG3ScriptExtension::beginScope(
    std::uint32_t teamId,
    std::uint64_t targetTick,
    std::size_t maximumIntents) {
    if (!state_ || !state_->session || state_->active ||
        teamId == 0 || maximumIntents == 0 ||
        targetTick <
            state_->session->base().simulation().nextExpectedTick()) {
        return false;
    }
    state_->teamId = teamId;
    state_->targetTick = targetTick;
    state_->maximumIntents = maximumIntents;
    state_->intents.clear();
    state_->active = true;
    return true;
}

bool RtsG3ScriptExtension::commit() {
    if (!state_ || !state_->session || !state_->active) return false;
    bool accepted = true;
    auto& sequence = state_->sequenceFor(state_->teamId);
    for (const auto& intent : state_->intents) {
        if (sequence.nextSequence ==
            std::numeric_limits<std::uint32_t>::max()) {
            accepted = false;
            break;
        }
        AbilityCommand command;
        command.targetTick = state_->targetTick;
        command.issuer = state_->teamId;
        command.sequence = sequence.nextSequence++;
        command.caster = intent.caster;
        command.abilityId = intent.abilityId;
        command.targetEntity = intent.target;
        command.targetPoint = intent.point;
        if (intent.kind == G3ScriptIntentKind::Self) {
            command.targetEntity = intent.caster;
        }
        const auto result = state_->session->submitAbility(command);
        state_->outcomes.push_back({
            command.targetTick,
            command.issuer,
            command.sequence,
            intent.ordinal,
            command.abilityId,
            result});
        if (result != AbilitySubmitResult::Accepted &&
            result != AbilitySubmitResult::Duplicate) {
            accepted = false;
        }
    }
    discard();
    return accepted;
}

void RtsG3ScriptExtension::discard() noexcept {
    if (!state_) return;
    state_->teamId = 0;
    state_->targetTick = 0;
    state_->maximumIntents = 0;
    state_->intents.clear();
    state_->active = false;
}

bool RtsG3ScriptExtension::active() const noexcept {
    return state_ && state_->active;
}

bool RtsG3ScriptExtension::queueSelf(
    ScriptEntityId caster,
    std::uint32_t abilityId) {
    if (!state_) return false;
    G3ScriptIntent intent;
    intent.kind = G3ScriptIntentKind::Self;
    intent.caster = unpackScriptEntity(caster);
    intent.abilityId = abilityId;
    return state_->append(intent);
}

bool RtsG3ScriptExtension::queueEntity(
    ScriptEntityId caster,
    std::uint32_t abilityId,
    ScriptEntityId target) {
    if (!state_) return false;
    G3ScriptIntent intent;
    intent.kind = G3ScriptIntentKind::Entity;
    intent.caster = unpackScriptEntity(caster);
    intent.abilityId = abilityId;
    intent.target = unpackScriptEntity(target);
    return state_->append(intent);
}

bool RtsG3ScriptExtension::queuePoint(
    ScriptEntityId caster,
    std::uint32_t abilityId,
    std::int32_t x,
    std::int32_t y) {
    if (!state_) return false;
    G3ScriptIntent intent;
    intent.kind = G3ScriptIntentKind::Point;
    intent.caster = unpackScriptEntity(caster);
    intent.abilityId = abilityId;
    intent.point = {x, y};
    return state_->append(intent);
}

bool RtsG3ScriptExtension::hasStatus(
    ScriptEntityId entity,
    std::uint32_t statusId) const noexcept {
    return statusStacks(entity, statusId) != 0;
}

std::uint32_t RtsG3ScriptExtension::statusStacks(
    ScriptEntityId entity,
    std::uint32_t statusId) const noexcept {
    if (!state_ || !state_->session || statusId == 0) return 0;
    const auto target = unpackScriptEntity(entity);
    std::uint64_t stacks = 0;
    for (const auto& status : state_->session->statuses()) {
        if (status.target == target &&
            status.definitionId == statusId) {
            stacks += status.stacks;
        }
    }
    return stacks >
            std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max()
        : static_cast<std::uint32_t>(stacks);
}

std::vector<std::uint8_t>
RtsG3ScriptExtension::encodeState() const {
    if (!state_ || state_->active ||
        state_->sequences.size() > kMaximumTeams) {
        return {};
    }
    foundation::BinaryWriter writer;
    writer.writeU32(kStateMagic);
    writer.writeU16(kStateVersion);
    writer.writeU32(
        static_cast<std::uint32_t>(
            state_->sequences.size()));
    for (const auto& sequence : state_->sequences) {
        writer.writeU32(sequence.teamId);
        writer.writeU32(sequence.nextSequence);
    }
    writer.writeU64(authoritativeHash());
    return writer.take();
}

bool RtsG3ScriptExtension::restoreState(
    const std::vector<std::uint8_t>& bytes) {
    if (!state_ || state_->active) return false;
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint32_t count = 0;
    std::uint64_t storedHash = 0;
    if (!reader.readU32(magic) ||
        !reader.readU16(version) ||
        !reader.readU32(count) ||
        magic != kStateMagic ||
        version != kStateVersion ||
        count > kMaximumTeams) {
        return false;
    }
    std::vector<TeamSequence> candidate;
    candidate.resize(count);
    std::uint32_t previous = 0;
    for (auto& sequence : candidate) {
        if (!reader.readU32(sequence.teamId) ||
            !reader.readU32(sequence.nextSequence) ||
            sequence.teamId == 0 ||
            sequence.teamId <= previous ||
            sequence.nextSequence < kInitialSequence) {
            return false;
        }
        previous = sequence.teamId;
    }
    if (!reader.readU64(storedHash) || !reader.atEnd()) {
        return false;
    }
    const auto old = std::move(state_->sequences);
    state_->sequences = std::move(candidate);
    if (authoritativeHash() != storedHash) {
        state_->sequences = old;
        return false;
    }
    state_->outcomes.clear();
    return true;
}

std::uint64_t
RtsG3ScriptExtension::authoritativeHash() const noexcept {
    foundation::CanonicalHash hash;
    hash.WriteString("rts.g3-script-extension.v1");
    if (!state_) return hash.Value();
    hash.WriteU32(
        static_cast<std::uint32_t>(
            state_->sequences.size()));
    for (const auto& sequence : state_->sequences) {
        hash.WriteU32(sequence.teamId);
        hash.WriteU32(sequence.nextSequence);
    }
    return hash.Value();
}

const std::vector<G3ScriptCommandOutcome>&
RtsG3ScriptExtension::outcomes() const noexcept {
    static const std::vector<G3ScriptCommandOutcome> empty;
    return state_ ? state_->outcomes : empty;
}

void RtsG3ScriptExtension::clearOutcomes() {
    if (state_) state_->outcomes.clear();
}

} // namespace rts::gameplay::scripting
