#pragma once

#include <RTSEngine/Rts/DefinitionCatalog.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/RtsGameSession.h>
#include <RTSEngine/Rts/RtsGameSessionArchive.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace rts::gameplay {

enum class G3DamageType : std::uint8_t {
    Kinetic,
    Explosive,
    Energy,
    TrueDamage
};

enum class StatusStackingPolicy : std::uint8_t {
    RefreshDuration,
    AddStacks,
    Independent
};

enum class AbilityTargetKind : std::uint8_t {
    Self,
    Entity,
    Point
};

enum class AbilityEffectKind : std::uint8_t {
    Damage,
    Heal,
    ApplyStatus,
    SpawnProjectile
};

enum class SquadObjectiveKind : std::uint8_t {
    Assault,
    Defend,
    Retreat,
    Hold
};

enum class G3EventType : std::uint8_t {
    ProjectileSpawned,
    ProjectileHit,
    AbilityCastStarted,
    AbilityCastCompleted,
    AbilityRejected,
    StatusApplied,
    StatusTicked,
    StatusExpired,
    SquadTargetChanged,
    SquadCommandIssued
};

struct ProjectileDefinition final {
    std::uint32_t id{};
    std::uint32_t speedQ16{static_cast<std::uint32_t>(FixedPosition2D::kOne)};
    std::uint32_t lifetimeTicks{30};
    std::uint32_t hitRadiusQ16{static_cast<std::uint32_t>(FixedPosition2D::kOne / 4)};
    std::int32_t damage{1};
    G3DamageType damageType{G3DamageType::Kinetic};
    std::uint32_t splashRadiusQ16{};
    bool homing{true};
    bool friendlyFire{};
    std::uint32_t statusEffectId{};
};

struct StatusEffectDefinition final {
    std::uint32_t id{};
    std::uint32_t durationTicks{1};
    std::uint32_t periodTicks{};
    std::uint16_t maxStacks{1};
    StatusStackingPolicy stacking{StatusStackingPolicy::RefreshDuration};
    std::int32_t periodicHealthDelta{};
    G3DamageType periodicDamageType{G3DamageType::TrueDamage};
    std::uint16_t moveScalePermille{1000};
    std::uint16_t damageScalePermille{1000};
    std::int32_t armorAdd{};
    bool stunned{};
};

struct AbilityEffectDefinition final {
    AbilityEffectKind kind{AbilityEffectKind::Damage};
    std::int32_t amount{};
    std::uint32_t radiusQ16{};
    G3DamageType damageType{G3DamageType::Kinetic};
    std::uint32_t projectileDefinitionId{};
    std::uint32_t statusEffectId{};
};

struct AbilityDefinition final {
    std::uint32_t id{};
    std::uint32_t cooldownTicks{1};
    std::uint32_t castTicks{};
    std::uint32_t rangeQ16{static_cast<std::uint32_t>(FixedPosition2D::kOne * 6)};
    AbilityTargetKind targetKind{AbilityTargetKind::Entity};
    bool targetAllies{};
    bool targetEnemies{true};
    std::vector<AbilityEffectDefinition> effects;
};

struct AbilityCommand final {
    std::uint64_t targetTick{};
    std::uint32_t issuer{};
    std::uint32_t sequence{};
    ecs::Entity caster{};
    std::uint32_t abilityId{};
    ecs::Entity targetEntity{};
    GridPoint targetPoint{};
};

enum class AbilitySubmitResult : std::uint8_t {
    Accepted,
    Duplicate,
    ConflictingDuplicate,
    Late,
    Invalid
};

struct G3Event final {
    std::uint64_t tick{};
    G3EventType type{};
    ecs::Entity entity{};
    ecs::Entity secondary{};
    std::uint32_t objectId{};
    std::int32_t value{};
    GridPoint point{};
};

struct ProjectileState final {
    std::uint64_t id{};
    std::uint32_t definitionId{};
    ecs::Entity source{};
    ecs::Entity target{};
    std::uint32_t sourceTeam{};
    FixedPosition2D position{};
    FixedPosition2D targetPosition{};
    Facing16 facing{Facing16::South};
    std::uint32_t remainingTicks{};
};

struct StatusEffectState final {
    std::uint64_t instanceId{};
    std::uint32_t definitionId{};
    ecs::Entity source{};
    ecs::Entity target{};
    std::uint16_t stacks{1};
    std::uint64_t expiresAtTick{};
    std::uint64_t nextPeriodicTick{};
};

struct AbilityCooldownState final {
    std::uint32_t abilityId{};
    std::uint64_t readyTick{};
};

struct AbilityLoadoutState final {
    ecs::Entity entity{};
    std::vector<std::uint32_t> abilities;
    std::vector<AbilityCooldownState> cooldowns;
};

struct AbilityCastState final {
    std::uint64_t castId{};
    ecs::Entity caster{};
    std::uint32_t issuer{};
    std::uint32_t abilityId{};
    ecs::Entity targetEntity{};
    GridPoint targetPoint{};
    std::uint64_t completeTick{};
};

struct ProjectileBinding final {
    std::uint32_t definitionId{};
    std::uint32_t projectileDefinitionId{};
};

struct EntityProjectileBinding final {
    ecs::Entity entity{};
    std::uint32_t projectileDefinitionId{};
};

struct SquadState final {
    std::uint32_t id{};
    std::uint32_t teamId{};
    FormationKind formation{FormationKind::Box};
    SquadObjectiveKind objectiveKind{SquadObjectiveKind::Assault};
    GridPoint objective{};
    GridPoint retreatPoint{};
    ecs::Entity focusTarget{};
    std::uint32_t thinkIntervalTicks{6};
    std::uint64_t nextThinkTick{};
    std::uint16_t retreatHealthPermille{250};
    std::int32_t defenseRadius{8};
    std::vector<ecs::Entity> members;
};

class RtsG3GameSession final {
public:
    static constexpr std::uint32_t kArchiveMagic = 0x31473352u;
    static constexpr std::uint16_t kArchiveVersion = 1u;
    static constexpr std::uint32_t kMaximumEntries = 1'000'000u;
    static constexpr std::uint32_t kMaximumNestedBytes = 192u * 1024u * 1024u;

    explicit RtsG3GameSession(
        std::int32_t width = 32,
        std::int32_t height = 32)
        : base_(width, height) {}

    RtsGameSession& base() noexcept { return base_; }
    const RtsGameSession& base() const noexcept { return base_; }

    const std::vector<G3Event>& events() const noexcept { return events_; }
    const std::vector<ProjectileState>& projectiles() const noexcept {
        return projectiles_;
    }
    const std::vector<StatusEffectState>& statuses() const noexcept {
        return statuses_;
    }
    const std::vector<SquadState>& squads() const noexcept { return squads_; }

    bool registerProjectile(ProjectileDefinition value) {
        if (configurationFrozen() || !valid(value)) return false;
        projectileDefinitions_.replace(std::move(value));
        return true;
    }

    bool registerStatusEffect(StatusEffectDefinition value) {
        if (configurationFrozen() || !valid(value)) return false;
        statusDefinitions_.replace(std::move(value));
        return true;
    }

    bool registerAbility(AbilityDefinition value) {
        if (configurationFrozen() || !valid(value)) return false;
        abilityDefinitions_.replace(std::move(value));
        return true;
    }

    bool bindUnitProjectile(
        std::uint32_t unitDefinitionId,
        std::uint32_t projectileDefinitionId) {
        return setDefinitionBinding(
            unitProjectileBindings_, unitDefinitionId, projectileDefinitionId);
    }

    bool bindBuildingProjectile(
        std::uint32_t buildingDefinitionId,
        std::uint32_t projectileDefinitionId) {
        return setDefinitionBinding(
            buildingProjectileBindings_,
            buildingDefinitionId,
            projectileDefinitionId);
    }

    bool bindEntityProjectile(
        ecs::Entity entity,
        std::uint32_t projectileDefinitionId) {
        if (!entity.valid() || !projectileDefinitions_.find(projectileDefinitionId) ||
            !base_.simulation().world().alive(entity)) {
            return false;
        }
        const auto found = lowerEntityBinding(entity);
        if (found != entityProjectileBindings_.end() && found->entity == entity) {
            found->projectileDefinitionId = projectileDefinitionId;
        } else {
            entityProjectileBindings_.insert(
                found, {entity, projectileDefinitionId});
        }
        synchronizeProjectileBindings(base_.simulation().mutableWorld());
        return true;
    }

    bool setDamageResistance(
        ecs::Entity entity,
        DamageResistance resistance) {
        if (!base_.simulation().world().alive(entity) || !valid(resistance)) {
            return false;
        }
        base_.simulation().mutableWorld().emplace<DamageResistance>(
            entity, resistance);
        return true;
    }

    bool grantAbility(ecs::Entity entity, std::uint32_t abilityId) {
        if (!entity.valid() || !base_.simulation().world().alive(entity) ||
            !abilityDefinitions_.find(abilityId)) {
            return false;
        }
        auto found = lowerLoadout(entity);
        if (found == loadouts_.end() || found->entity != entity) {
            found = loadouts_.insert(found, AbilityLoadoutState{entity});
        }
        const auto ability = std::lower_bound(
            found->abilities.begin(), found->abilities.end(), abilityId);
        if (ability == found->abilities.end() || *ability != abilityId) {
            found->abilities.insert(ability, abilityId);
        }
        const auto cooldown = std::lower_bound(
            found->cooldowns.begin(), found->cooldowns.end(), abilityId,
            [](const AbilityCooldownState& value, std::uint32_t id) {
                return value.abilityId < id;
            });
        if (cooldown == found->cooldowns.end() || cooldown->abilityId != abilityId) {
            found->cooldowns.insert(cooldown, {abilityId, 0});
        }
        return true;
    }

    AbilitySubmitResult submitAbility(AbilityCommand command) {
        if (command.targetTick < base_.simulation().nextExpectedTick()) {
            return AbilitySubmitResult::Late;
        }
        if (command.issuer == 0 || command.sequence == 0 ||
            !command.caster.valid() || command.abilityId == 0) {
            return AbilitySubmitResult::Invalid;
        }
        const auto key = std::make_tuple(
            command.targetTick, command.issuer, command.sequence);
        const auto found = std::lower_bound(
            abilityCommands_.begin(), abilityCommands_.end(), key,
            [](const AbilityCommand& value, const auto& lookup) {
                return std::make_tuple(
                           value.targetTick, value.issuer, value.sequence) < lookup;
            });
        if (found != abilityCommands_.end() &&
            std::make_tuple(found->targetTick, found->issuer, found->sequence) == key) {
            return sameCommand(*found, command)
                ? AbilitySubmitResult::Duplicate
                : AbilitySubmitResult::ConflictingDuplicate;
        }
        abilityCommands_.insert(found, std::move(command));
        return AbilitySubmitResult::Accepted;
    }

    bool applyStatus(
        ecs::Entity target,
        std::uint32_t statusEffectId,
        ecs::Entity source = {}) {
        return applyStatusAt(
            base_.simulation().nextExpectedTick(),
            base_.simulation().mutableWorld(),
            target,
            statusEffectId,
            source);
    }

    std::uint32_t createSquad(SquadState squad) {
        if (squad.teamId == 0 || squad.thinkIntervalTicks == 0 ||
            squad.members.empty()) {
            return 0;
        }
        normalizeEntities(squad.members);
        auto& world = base_.simulation().mutableWorld();
        squad.members.erase(
            std::remove_if(
                squad.members.begin(), squad.members.end(),
                [&](ecs::Entity entity) {
                    const auto* team = world.try_get<Team>(entity);
                    const auto* health = world.try_get<Health>(entity);
                    return !team || !health || health->current <= 0 ||
                           team->id != squad.teamId;
                }),
            squad.members.end());
        if (squad.members.empty()) return 0;
        squad.id = ++nextSquadId_;
        squad.nextThinkTick = base_.simulation().nextExpectedTick();
        const auto found = std::lower_bound(
            squads_.begin(), squads_.end(), squad.id,
            [](const SquadState& value, std::uint32_t id) {
                return value.id < id;
            });
        squads_.insert(found, std::move(squad));
        ensureTeamSequence(squads_.back().teamId);
        return nextSquadId_;
    }

    RtsStepResult stepDetailed(std::uint64_t tick) {
        const auto expected = base_.simulation().nextExpectedTick();
        if (tick < expected) return RtsStepResult::StaleTick;
        if (tick > expected) return RtsStepResult::NonSequentialTick;

        events_.clear();
        auto& world = base_.simulation().mutableWorld();
        pruneRuntimeState(world);
        synchronizeProjectileBindings(world);
        advanceStatuses(tick, world);
        processAbilityCommands(tick, world);
        completeAbilityCasts(tick, world);
        advanceProjectiles(tick, world);
        rebuildStatusControls(world);

        const auto result = base_.stepDetailed(tick);
        if (result != RtsStepResult::Advanced) return result;

        observeWeaponEvents(tick, world);
        pruneRuntimeState(world);
        advanceSquads(base_.simulation().nextExpectedTick(), world);
        return result;
    }

    bool step(std::uint64_t tick) {
        return stepDetailed(tick) == RtsStepResult::Advanced;
    }

    bool stepNext() {
        return step(base_.simulation().nextExpectedTick());
    }

    std::uint64_t authoritativeHash() const {
        foundation::CanonicalHash hash;
        hash.WriteString("rts.g3-session.v1");
        hash.WriteU64(RtsGameSessionArchive::authoritativeHash(base_));
        appendContentHash(hash);
        appendStateHash(hash);
        return hash.Value();
    }

    std::vector<std::uint8_t> encode() const {
        const auto baseBytes = RtsGameSessionArchive::encode(base_);
        if (baseBytes.empty() || baseBytes.size() > kMaximumNestedBytes ||
            !boundedState()) {
            return {};
        }
        foundation::BinaryWriter writer;
        writer.writeU32(kArchiveMagic);
        writer.writeU16(kArchiveVersion);
        writer.writeU64(contentHash());
        writer.writeU32(static_cast<std::uint32_t>(baseBytes.size()));
        writer.writeBytes(baseBytes);
        writeAbilityCommands(writer);
        writeProjectiles(writer);
        writeStatuses(writer);
        writeLoadouts(writer);
        writeCasts(writer);
        writeBindings(writer, entityProjectileBindings_);
        writeSquads(writer);
        writeTeamSequences(writer);
        writer.writeU64(nextProjectileId_);
        writer.writeU64(nextStatusInstanceId_);
        writer.writeU64(nextCastId_);
        writer.writeU32(nextSquadId_);
        writer.writeU64(authoritativeHash());
        return writer.take();
    }

    bool decode(const std::vector<std::uint8_t>& bytes) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint64_t storedContentHash = 0;
        std::uint32_t nestedCount = 0;
        std::vector<std::uint8_t> baseBytes;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU64(storedContentHash) || magic != kArchiveMagic ||
            version != kArchiveVersion || storedContentHash != contentHash() ||
            !reader.readU32(nestedCount) || nestedCount == 0 ||
            nestedCount > kMaximumNestedBytes ||
            !reader.readBytes(nestedCount, baseBytes, kMaximumNestedBytes)) {
            return false;
        }

        std::vector<AbilityCommand> commands;
        std::vector<ProjectileState> projectiles;
        std::vector<StatusEffectState> statuses;
        std::vector<AbilityLoadoutState> loadouts;
        std::vector<AbilityCastState> casts;
        std::vector<EntityProjectileBinding> entityBindings;
        std::vector<SquadState> squads;
        std::vector<TeamSequenceState> teamSequences;
        std::uint64_t nextProjectile = 0;
        std::uint64_t nextStatus = 0;
        std::uint64_t nextCast = 0;
        std::uint32_t nextSquad = 0;
        std::uint64_t storedHash = 0;
        if (!readAbilityCommands(reader, commands) ||
            !readProjectiles(reader, projectiles) ||
            !readStatuses(reader, statuses) ||
            !readLoadouts(reader, loadouts) ||
            !readCasts(reader, casts) ||
            !readEntityBindings(reader, entityBindings) ||
            !readSquads(reader, squads) ||
            !readTeamSequences(reader, teamSequences) ||
            !reader.readU64(nextProjectile) ||
            !reader.readU64(nextStatus) ||
            !reader.readU64(nextCast) ||
            !reader.readU32(nextSquad) ||
            !reader.readU64(storedHash) || !reader.atEnd() ||
            !validateState(
                commands, projectiles, statuses, loadouts, casts,
                entityBindings, squads, teamSequences,
                nextProjectile, nextStatus, nextCast, nextSquad)) {
            return false;
        }

        if (!RtsGameSessionArchive::decode(baseBytes, base_)) return false;
        abilityCommands_ = std::move(commands);
        projectiles_ = std::move(projectiles);
        statuses_ = std::move(statuses);
        loadouts_ = std::move(loadouts);
        casts_ = std::move(casts);
        entityProjectileBindings_ = std::move(entityBindings);
        squads_ = std::move(squads);
        teamSequences_ = std::move(teamSequences);
        nextProjectileId_ = nextProjectile;
        nextStatusInstanceId_ = nextStatus;
        nextCastId_ = nextCast;
        nextSquadId_ = nextSquad;
        auto& world = base_.simulation().mutableWorld();
        controlledEntities_.clear();
        synchronizeProjectileBindings(world);
        rebuildStatusControls(world);
        return authoritativeHash() == storedHash;
    }

private:
    struct TeamSequenceState final {
        std::uint32_t teamId{};
        std::uint32_t nextSequence{0x60000000u};
    };

    static std::uint64_t packEntity(ecs::Entity entity) noexcept {
        return (static_cast<std::uint64_t>(entity.generation) << 32u) |
               entity.index;
    }

    static void writeEntity(
        foundation::BinaryWriter& writer,
        ecs::Entity entity) {
        writer.writeU32(entity.index);
        writer.writeU32(entity.generation);
    }

    static bool readEntity(
        foundation::BinaryReader& reader,
        ecs::Entity& entity) {
        return reader.readU32(entity.index) &&
               reader.readU32(entity.generation) &&
               ((entity.index == 0 && entity.generation == 0) ||
                (entity.index != 0 && entity.generation != 0));
    }

    static void hashEntity(
        foundation::CanonicalHash& hash,
        ecs::Entity entity) {
        hash.WriteU32(entity.index);
        hash.WriteU32(entity.generation);
    }

    static bool valid(const ProjectileDefinition& value) noexcept {
        return value.id != 0 && value.speedQ16 != 0 &&
               value.lifetimeTicks != 0 && value.damage >= 0 &&
               static_cast<std::uint8_t>(value.damageType) <=
                   static_cast<std::uint8_t>(G3DamageType::TrueDamage);
    }

    static bool valid(const StatusEffectDefinition& value) noexcept {
        return value.id != 0 && value.durationTicks != 0 &&
               value.maxStacks != 0 && value.moveScalePermille <= 4000 &&
               value.damageScalePermille <= 4000 &&
               static_cast<std::uint8_t>(value.stacking) <=
                   static_cast<std::uint8_t>(StatusStackingPolicy::Independent);
    }

    bool valid(const AbilityDefinition& value) const noexcept {
        if (value.id == 0 || value.cooldownTicks == 0 ||
            value.effects.empty() || value.effects.size() > 32u ||
            static_cast<std::uint8_t>(value.targetKind) >
                static_cast<std::uint8_t>(AbilityTargetKind::Point)) {
            return false;
        }
        for (const auto& effect : value.effects) {
            if (static_cast<std::uint8_t>(effect.kind) >
                static_cast<std::uint8_t>(AbilityEffectKind::SpawnProjectile)) {
                return false;
            }
            if (effect.kind == AbilityEffectKind::SpawnProjectile &&
                !projectileDefinitions_.find(effect.projectileDefinitionId)) {
                return false;
            }
            if (effect.kind == AbilityEffectKind::ApplyStatus &&
                !statusDefinitions_.find(effect.statusEffectId)) {
                return false;
            }
        }
        return true;
    }

    static bool valid(const DamageResistance& value) noexcept {
        return value.kineticPermille <= 4000 &&
               value.explosivePermille <= 4000 &&
               value.energyPermille <= 4000;
    }

    bool configurationFrozen() const noexcept {
        return base_.simulation().configurationFrozen();
    }

    bool setDefinitionBinding(
        std::vector<ProjectileBinding>& bindings,
        std::uint32_t definitionId,
        std::uint32_t projectileDefinitionId) {
        if (configurationFrozen() || definitionId == 0 ||
            !projectileDefinitions_.find(projectileDefinitionId)) {
            return false;
        }
        const auto found = std::lower_bound(
            bindings.begin(), bindings.end(), definitionId,
            [](const ProjectileBinding& value, std::uint32_t id) {
                return value.definitionId < id;
            });
        if (found != bindings.end() && found->definitionId == definitionId) {
            found->projectileDefinitionId = projectileDefinitionId;
        } else {
            bindings.insert(found, {definitionId, projectileDefinitionId});
        }
        return true;
    }

    static const ProjectileBinding* findBinding(
        const std::vector<ProjectileBinding>& bindings,
        std::uint32_t definitionId) noexcept {
        const auto found = std::lower_bound(
            bindings.begin(), bindings.end(), definitionId,
            [](const ProjectileBinding& value, std::uint32_t id) {
                return value.definitionId < id;
            });
        return found != bindings.end() && found->definitionId == definitionId
            ? &*found : nullptr;
    }

    std::vector<EntityProjectileBinding>::iterator lowerEntityBinding(
        ecs::Entity entity) noexcept {
        return std::lower_bound(
            entityProjectileBindings_.begin(), entityProjectileBindings_.end(), entity,
            [](const EntityProjectileBinding& value, ecs::Entity key) {
                return value.entity < key;
            });
    }

    std::vector<AbilityLoadoutState>::iterator lowerLoadout(
        ecs::Entity entity) noexcept {
        return std::lower_bound(
            loadouts_.begin(), loadouts_.end(), entity,
            [](const AbilityLoadoutState& value, ecs::Entity key) {
                return value.entity < key;
            });
    }

    std::vector<AbilityLoadoutState>::const_iterator lowerLoadout(
        ecs::Entity entity) const noexcept {
        return std::lower_bound(
            loadouts_.begin(), loadouts_.end(), entity,
            [](const AbilityLoadoutState& value, ecs::Entity key) {
                return value.entity < key;
            });
    }

    static bool sameCommand(
        const AbilityCommand& first,
        const AbilityCommand& second) noexcept {
        return first.targetTick == second.targetTick &&
               first.issuer == second.issuer &&
               first.sequence == second.sequence &&
               first.caster == second.caster &&
               first.abilityId == second.abilityId &&
               first.targetEntity == second.targetEntity &&
               first.targetPoint == second.targetPoint;
    }

    static void normalizeEntities(std::vector<ecs::Entity>& values) {
        values.erase(
            std::remove_if(
                values.begin(), values.end(),
                [](ecs::Entity value) { return !value.valid(); }),
            values.end());
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }

    static std::int64_t square(std::int64_t value) noexcept {
        return value * value;
    }

    static std::uint64_t distanceSquaredQ16(
        FixedPosition2D first,
        FixedPosition2D second) noexcept {
        const auto dx = static_cast<std::int64_t>(first.x) - second.x;
        const auto dy = static_cast<std::int64_t>(first.y) - second.y;
        return static_cast<std::uint64_t>(square(dx) + square(dy));
    }

    static FixedPosition2D fixedPosition(Position value) noexcept {
        return FixedPosition2D::fromCell({value.x, value.y});
    }

    static std::uint16_t resistanceFor(
        const DamageResistance* resistance,
        G3DamageType type) noexcept {
        if (!resistance) return 1000;
        switch (type) {
        case G3DamageType::Kinetic: return resistance->kineticPermille;
        case G3DamageType::Explosive: return resistance->explosivePermille;
        case G3DamageType::Energy: return resistance->energyPermille;
        case G3DamageType::TrueDamage: return 1000;
        }
        return 1000;
    }

    std::int32_t applyDamage(
        std::uint64_t tick,
        ecs::World& world,
        ecs::Entity source,
        ecs::Entity target,
        std::int32_t amount,
        G3DamageType type) {
        auto* health = world.try_get<Health>(target);
        if (!health || health->current <= 0 || amount <= 0) return 0;
        std::int32_t mitigated = amount;
        if (type != G3DamageType::TrueDamage) {
            const auto* armor = world.try_get<Armor>(target);
            const auto* control = world.try_get<StatusControl>(target);
            const auto armorValue = std::max<std::int32_t>(
                0, (armor ? armor->value : 0) +
                   (control ? control->armorAdd : 0));
            mitigated = std::max<std::int32_t>(0, mitigated - armorValue);
            mitigated = static_cast<std::int32_t>(
                static_cast<std::int64_t>(mitigated) *
                resistanceFor(world.try_get<DamageResistance>(target), type) /
                1000);
        }
        health->current = std::max<std::int32_t>(0, health->current - mitigated);
        events_.push_back(
            {tick, G3EventType::ProjectileHit, source, target, 0, mitigated, {}});
        return mitigated;
    }

    static std::int32_t heal(
        ecs::World& world,
        ecs::Entity target,
        std::int32_t amount) {
        auto* health = world.try_get<Health>(target);
        if (!health || health->current <= 0 || amount <= 0) return 0;
        const auto before = health->current;
        health->current = std::min(health->maximum, health->current + amount);
        return health->current - before;
    }

    void synchronizeProjectileBindings(ecs::World& world) {
        world.eachRef<Weapon>(
            [&](ecs::Entity entity, Weapon& weapon) {
                std::uint32_t projectile = 0;
                const auto entityBinding = lowerEntityBinding(entity);
                if (entityBinding != entityProjectileBindings_.end() &&
                    entityBinding->entity == entity) {
                    projectile = entityBinding->projectileDefinitionId;
                }
                if (projectile == 0) {
                    const auto* archetype = world.try_get<UnitArchetype>(entity);
                    if (archetype) {
                        const auto* binding = findBinding(
                            unitProjectileBindings_, archetype->definitionId);
                        if (binding) projectile = binding->projectileDefinitionId;
                    }
                }
                if (projectile == 0) {
                    const auto* building = world.try_get<Building>(entity);
                    if (building) {
                        const auto* binding = findBinding(
                            buildingProjectileBindings_, building->definitionId);
                        if (binding) projectile = binding->projectileDefinitionId;
                    }
                }
                weapon.projectileDefinitionId = projectile;
            });
    }

    bool applyStatusAt(
        std::uint64_t tick,
        ecs::World& world,
        ecs::Entity target,
        std::uint32_t statusEffectId,
        ecs::Entity source) {
        const auto* definition = statusDefinitions_.find(statusEffectId);
        if (!definition || !world.alive(target) ||
            !world.try_get<Health>(target)) {
            return false;
        }
        if (definition->stacking != StatusStackingPolicy::Independent) {
            const auto found = std::find_if(
                statuses_.begin(), statuses_.end(),
                [&](const StatusEffectState& value) {
                    return value.target == target &&
                           value.definitionId == statusEffectId;
                });
            if (found != statuses_.end()) {
                found->expiresAtTick = tick + definition->durationTicks;
                found->source = source;
                if (definition->stacking == StatusStackingPolicy::AddStacks) {
                    found->stacks = static_cast<std::uint16_t>(std::min<std::uint32_t>(
                        definition->maxStacks,
                        static_cast<std::uint32_t>(found->stacks) + 1u));
                }
                events_.push_back(
                    {tick, G3EventType::StatusApplied, target, source,
                     statusEffectId, found->stacks, {}});
                rebuildStatusControls(world);
                return true;
            }
        }
        StatusEffectState state;
        state.instanceId = ++nextStatusInstanceId_;
        state.definitionId = statusEffectId;
        state.source = source;
        state.target = target;
        state.expiresAtTick = tick + definition->durationTicks;
        state.nextPeriodicTick = definition->periodTicks == 0
            ? std::numeric_limits<std::uint64_t>::max()
            : tick + definition->periodTicks;
        statuses_.push_back(state);
        sortStatuses();
        events_.push_back(
            {tick, G3EventType::StatusApplied, target, source,
             statusEffectId, 1, {}});
        rebuildStatusControls(world);
        return true;
    }

    void sortStatuses() {
        std::sort(
            statuses_.begin(), statuses_.end(),
            [](const StatusEffectState& first, const StatusEffectState& second) {
                if (first.target != second.target) return first.target < second.target;
                if (first.definitionId != second.definitionId) {
                    return first.definitionId < second.definitionId;
                }
                return first.instanceId < second.instanceId;
            });
    }

    static std::uint16_t multiplyScale(
        std::uint16_t current,
        std::uint16_t factor,
        std::uint16_t stacks) noexcept {
        std::uint32_t result = current;
        for (std::uint16_t index = 0; index < stacks; ++index) {
            result = std::min<std::uint32_t>(
                4000u, static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(result) * factor / 1000u));
        }
        return static_cast<std::uint16_t>(result);
    }

    void rebuildStatusControls(ecs::World& world) {
        for (const auto entity : controlledEntities_) {
            if (world.alive(entity)) world.remove<StatusControl>(entity);
        }
        controlledEntities_.clear();

        std::size_t index = 0;
        while (index < statuses_.size()) {
            const auto target = statuses_[index].target;
            StatusControl control;
            bool validTarget = world.alive(target) &&
                               world.try_get<Health>(target);
            while (index < statuses_.size() && statuses_[index].target == target) {
                const auto& status = statuses_[index];
                const auto* definition = statusDefinitions_.find(status.definitionId);
                if (validTarget && definition) {
                    control.moveScalePermille = multiplyScale(
                        control.moveScalePermille,
                        definition->moveScalePermille,
                        status.stacks);
                    control.damageScalePermille = multiplyScale(
                        control.damageScalePermille,
                        definition->damageScalePermille,
                        status.stacks);
                    const auto armor = static_cast<std::int64_t>(control.armorAdd) +
                        static_cast<std::int64_t>(definition->armorAdd) * status.stacks;
                    control.armorAdd = static_cast<std::int32_t>(std::max<std::int64_t>(
                        std::numeric_limits<std::int32_t>::min(),
                        std::min<std::int64_t>(
                            std::numeric_limits<std::int32_t>::max(), armor)));
                    control.stunned = control.stunned || definition->stunned;
                }
                ++index;
            }
            if (validTarget) {
                world.emplace<StatusControl>(target, control);
                controlledEntities_.push_back(target);
            }
        }
    }

    void advanceStatuses(std::uint64_t tick, ecs::World& world) {
        auto iterator = statuses_.begin();
        while (iterator != statuses_.end()) {
            const auto* definition = statusDefinitions_.find(iterator->definitionId);
            if (!definition || !world.alive(iterator->target) ||
                !world.try_get<Health>(iterator->target)) {
                iterator = statuses_.erase(iterator);
                continue;
            }
            if (tick >= iterator->expiresAtTick) {
                events_.push_back(
                    {tick, G3EventType::StatusExpired, iterator->target,
                     iterator->source, iterator->definitionId, 0, {}});
                iterator = statuses_.erase(iterator);
                continue;
            }
            if (definition->periodTicks != 0 &&
                tick >= iterator->nextPeriodicTick) {
                const auto magnitude = static_cast<std::int64_t>(
                    definition->periodicHealthDelta) * iterator->stacks;
                if (magnitude < 0) {
                    applyDamage(
                        tick, world, iterator->source, iterator->target,
                        static_cast<std::int32_t>(std::min<std::int64_t>(
                            std::numeric_limits<std::int32_t>::max(), -magnitude)),
                        definition->periodicDamageType);
                } else if (magnitude > 0) {
                    (void)heal(
                        world, iterator->target,
                        static_cast<std::int32_t>(std::min<std::int64_t>(
                            std::numeric_limits<std::int32_t>::max(), magnitude)));
                }
                iterator->nextPeriodicTick += definition->periodTicks;
                events_.push_back(
                    {tick, G3EventType::StatusTicked, iterator->target,
                     iterator->source, iterator->definitionId,
                     definition->periodicHealthDelta, {}});
            }
            ++iterator;
        }
        rebuildStatusControls(world);
    }

    bool entityGrantedAbility(
        ecs::Entity entity,
        std::uint32_t abilityId) const noexcept {
        const auto found = lowerLoadout(entity);
        return found != loadouts_.end() && found->entity == entity &&
               std::binary_search(
                   found->abilities.begin(), found->abilities.end(), abilityId);
    }

    std::uint64_t cooldownReadyTick(
        ecs::Entity entity,
        std::uint32_t abilityId) const noexcept {
        const auto found = lowerLoadout(entity);
        if (found == loadouts_.end() || found->entity != entity) return 0;
        const auto cooldown = std::lower_bound(
            found->cooldowns.begin(), found->cooldowns.end(), abilityId,
            [](const AbilityCooldownState& value, std::uint32_t id) {
                return value.abilityId < id;
            });
        return cooldown != found->cooldowns.end() && cooldown->abilityId == abilityId
            ? cooldown->readyTick : 0;
    }

    void setCooldown(
        ecs::Entity entity,
        std::uint32_t abilityId,
        std::uint64_t readyTick) {
        auto found = lowerLoadout(entity);
        if (found == loadouts_.end() || found->entity != entity) return;
        const auto cooldown = std::lower_bound(
            found->cooldowns.begin(), found->cooldowns.end(), abilityId,
            [](const AbilityCooldownState& value, std::uint32_t id) {
                return value.abilityId < id;
            });
        if (cooldown != found->cooldowns.end() && cooldown->abilityId == abilityId) {
            cooldown->readyTick = readyTick;
        }
    }

    bool validAbilityTarget(
        const ecs::World& world,
        std::uint32_t issuer,
        const AbilityDefinition& definition,
        const AbilityCommand& command,
        GridPoint& resolvedPoint) const {
        const auto* casterPosition = world.try_get<Position>(command.caster);
        const auto* casterTeam = world.try_get<Team>(command.caster);
        if (!casterPosition || !casterTeam || casterTeam->id != issuer) return false;
        if (definition.targetKind == AbilityTargetKind::Self) {
            resolvedPoint = {casterPosition->x, casterPosition->y};
            return true;
        }
        if (definition.targetKind == AbilityTargetKind::Point) {
            resolvedPoint = command.targetPoint;
        } else {
            const auto* targetPosition = world.try_get<Position>(command.targetEntity);
            const auto* targetTeam = world.try_get<Team>(command.targetEntity);
            const auto* targetHealth = world.try_get<Health>(command.targetEntity);
            if (!targetPosition || !targetTeam || !targetHealth ||
                targetHealth->current <= 0) {
                return false;
            }
            const bool ally = !base_.diplomacy().hostile(issuer, targetTeam->id);
            if ((ally && !definition.targetAllies) ||
                (!ally && !definition.targetEnemies)) {
                return false;
            }
            if (!ally && base_.simulation().nextExpectedTick() != 0 &&
                    !base_.simulation().vision().visible(
                    issuer, {targetPosition->x, targetPosition->y})) {
                return false;
            }
            resolvedPoint = {targetPosition->x, targetPosition->y};
        }
        const auto dx = static_cast<std::int64_t>(resolvedPoint.x - casterPosition->x) *
                        FixedPosition2D::kOne;
        const auto dy = static_cast<std::int64_t>(resolvedPoint.y - casterPosition->y) *
                        FixedPosition2D::kOne;
        return square(dx) + square(dy) <=
               square(static_cast<std::int64_t>(definition.rangeQ16));
    }

    void processAbilityCommands(std::uint64_t tick, ecs::World& world) {
        auto iterator = abilityCommands_.begin();
        while (iterator != abilityCommands_.end() &&
               iterator->targetTick <= tick) {
            const auto command = *iterator;
            iterator = abilityCommands_.erase(iterator);
            if (command.targetTick != tick) continue;
            startAbilityCast(tick, world, command);
        }
    }

    void startAbilityCast(
        std::uint64_t tick,
        ecs::World& world,
        const AbilityCommand& command) {
        const auto* definition = abilityDefinitions_.find(command.abilityId);
        const auto* health = world.try_get<Health>(command.caster);
        const auto* control = world.try_get<StatusControl>(command.caster);
        GridPoint point;
        if (!definition || !health || health->current <= 0 ||
            (control && control->stunned) ||
            !entityGrantedAbility(command.caster, command.abilityId) ||
            cooldownReadyTick(command.caster, command.abilityId) > tick ||
            !validAbilityTarget(world, command.issuer, *definition, command, point)) {
            events_.push_back(
                {tick, G3EventType::AbilityRejected, command.caster,
                 command.targetEntity, command.abilityId, 0, command.targetPoint});
            return;
        }
        setCooldown(
            command.caster,
            command.abilityId,
            tick + definition->cooldownTicks);
        AbilityCastState cast;
        cast.castId = ++nextCastId_;
        cast.caster = command.caster;
        cast.issuer = command.issuer;
        cast.abilityId = command.abilityId;
        cast.targetEntity = command.targetEntity;
        cast.targetPoint = point;
        cast.completeTick = tick + definition->castTicks;
        casts_.push_back(cast);
        std::sort(
            casts_.begin(), casts_.end(),
            [](const AbilityCastState& first, const AbilityCastState& second) {
                if (first.completeTick != second.completeTick) {
                    return first.completeTick < second.completeTick;
                }
                return first.castId < second.castId;
            });
        events_.push_back(
            {tick, G3EventType::AbilityCastStarted, command.caster,
             command.targetEntity, command.abilityId, 0, point});
    }

    void completeAbilityCasts(std::uint64_t tick, ecs::World& world) {
        auto iterator = casts_.begin();
        while (iterator != casts_.end() && iterator->completeTick <= tick) {
            const auto cast = *iterator;
            iterator = casts_.erase(iterator);
            const auto* definition = abilityDefinitions_.find(cast.abilityId);
            const auto* health = world.try_get<Health>(cast.caster);
            if (!definition || !health || health->current <= 0) continue;
            for (const auto& effect : definition->effects) {
                applyAbilityEffect(tick, world, cast, *definition, effect);
            }
            events_.push_back(
                {tick, G3EventType::AbilityCastCompleted, cast.caster,
                 cast.targetEntity, cast.abilityId, 0, cast.targetPoint});
        }
    }

    std::vector<ecs::Entity> collectEffectTargets(
        const ecs::World& world,
        const AbilityCastState& cast,
        const AbilityDefinition& ability,
        std::uint32_t radiusQ16) const {
        std::vector<ecs::Entity> targets;
        if (radiusQ16 == 0 && ability.targetKind == AbilityTargetKind::Entity &&
            world.alive(cast.targetEntity)) {
            targets.push_back(cast.targetEntity);
            return targets;
        }
        const auto center = FixedPosition2D::fromCell(cast.targetPoint);
        world.eachRef<Position, Team, Health>(
            [&](ecs::Entity entity,
                const Position& position,
                const Team& team,
                const Health& health) {
                if (health.current <= 0) return;
                const bool ally = !base_.diplomacy().hostile(cast.issuer, team.id);
                if ((ally && !ability.targetAllies) ||
                    (!ally && !ability.targetEnemies)) {
                    return;
                }
                if (!ally && base_.simulation().nextExpectedTick() != 0 &&
                    !base_.simulation().vision().visible(
                        cast.issuer, {position.x, position.y})) {
                    return;
                }
                if (distanceSquaredQ16(fixedPosition(position), center) <=
                    square(static_cast<std::int64_t>(radiusQ16))) {
                    targets.push_back(entity);
                }
            });
        std::sort(targets.begin(), targets.end());
        return targets;
    }

    void applyAbilityEffect(
        std::uint64_t tick,
        ecs::World& world,
        const AbilityCastState& cast,
        const AbilityDefinition& ability,
        const AbilityEffectDefinition& effect) {
        if (effect.kind == AbilityEffectKind::SpawnProjectile) {
            spawnProjectile(
                tick,
                world,
                effect.projectileDefinitionId,
                cast.caster,
                cast.issuer,
                cast.targetEntity,
                FixedPosition2D::fromCell(cast.targetPoint));
            return;
        }
        const auto targets = collectEffectTargets(
            world, cast, ability, effect.radiusQ16);
        for (const auto target : targets) {
            switch (effect.kind) {
            case AbilityEffectKind::Damage:
                (void)applyDamage(
                    tick, world, cast.caster, target,
                    std::max<std::int32_t>(0, effect.amount), effect.damageType);
                break;
            case AbilityEffectKind::Heal:
                (void)heal(world, target, std::max<std::int32_t>(0, effect.amount));
                break;
            case AbilityEffectKind::ApplyStatus:
                (void)applyStatusAt(
                    tick, world, target, effect.statusEffectId, cast.caster);
                break;
            case AbilityEffectKind::SpawnProjectile:
                break;
            }
        }
    }

    void spawnProjectile(
        std::uint64_t tick,
        ecs::World& world,
        std::uint32_t definitionId,
        ecs::Entity source,
        std::uint32_t sourceTeam,
        ecs::Entity target,
        FixedPosition2D targetPosition) {
        const auto* definition = projectileDefinitions_.find(definitionId);
        const auto* sourcePosition = world.try_get<Position>(source);
        if (!definition || !sourcePosition || sourceTeam == 0) return;
        if (world.alive(target)) {
            const auto* position = world.try_get<Position>(target);
            if (position) targetPosition = fixedPosition(*position);
        }
        ProjectileState projectile;
        projectile.id = ++nextProjectileId_;
        projectile.definitionId = definitionId;
        projectile.source = source;
        projectile.target = target;
        projectile.sourceTeam = sourceTeam;
        projectile.position = fixedPosition(*sourcePosition);
        projectile.targetPosition = targetPosition;
        projectile.remainingTicks = definition->lifetimeTicks;
        projectiles_.push_back(projectile);
        std::sort(
            projectiles_.begin(), projectiles_.end(),
            [](const ProjectileState& first, const ProjectileState& second) {
                return first.id < second.id;
            });
        events_.push_back(
            {tick, G3EventType::ProjectileSpawned, source, target,
             definitionId, 0, targetPosition.cell()});
    }

    void observeWeaponEvents(std::uint64_t tick, ecs::World& world) {
        for (const auto& event : base_.simulation().events()) {
            if (event.type != DomainEventType::WeaponFired) continue;
            const auto* weapon = world.try_get<Weapon>(event.entity);
            const auto* team = world.try_get<Team>(event.entity);
            const auto* targetPosition = world.try_get<Position>(event.secondary);
            if (!weapon || !team || weapon->projectileDefinitionId == 0 ||
                !projectileDefinitions_.find(weapon->projectileDefinitionId)) {
                continue;
            }
            spawnProjectile(
                tick,
                world,
                weapon->projectileDefinitionId,
                event.entity,
                team->id,
                event.secondary,
                targetPosition ? fixedPosition(*targetPosition) : FixedPosition2D{});
        }
    }

    void impactProjectile(
        std::uint64_t tick,
        ecs::World& world,
        const ProjectileState& projectile,
        const ProjectileDefinition& definition) {
        std::vector<ecs::Entity> targets;
        const auto center = projectile.position;
        if (definition.splashRadiusQ16 == 0 && world.alive(projectile.target)) {
            targets.push_back(projectile.target);
        } else {
            const auto radius = definition.splashRadiusQ16 == 0
                ? definition.hitRadiusQ16 : definition.splashRadiusQ16;
            world.eachRef<Position, Team, Health>(
                [&](ecs::Entity entity,
                    const Position& position,
                    const Team& team,
                    const Health& health) {
                    if (health.current <= 0) return;
                    const bool hostile = base_.diplomacy().hostile(
                        projectile.sourceTeam, team.id);
                    if (!hostile && !definition.friendlyFire) return;
                    if (distanceSquaredQ16(fixedPosition(position), center) <=
                        square(static_cast<std::int64_t>(radius))) {
                        targets.push_back(entity);
                    }
                });
            std::sort(targets.begin(), targets.end());
        }
        for (const auto target : targets) {
            (void)applyDamage(
                tick, world, projectile.source, target,
                definition.damage, definition.damageType);
            if (definition.statusEffectId != 0) {
                (void)applyStatusAt(
                    tick, world, target,
                    definition.statusEffectId, projectile.source);
            }
        }
    }

    void advanceProjectiles(std::uint64_t tick, ecs::World& world) {
        auto iterator = projectiles_.begin();
        while (iterator != projectiles_.end()) {
            const auto* definition = projectileDefinitions_.find(
                iterator->definitionId);
            if (!definition || iterator->remainingTicks == 0) {
                iterator = projectiles_.erase(iterator);
                continue;
            }
            if (definition->homing && world.alive(iterator->target)) {
                const auto* targetPosition = world.try_get<Position>(iterator->target);
                if (targetPosition) iterator->targetPosition = fixedPosition(*targetPosition);
            }
            const auto moved = FixedMover::advanceToward(
                iterator->position,
                iterator->targetPosition,
                definition->speedQ16);
            iterator->position = moved.position;
            iterator->facing = moved.facing;
            --iterator->remainingTicks;
            const bool hit = moved.arrived ||
                distanceSquaredQ16(iterator->position, iterator->targetPosition) <=
                    square(static_cast<std::int64_t>(definition->hitRadiusQ16));
            if (hit) {
                impactProjectile(tick, world, *iterator, *definition);
                iterator = projectiles_.erase(iterator);
            } else if (iterator->remainingTicks == 0) {
                iterator = projectiles_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void pruneRuntimeState(ecs::World& world) {
        entityProjectileBindings_.erase(
            std::remove_if(
                entityProjectileBindings_.begin(), entityProjectileBindings_.end(),
                [&](const EntityProjectileBinding& value) {
                    return !world.alive(value.entity);
                }),
            entityProjectileBindings_.end());
        loadouts_.erase(
            std::remove_if(
                loadouts_.begin(), loadouts_.end(),
                [&](const AbilityLoadoutState& value) {
                    return !world.alive(value.entity);
                }),
            loadouts_.end());
        casts_.erase(
            std::remove_if(
                casts_.begin(), casts_.end(),
                [&](const AbilityCastState& value) {
                    return !world.alive(value.caster);
                }),
            casts_.end());
        statuses_.erase(
            std::remove_if(
                statuses_.begin(), statuses_.end(),
                [&](const StatusEffectState& value) {
                    return !world.alive(value.target);
                }),
            statuses_.end());
        for (auto& squad : squads_) {
            squad.members.erase(
                std::remove_if(
                    squad.members.begin(), squad.members.end(),
                    [&](ecs::Entity entity) {
                        const auto* health = world.try_get<Health>(entity);
                        const auto* team = world.try_get<Team>(entity);
                        return !health || health->current <= 0 || !team ||
                               team->id != squad.teamId;
                    }),
                squad.members.end());
            if (!world.alive(squad.focusTarget)) squad.focusTarget = {};
        }
        squads_.erase(
            std::remove_if(
                squads_.begin(), squads_.end(),
                [](const SquadState& value) { return value.members.empty(); }),
            squads_.end());
    }

    TeamSequenceState& ensureTeamSequence(std::uint32_t teamId) {
        const auto found = std::lower_bound(
            teamSequences_.begin(), teamSequences_.end(), teamId,
            [](const TeamSequenceState& value, std::uint32_t id) {
                return value.teamId < id;
            });
        if (found != teamSequences_.end() && found->teamId == teamId) {
            return *found;
        }
        return *teamSequences_.insert(found, {teamId, 0x60000000u});
    }

    ecs::Entity chooseSquadTarget(
        const ecs::World& world,
        const SquadState& squad,
        GridPoint centroid) const {
        ecs::Entity best{};
        std::int32_t bestDistance = std::numeric_limits<std::int32_t>::max();
        world.eachRef<Position, Team, Health>(
            [&](ecs::Entity entity,
                const Position& position,
                const Team& team,
                const Health& health) {
                if (health.current <= 0 ||
                    !base_.diplomacy().hostile(squad.teamId, team.id) ||
                    !base_.simulation().vision().visible(
                        squad.teamId, {position.x, position.y})) {
                    return;
                }
                const auto dx = std::abs(position.x - centroid.x);
                const auto dy = std::abs(position.y - centroid.y);
                const auto distance = dx + dy;
                if (squad.objectiveKind == SquadObjectiveKind::Defend &&
                    std::abs(position.x - squad.objective.x) +
                    std::abs(position.y - squad.objective.y) >
                        squad.defenseRadius) {
                    return;
                }
                if (distance < bestDistance ||
                    (distance == bestDistance && (!best.valid() || entity < best))) {
                    best = entity;
                    bestDistance = distance;
                }
            });
        return best;
    }

    static GridPoint squadCentroid(
        const ecs::World& world,
        const SquadState& squad) {
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t count = 0;
        for (const auto entity : squad.members) {
            const auto* position = world.try_get<Position>(entity);
            if (!position) continue;
            x += position->x;
            y += position->y;
            ++count;
        }
        return count == 0
            ? squad.objective
            : GridPoint{
                  static_cast<std::int32_t>(x / count),
                  static_cast<std::int32_t>(y / count)};
    }

    static std::uint16_t squadHealthPermille(
        const ecs::World& world,
        const SquadState& squad) {
        std::int64_t current = 0;
        std::int64_t maximum = 0;
        for (const auto entity : squad.members) {
            const auto* health = world.try_get<Health>(entity);
            if (!health) continue;
            current += health->current;
            maximum += health->maximum;
        }
        return maximum == 0 ? 0 : static_cast<std::uint16_t>(std::min<std::int64_t>(
            1000, current * 1000 / maximum));
    }

    void advanceSquads(std::uint64_t nextTick, ecs::World& world) {
        for (auto& squad : squads_) {
            if (nextTick < squad.nextThinkTick) continue;
            squad.nextThinkTick = nextTick + squad.thinkIntervalTicks;
            const auto centroid = squadCentroid(world, squad);
            const bool retreating = squad.objectiveKind == SquadObjectiveKind::Retreat ||
                squadHealthPermille(world, squad) <= squad.retreatHealthPermille;
            const auto target = retreating || squad.objectiveKind == SquadObjectiveKind::Hold
                ? ecs::Entity{} : chooseSquadTarget(world, squad, centroid);
            if (target != squad.focusTarget) {
                squad.focusTarget = target;
                events_.push_back(
                    {nextTick, G3EventType::SquadTargetChanged, {}, target,
                     squad.id, 0, centroid});
            }
            std::vector<std::uint64_t> ids;
            ids.reserve(squad.members.size());
            for (const auto member : squad.members) ids.push_back(packEntity(member));
            const auto slots = FormationPlanner::assign(squad.formation, std::move(ids));
            auto& sequence = ensureTeamSequence(squad.teamId);
            const auto anchor = retreating ? squad.retreatPoint : squad.objective;
            for (std::size_t index = 0; index < squad.members.size(); ++index) {
                if (sequence.nextSequence == std::numeric_limits<std::uint32_t>::max()) {
                    break;
                }
                TickCommand command;
                command.targetTick = nextTick;
                command.issuer = squad.teamId;
                command.sequence = sequence.nextSequence++;
                command.subject = squad.members[index];
                if (target.valid()) {
                    command.type = CommandType::Attack;
                    command.targetEntity = target;
                } else if (squad.objectiveKind == SquadObjectiveKind::Hold) {
                    command.type = CommandType::HoldPosition;
                } else {
                    command.type = retreating ? CommandType::Move : CommandType::AttackMove;
                    const auto offsetX = index < slots.size()
                        ? slots[index].offsetXQ16 / FixedPosition2D::kOne : 0;
                    const auto offsetY = index < slots.size()
                        ? slots[index].offsetYQ16 / FixedPosition2D::kOne : 0;
                    command.targetX = anchor.x + offsetX;
                    command.targetY = anchor.y + offsetY;
                }
                (void)base_.submitDetailed(command);
                events_.push_back(
                    {nextTick, G3EventType::SquadCommandIssued,
                     command.subject, command.targetEntity, squad.id,
                     static_cast<std::int32_t>(command.type),
                     {command.targetX, command.targetY}});
            }
        }
    }

    std::uint64_t contentHash() const {
        foundation::CanonicalHash hash;
        appendContentHash(hash);
        return hash.Value();
    }

    void appendContentHash(foundation::CanonicalHash& hash) const {
        hash.WriteString("rts.g3-content.v1");
        hash.WriteU32(static_cast<std::uint32_t>(
            projectileDefinitions_.values().size()));
        for (const auto& value : projectileDefinitions_.values()) {
            hash.WriteU32(value.id);
            hash.WriteU32(value.speedQ16);
            hash.WriteU32(value.lifetimeTicks);
            hash.WriteU32(value.hitRadiusQ16);
            hash.WriteI32(value.damage);
            hash.WriteU8(static_cast<std::uint8_t>(value.damageType));
            hash.WriteU32(value.splashRadiusQ16);
            hash.WriteBool(value.homing);
            hash.WriteBool(value.friendlyFire);
            hash.WriteU32(value.statusEffectId);
        }
        hash.WriteU32(static_cast<std::uint32_t>(
            statusDefinitions_.values().size()));
        for (const auto& value : statusDefinitions_.values()) {
            hash.WriteU32(value.id);
            hash.WriteU32(value.durationTicks);
            hash.WriteU32(value.periodTicks);
            hash.WriteU16(value.maxStacks);
            hash.WriteU8(static_cast<std::uint8_t>(value.stacking));
            hash.WriteI32(value.periodicHealthDelta);
            hash.WriteU8(static_cast<std::uint8_t>(value.periodicDamageType));
            hash.WriteU16(value.moveScalePermille);
            hash.WriteU16(value.damageScalePermille);
            hash.WriteI32(value.armorAdd);
            hash.WriteBool(value.stunned);
        }
        hash.WriteU32(static_cast<std::uint32_t>(
            abilityDefinitions_.values().size()));
        for (const auto& value : abilityDefinitions_.values()) {
            hash.WriteU32(value.id);
            hash.WriteU32(value.cooldownTicks);
            hash.WriteU32(value.castTicks);
            hash.WriteU32(value.rangeQ16);
            hash.WriteU8(static_cast<std::uint8_t>(value.targetKind));
            hash.WriteBool(value.targetAllies);
            hash.WriteBool(value.targetEnemies);
            hash.WriteU32(static_cast<std::uint32_t>(value.effects.size()));
            for (const auto& effect : value.effects) {
                hash.WriteU8(static_cast<std::uint8_t>(effect.kind));
                hash.WriteI32(effect.amount);
                hash.WriteU32(effect.radiusQ16);
                hash.WriteU8(static_cast<std::uint8_t>(effect.damageType));
                hash.WriteU32(effect.projectileDefinitionId);
                hash.WriteU32(effect.statusEffectId);
            }
        }
        appendBindingsHash(hash, unitProjectileBindings_);
        appendBindingsHash(hash, buildingProjectileBindings_);
    }

    void appendStateHash(foundation::CanonicalHash& hash) const {
        hash.WriteU32(static_cast<std::uint32_t>(abilityCommands_.size()));
        for (const auto& value : abilityCommands_) hashAbilityCommand(hash, value);
        hash.WriteU32(static_cast<std::uint32_t>(projectiles_.size()));
        for (const auto& value : projectiles_) hashProjectile(hash, value);
        hash.WriteU32(static_cast<std::uint32_t>(statuses_.size()));
        for (const auto& value : statuses_) hashStatus(hash, value);
        hash.WriteU32(static_cast<std::uint32_t>(loadouts_.size()));
        for (const auto& value : loadouts_) hashLoadout(hash, value);
        hash.WriteU32(static_cast<std::uint32_t>(casts_.size()));
        for (const auto& value : casts_) hashCast(hash, value);
        hash.WriteU32(static_cast<std::uint32_t>(entityProjectileBindings_.size()));
        for (const auto& value : entityProjectileBindings_) {
            hashEntity(hash, value.entity);
            hash.WriteU32(value.projectileDefinitionId);
        }
        hash.WriteU32(static_cast<std::uint32_t>(squads_.size()));
        for (const auto& value : squads_) hashSquad(hash, value);
        hash.WriteU32(static_cast<std::uint32_t>(teamSequences_.size()));
        for (const auto& value : teamSequences_) {
            hash.WriteU32(value.teamId);
            hash.WriteU32(value.nextSequence);
        }
        hash.WriteU64(nextProjectileId_);
        hash.WriteU64(nextStatusInstanceId_);
        hash.WriteU64(nextCastId_);
        hash.WriteU32(nextSquadId_);
    }

    static void appendBindingsHash(
        foundation::CanonicalHash& hash,
        const std::vector<ProjectileBinding>& values) {
        hash.WriteU32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) {
            hash.WriteU32(value.definitionId);
            hash.WriteU32(value.projectileDefinitionId);
        }
    }

    static void hashAbilityCommand(
        foundation::CanonicalHash& hash,
        const AbilityCommand& value) {
        hash.WriteU64(value.targetTick);
        hash.WriteU32(value.issuer);
        hash.WriteU32(value.sequence);
        hashEntity(hash, value.caster);
        hash.WriteU32(value.abilityId);
        hashEntity(hash, value.targetEntity);
        hash.WriteI32(value.targetPoint.x);
        hash.WriteI32(value.targetPoint.y);
    }

    static void hashProjectile(
        foundation::CanonicalHash& hash,
        const ProjectileState& value) {
        hash.WriteU64(value.id);
        hash.WriteU32(value.definitionId);
        hashEntity(hash, value.source);
        hashEntity(hash, value.target);
        hash.WriteU32(value.sourceTeam);
        hash.WriteI32(value.position.x);
        hash.WriteI32(value.position.y);
        hash.WriteI32(value.targetPosition.x);
        hash.WriteI32(value.targetPosition.y);
        hash.WriteU8(static_cast<std::uint8_t>(value.facing));
        hash.WriteU32(value.remainingTicks);
    }

    static void hashStatus(
        foundation::CanonicalHash& hash,
        const StatusEffectState& value) {
        hash.WriteU64(value.instanceId);
        hash.WriteU32(value.definitionId);
        hashEntity(hash, value.source);
        hashEntity(hash, value.target);
        hash.WriteU16(value.stacks);
        hash.WriteU64(value.expiresAtTick);
        hash.WriteU64(value.nextPeriodicTick);
    }

    static void hashLoadout(
        foundation::CanonicalHash& hash,
        const AbilityLoadoutState& value) {
        hashEntity(hash, value.entity);
        hash.WriteU32(static_cast<std::uint32_t>(value.abilities.size()));
        for (const auto ability : value.abilities) hash.WriteU32(ability);
        hash.WriteU32(static_cast<std::uint32_t>(value.cooldowns.size()));
        for (const auto& cooldown : value.cooldowns) {
            hash.WriteU32(cooldown.abilityId);
            hash.WriteU64(cooldown.readyTick);
        }
    }

    static void hashCast(
        foundation::CanonicalHash& hash,
        const AbilityCastState& value) {
        hash.WriteU64(value.castId);
        hashEntity(hash, value.caster);
        hash.WriteU32(value.issuer);
        hash.WriteU32(value.abilityId);
        hashEntity(hash, value.targetEntity);
        hash.WriteI32(value.targetPoint.x);
        hash.WriteI32(value.targetPoint.y);
        hash.WriteU64(value.completeTick);
    }

    static void hashSquad(
        foundation::CanonicalHash& hash,
        const SquadState& value) {
        hash.WriteU32(value.id);
        hash.WriteU32(value.teamId);
        hash.WriteU8(static_cast<std::uint8_t>(value.formation));
        hash.WriteU8(static_cast<std::uint8_t>(value.objectiveKind));
        hash.WriteI32(value.objective.x);
        hash.WriteI32(value.objective.y);
        hash.WriteI32(value.retreatPoint.x);
        hash.WriteI32(value.retreatPoint.y);
        hashEntity(hash, value.focusTarget);
        hash.WriteU32(value.thinkIntervalTicks);
        hash.WriteU64(value.nextThinkTick);
        hash.WriteU16(value.retreatHealthPermille);
        hash.WriteI32(value.defenseRadius);
        hash.WriteU32(static_cast<std::uint32_t>(value.members.size()));
        for (const auto member : value.members) hashEntity(hash, member);
    }

    bool boundedState() const noexcept {
        return abilityCommands_.size() <= kMaximumEntries &&
               projectiles_.size() <= kMaximumEntries &&
               statuses_.size() <= kMaximumEntries &&
               loadouts_.size() <= kMaximumEntries &&
               casts_.size() <= kMaximumEntries &&
               entityProjectileBindings_.size() <= kMaximumEntries &&
               squads_.size() <= kMaximumEntries &&
               teamSequences_.size() <= kMaximumEntries;
    }

    static void writeAbilityCommands(
        foundation::BinaryWriter& writer,
        const std::vector<AbilityCommand>& values) {
        writer.writeU32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) {
            writer.writeU64(value.targetTick);
            writer.writeU32(value.issuer);
            writer.writeU32(value.sequence);
            writeEntity(writer, value.caster);
            writer.writeU32(value.abilityId);
            writeEntity(writer, value.targetEntity);
            writer.writeI32(value.targetPoint.x);
            writer.writeI32(value.targetPoint.y);
        }
    }

    void writeAbilityCommands(foundation::BinaryWriter& writer) const {
        writeAbilityCommands(writer, abilityCommands_);
    }

    static bool readAbilityCommands(
        foundation::BinaryReader& reader,
        std::vector<AbilityCommand>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        values.resize(count);
        for (auto& value : values) {
            if (!reader.readU64(value.targetTick) ||
                !reader.readU32(value.issuer) ||
                !reader.readU32(value.sequence) ||
                !readEntity(reader, value.caster) ||
                !reader.readU32(value.abilityId) ||
                !readEntity(reader, value.targetEntity) ||
                !reader.readI32(value.targetPoint.x) ||
                !reader.readI32(value.targetPoint.y)) {
                return false;
            }
        }
        return true;
    }

    void writeProjectiles(foundation::BinaryWriter& writer) const {
        writer.writeU32(static_cast<std::uint32_t>(projectiles_.size()));
        for (const auto& value : projectiles_) {
            writer.writeU64(value.id);
            writer.writeU32(value.definitionId);
            writeEntity(writer, value.source);
            writeEntity(writer, value.target);
            writer.writeU32(value.sourceTeam);
            writer.writeI32(value.position.x);
            writer.writeI32(value.position.y);
            writer.writeI32(value.targetPosition.x);
            writer.writeI32(value.targetPosition.y);
            writer.writeU8(static_cast<std::uint8_t>(value.facing));
            writer.writeU32(value.remainingTicks);
        }
    }

    static bool readProjectiles(
        foundation::BinaryReader& reader,
        std::vector<ProjectileState>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        values.resize(count);
        for (auto& value : values) {
            std::uint8_t facing = 0;
            if (!reader.readU64(value.id) ||
                !reader.readU32(value.definitionId) ||
                !readEntity(reader, value.source) ||
                !readEntity(reader, value.target) ||
                !reader.readU32(value.sourceTeam) ||
                !reader.readI32(value.position.x) ||
                !reader.readI32(value.position.y) ||
                !reader.readI32(value.targetPosition.x) ||
                !reader.readI32(value.targetPosition.y) ||
                !reader.readU8(facing) ||
                facing > static_cast<std::uint8_t>(Facing16::EastNorthEast) ||
                !reader.readU32(value.remainingTicks)) {
                return false;
            }
            value.facing = static_cast<Facing16>(facing);
        }
        return true;
    }

    void writeStatuses(foundation::BinaryWriter& writer) const {
        writer.writeU32(static_cast<std::uint32_t>(statuses_.size()));
        for (const auto& value : statuses_) {
            writer.writeU64(value.instanceId);
            writer.writeU32(value.definitionId);
            writeEntity(writer, value.source);
            writeEntity(writer, value.target);
            writer.writeU16(value.stacks);
            writer.writeU64(value.expiresAtTick);
            writer.writeU64(value.nextPeriodicTick);
        }
    }

    static bool readStatuses(
        foundation::BinaryReader& reader,
        std::vector<StatusEffectState>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        values.resize(count);
        for (auto& value : values) {
            if (!reader.readU64(value.instanceId) ||
                !reader.readU32(value.definitionId) ||
                !readEntity(reader, value.source) ||
                !readEntity(reader, value.target) ||
                !reader.readU16(value.stacks) ||
                !reader.readU64(value.expiresAtTick) ||
                !reader.readU64(value.nextPeriodicTick)) {
                return false;
            }
        }
        return true;
    }

    void writeLoadouts(foundation::BinaryWriter& writer) const {
        writer.writeU32(static_cast<std::uint32_t>(loadouts_.size()));
        for (const auto& value : loadouts_) {
            writeEntity(writer, value.entity);
            writer.writeU32(static_cast<std::uint32_t>(value.abilities.size()));
            for (const auto ability : value.abilities) writer.writeU32(ability);
            writer.writeU32(static_cast<std::uint32_t>(value.cooldowns.size()));
            for (const auto& cooldown : value.cooldowns) {
                writer.writeU32(cooldown.abilityId);
                writer.writeU64(cooldown.readyTick);
            }
        }
    }

    static bool readLoadouts(
        foundation::BinaryReader& reader,
        std::vector<AbilityLoadoutState>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        values.resize(count);
        for (auto& value : values) {
            std::uint32_t abilityCount = 0;
            std::uint32_t cooldownCount = 0;
            if (!readEntity(reader, value.entity) ||
                !reader.readU32(abilityCount) || abilityCount > kMaximumEntries) {
                return false;
            }
            value.abilities.resize(abilityCount);
            for (auto& ability : value.abilities) {
                if (!reader.readU32(ability)) return false;
            }
            if (!reader.readU32(cooldownCount) || cooldownCount > kMaximumEntries) {
                return false;
            }
            value.cooldowns.resize(cooldownCount);
            for (auto& cooldown : value.cooldowns) {
                if (!reader.readU32(cooldown.abilityId) ||
                    !reader.readU64(cooldown.readyTick)) {
                    return false;
                }
            }
        }
        return true;
    }

    void writeCasts(foundation::BinaryWriter& writer) const {
        writer.writeU32(static_cast<std::uint32_t>(casts_.size()));
        for (const auto& value : casts_) {
            writer.writeU64(value.castId);
            writeEntity(writer, value.caster);
            writer.writeU32(value.issuer);
            writer.writeU32(value.abilityId);
            writeEntity(writer, value.targetEntity);
            writer.writeI32(value.targetPoint.x);
            writer.writeI32(value.targetPoint.y);
            writer.writeU64(value.completeTick);
        }
    }

    static bool readCasts(
        foundation::BinaryReader& reader,
        std::vector<AbilityCastState>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        values.resize(count);
        for (auto& value : values) {
            if (!reader.readU64(value.castId) ||
                !readEntity(reader, value.caster) ||
                !reader.readU32(value.issuer) ||
                !reader.readU32(value.abilityId) ||
                !readEntity(reader, value.targetEntity) ||
                !reader.readI32(value.targetPoint.x) ||
                !reader.readI32(value.targetPoint.y) ||
                !reader.readU64(value.completeTick)) {
                return false;
            }
        }
        return true;
    }

    static void writeBindings(
        foundation::BinaryWriter& writer,
        const std::vector<EntityProjectileBinding>& values) {
        writer.writeU32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) {
            writeEntity(writer, value.entity);
            writer.writeU32(value.projectileDefinitionId);
        }
    }

    static bool readEntityBindings(
        foundation::BinaryReader& reader,
        std::vector<EntityProjectileBinding>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        values.resize(count);
        for (auto& value : values) {
            if (!readEntity(reader, value.entity) ||
                !reader.readU32(value.projectileDefinitionId)) {
                return false;
            }
        }
        return true;
    }

    void writeSquads(foundation::BinaryWriter& writer) const {
        writer.writeU32(static_cast<std::uint32_t>(squads_.size()));
        for (const auto& value : squads_) {
            writer.writeU32(value.id);
            writer.writeU32(value.teamId);
            writer.writeU8(static_cast<std::uint8_t>(value.formation));
            writer.writeU8(static_cast<std::uint8_t>(value.objectiveKind));
            writer.writeI32(value.objective.x);
            writer.writeI32(value.objective.y);
            writer.writeI32(value.retreatPoint.x);
            writer.writeI32(value.retreatPoint.y);
            writeEntity(writer, value.focusTarget);
            writer.writeU32(value.thinkIntervalTicks);
            writer.writeU64(value.nextThinkTick);
            writer.writeU16(value.retreatHealthPermille);
            writer.writeI32(value.defenseRadius);
            writer.writeU32(static_cast<std::uint32_t>(value.members.size()));
            for (const auto member : value.members) writeEntity(writer, member);
        }
    }

    static bool readSquads(
        foundation::BinaryReader& reader,
        std::vector<SquadState>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        values.resize(count);
        for (auto& value : values) {
            std::uint8_t formation = 0;
            std::uint8_t objective = 0;
            std::uint32_t memberCount = 0;
            if (!reader.readU32(value.id) ||
                !reader.readU32(value.teamId) ||
                !reader.readU8(formation) ||
                formation > static_cast<std::uint8_t>(FormationKind::Wedge) ||
                !reader.readU8(objective) ||
                objective > static_cast<std::uint8_t>(SquadObjectiveKind::Hold) ||
                !reader.readI32(value.objective.x) ||
                !reader.readI32(value.objective.y) ||
                !reader.readI32(value.retreatPoint.x) ||
                !reader.readI32(value.retreatPoint.y) ||
                !readEntity(reader, value.focusTarget) ||
                !reader.readU32(value.thinkIntervalTicks) ||
                !reader.readU64(value.nextThinkTick) ||
                !reader.readU16(value.retreatHealthPermille) ||
                !reader.readI32(value.defenseRadius) ||
                !reader.readU32(memberCount) || memberCount > kMaximumEntries) {
                return false;
            }
            value.formation = static_cast<FormationKind>(formation);
            value.objectiveKind = static_cast<SquadObjectiveKind>(objective);
            value.members.resize(memberCount);
            for (auto& member : value.members) {
                if (!readEntity(reader, member)) return false;
            }
        }
        return true;
    }

    void writeTeamSequences(foundation::BinaryWriter& writer) const {
        writer.writeU32(static_cast<std::uint32_t>(teamSequences_.size()));
        for (const auto& value : teamSequences_) {
            writer.writeU32(value.teamId);
            writer.writeU32(value.nextSequence);
        }
    }

    static bool readTeamSequences(
        foundation::BinaryReader& reader,
        std::vector<TeamSequenceState>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > kMaximumEntries) return false;
        values.resize(count);
        for (auto& value : values) {
            if (!reader.readU32(value.teamId) ||
                !reader.readU32(value.nextSequence)) {
                return false;
            }
        }
        return true;
    }

    bool validateState(
        const std::vector<AbilityCommand>& commands,
        const std::vector<ProjectileState>& projectiles,
        const std::vector<StatusEffectState>& statuses,
        const std::vector<AbilityLoadoutState>& loadouts,
        const std::vector<AbilityCastState>& casts,
        const std::vector<EntityProjectileBinding>& entityBindings,
        const std::vector<SquadState>& squads,
        const std::vector<TeamSequenceState>& sequences,
        std::uint64_t nextProjectile,
        std::uint64_t nextStatus,
        std::uint64_t nextCast,
        std::uint32_t nextSquad) const {
        std::uint64_t maximumProjectile = 0;
        std::uint64_t maximumStatus = 0;
        std::uint64_t maximumCast = 0;
        std::uint32_t maximumSquad = 0;
        for (std::size_t index = 1; index < commands.size(); ++index) {
            const auto& previous = commands[index - 1];
            const auto& current = commands[index];
            if (std::tie(current.targetTick, current.issuer, current.sequence) <
                std::tie(previous.targetTick, previous.issuer, previous.sequence)) {
                return false;
            }
        }
        for (const auto& value : commands) {
            if (value.issuer == 0 || value.sequence == 0 ||
                !value.caster.valid() || !abilityDefinitions_.find(value.abilityId)) {
                return false;
            }
        }
        std::uint64_t previousProjectile = 0;
        for (const auto& value : projectiles) {
            if (value.id == 0 || value.id <= previousProjectile ||
                value.sourceTeam == 0 || value.remainingTicks == 0 ||
                !projectileDefinitions_.find(value.definitionId)) {
                return false;
            }
            previousProjectile = value.id;
            maximumProjectile = value.id;
        }
        for (const auto& value : statuses) {
            const auto* definition = statusDefinitions_.find(value.definitionId);
            if (value.instanceId == 0 || !definition || !value.target.valid() ||
                value.stacks == 0 || value.stacks > definition->maxStacks) {
                return false;
            }
            maximumStatus = std::max(maximumStatus, value.instanceId);
        }
        ecs::Entity previousLoadout{};
        bool hasLoadout = false;
        for (const auto& value : loadouts) {
            if (!value.entity.valid() || (hasLoadout && !(previousLoadout < value.entity)) ||
                !std::is_sorted(value.abilities.begin(), value.abilities.end()) ||
                std::adjacent_find(value.abilities.begin(), value.abilities.end()) !=
                    value.abilities.end()) {
                return false;
            }
            for (const auto ability : value.abilities) {
                if (!abilityDefinitions_.find(ability)) return false;
            }
            previousLoadout = value.entity;
            hasLoadout = true;
        }
        for (const auto& value : casts) {
            if (value.castId == 0 || !value.caster.valid() || value.issuer == 0 ||
                !abilityDefinitions_.find(value.abilityId)) {
                return false;
            }
            maximumCast = std::max(maximumCast, value.castId);
        }
        ecs::Entity previousBinding{};
        bool hasBinding = false;
        for (const auto& value : entityBindings) {
            if (!value.entity.valid() || (hasBinding && !(previousBinding < value.entity)) ||
                !projectileDefinitions_.find(value.projectileDefinitionId)) {
                return false;
            }
            previousBinding = value.entity;
            hasBinding = true;
        }
        std::uint32_t previousSquad = 0;
        for (const auto& value : squads) {
            if (value.id == 0 || value.id <= previousSquad || value.teamId == 0 ||
                value.thinkIntervalTicks == 0 || value.members.empty() ||
                !std::is_sorted(value.members.begin(), value.members.end()) ||
                std::adjacent_find(value.members.begin(), value.members.end()) !=
                    value.members.end()) {
                return false;
            }
            previousSquad = value.id;
            maximumSquad = value.id;
        }
        std::uint32_t previousTeam = 0;
        for (const auto& value : sequences) {
            if (value.teamId == 0 || value.teamId <= previousTeam ||
                value.nextSequence < 0x60000000u) {
                return false;
            }
            previousTeam = value.teamId;
        }
        return nextProjectile >= maximumProjectile &&
               nextStatus >= maximumStatus &&
               nextCast >= maximumCast &&
               nextSquad >= maximumSquad;
    }

    RtsGameSession base_;
    DefinitionCatalog<ProjectileDefinition> projectileDefinitions_;
    DefinitionCatalog<StatusEffectDefinition> statusDefinitions_;
    DefinitionCatalog<AbilityDefinition> abilityDefinitions_;
    std::vector<ProjectileBinding> unitProjectileBindings_;
    std::vector<ProjectileBinding> buildingProjectileBindings_;
    std::vector<EntityProjectileBinding> entityProjectileBindings_;
    std::vector<AbilityCommand> abilityCommands_;
    std::vector<ProjectileState> projectiles_;
    std::vector<StatusEffectState> statuses_;
    std::vector<AbilityLoadoutState> loadouts_;
    std::vector<AbilityCastState> casts_;
    std::vector<SquadState> squads_;
    std::vector<TeamSequenceState> teamSequences_;
    std::vector<ecs::Entity> controlledEntities_;
    std::vector<G3Event> events_;
    std::uint64_t nextProjectileId_{};
    std::uint64_t nextStatusInstanceId_{};
    std::uint64_t nextCastId_{};
    std::uint32_t nextSquadId_{};
};

} // namespace rts::gameplay
