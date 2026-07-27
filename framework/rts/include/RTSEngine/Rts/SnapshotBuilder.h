#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/GameplayModifierSystem.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <RTSEngine/Rts/Vision.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>

namespace rts::gameplay {

struct SnapshotBuilderDependencies {
    const TeamEconomyRuntime& economies;
    std::uint32_t playerTeamId{};
    const GameplayModifierSystem& modifiers;
    const NavigationGrid& navigation;
    const TickCommandStream& commands;
    WorldSnapshot& snapshot;
    const VisionRuntime* vision{};
};

class SnapshotBuilder final {
public:
    static void build(
        const ecs::World& world,
        std::uint64_t tick,
        SnapshotBuilderDependencies dependencies) {
        auto& snapshot = dependencies.snapshot;
        snapshot.tick = tick;
        snapshot.resources = dependencies.economies.resources(
            dependencies.playerTeamId);
        dependencies.economies.buildSnapshot(snapshot.teamEconomies);
        snapshot.teamModifiers = dependencies.modifiers.entries();
        snapshot.commandCommittedThrough =
            dependencies.commands.committedThrough();
        snapshot.pendingCommands = static_cast<std::uint32_t>(
            dependencies.commands.pending());
        snapshot.entities.clear();
        snapshot.visibility.clear();
        snapshot.visibilityWidth = 0;
        snapshot.visibilityHeight = 0;
        if (dependencies.vision) {
            snapshot.visibilityWidth = dependencies.vision->width();
            snapshot.visibilityHeight = dependencies.vision->height();
            dependencies.vision->buildSnapshot(snapshot.visibility);
        }

        foundation::CanonicalHash hash;
        hash.WriteU64(tick);
        dependencies.economies.appendHash(hash);
        dependencies.modifiers.appendHash(hash);
        hash.WriteU64(dependencies.commands.committedThrough());
        hash.WriteU32(static_cast<std::uint32_t>(
            dependencies.commands.pending()));
        dependencies.commands.forEachPending(
            [&](const TickCommand& command) {
                hashCommand(hash, command);
            });
        hash.WriteU64(dependencies.navigation.revision());
        for (const auto blocked : dependencies.navigation.blockers()) {
            hash.WriteU8(blocked);
        }
        if (dependencies.vision) {
            dependencies.vision->appendExploredHash(hash);
        }

        const bool includeVision = dependencies.vision != nullptr;
        appendUnits(world, hash, snapshot, includeVision);
        appendConstruction(world, hash, snapshot, includeVision);
        appendBuildings(world, hash, snapshot, includeVision);

        std::sort(
            snapshot.entities.begin(),
            snapshot.entities.end(),
            [](const SnapshotEntity& a, const SnapshotEntity& b) {
                return a.entity < b.entity;
            });
        snapshot.worldHash = hash.Value();
    }

private:
    static void hashEntity(
        foundation::CanonicalHash& hash,
        ecs::Entity entity) {
        hash.WriteU32(entity.index);
        hash.WriteU32(entity.generation);
    }

    static void hashCommand(
        foundation::CanonicalHash& hash,
        const TickCommand& command) {
        hash.WriteU64(command.targetTick);
        hash.WriteU32(command.issuer);
        hash.WriteU32(command.sequence);
        hash.WriteU8(static_cast<std::uint8_t>(command.type));
        hashEntity(hash, command.subject);
        hash.WriteI32(command.targetX);
        hash.WriteI32(command.targetY);
        hash.WriteBool(command.append);
        hash.WriteU32(command.definitionId);
        hash.WriteU32(command.objectId);
        hashEntity(hash, command.targetEntity);
    }

    static void populateCombatSnapshot(
        const ecs::World& world,
        ecs::Entity entity,
        SnapshotEntity& snapshot) {
        if (const auto* team = world.try_get<Team>(entity)) {
            snapshot.teamId = team->id;
        }
        if (const auto* supply = world.try_get<UnitSupply>(entity)) {
            snapshot.supplyCost = supply->amount;
        }
        if (const auto* health = world.try_get<Health>(entity)) {
            snapshot.healthCurrent = health->current;
            snapshot.healthMaximum = health->maximum;
        }
        if (const auto* armor = world.try_get<Armor>(entity)) {
            snapshot.armor = armor->value;
        }
        if (const auto* weapon = world.try_get<Weapon>(entity)) {
            snapshot.cooldownRemaining = weapon->cooldownRemaining;
        }
        if (const auto* target = world.try_get<CombatTarget>(entity)) {
            snapshot.target = target->entity;
        }
        if (const auto* directive =
                world.try_get<CombatDirective>(entity)) {
            snapshot.combatMode = directive->mode;
        }
        if (const auto* vision = world.try_get<VisionSource>(entity)) {
            snapshot.visionRange = vision->range;
        }
    }

    static void hashCombat(
        foundation::CanonicalHash& hash,
        const ecs::World& world,
        ecs::Entity entity) {
        const auto* team = world.try_get<Team>(entity);
        hash.WriteBool(team != nullptr);
        if (team) hash.WriteU32(team->id);

        const auto* supply = world.try_get<UnitSupply>(entity);
        hash.WriteBool(supply != nullptr);
        if (supply) hash.WriteU32(supply->amount);

        const auto* health = world.try_get<Health>(entity);
        hash.WriteBool(health != nullptr);
        if (health) {
            hash.WriteI32(health->current);
            hash.WriteI32(health->maximum);
        }

        const auto* armor = world.try_get<Armor>(entity);
        hash.WriteBool(armor != nullptr);
        if (armor) hash.WriteI32(armor->value);

        const auto* weapon = world.try_get<Weapon>(entity);
        hash.WriteBool(weapon != nullptr);
        if (weapon) {
            hash.WriteI32(weapon->damage);
            hash.WriteI32(weapon->range);
            hash.WriteU32(weapon->cooldownTicks);
            hash.WriteU32(weapon->cooldownRemaining);
        }

        const auto* target = world.try_get<CombatTarget>(entity);
        hash.WriteBool(target != nullptr);
        if (target) hashEntity(hash, target->entity);

        const auto* directive = world.try_get<CombatDirective>(entity);
        hash.WriteBool(directive != nullptr);
        if (directive) {
            hash.WriteU8(static_cast<std::uint8_t>(directive->mode));
            hashEntity(hash, directive->forcedTarget);
        }

        const auto* bounty = world.try_get<Bounty>(entity);
        hash.WriteBool(bounty != nullptr);
        if (bounty) hash.WriteI32(bounty->amount);

        const auto* tunable = world.try_get<TunableStats>(entity);
        hash.WriteBool(tunable != nullptr);
        if (tunable) {
            hash.WriteBool(tunable->building);
            hash.WriteI32(tunable->baseMoveSpeed);
            hash.WriteI32(tunable->baseCombat.maximumHealth);
            hash.WriteI32(tunable->baseCombat.armor);
            hash.WriteI32(tunable->baseCombat.weaponDamage);
            hash.WriteI32(tunable->baseCombat.weaponRange);
            hash.WriteU32(tunable->baseCombat.cooldownTicks);
            hash.WriteI32(tunable->baseCombat.bounty);
        }
    }

    static void hashVision(
        foundation::CanonicalHash& hash,
        const ecs::World& world,
        ecs::Entity entity) {
        const auto* vision = world.try_get<VisionSource>(entity);
        hash.WriteBool(vision != nullptr);
        if (vision) hash.WriteI32(vision->range);
    }

    static void hashFootprint(
        foundation::CanonicalHash& hash,
        const BuildingFootprint& footprint) {
        hash.WriteI32(footprint.origin.x);
        hash.WriteI32(footprint.origin.y);
        hash.WriteI32(footprint.width);
        hash.WriteI32(footprint.height);
    }

    static void hashUnit(
        foundation::CanonicalHash& hash,
        const ecs::World& world,
        ecs::Entity entity,
        const Position& position,
        const OrderQueue& queue,
        const MovementAgent& agent,
        bool moving,
        bool includeVision) {
        hashEntity(hash, entity);
        hash.WriteI32(position.x);
        hash.WriteI32(position.y);
        hash.WriteBool(moving);
        hash.WriteU32(static_cast<std::uint32_t>(queue.pending.size()));
        for (const auto& order : queue.pending) {
            hash.WriteU8(static_cast<std::uint8_t>(order.type));
            hash.WriteI32(order.target.x);
            hash.WriteI32(order.target.y);
        }
        hash.WriteU64(agent.pathRevision);
        hash.WriteU64(static_cast<std::uint64_t>(agent.nextPoint));
        hash.WriteU64(static_cast<std::uint64_t>(agent.path.size()));
        for (const auto point : agent.path) {
            hash.WriteI32(point.x);
            hash.WriteI32(point.y);
        }
        hash.WriteI32(agent.pathGoal.x);
        hash.WriteI32(agent.pathGoal.y);
        hash.WriteBool(agent.hasPathGoal);
        hash.WriteBool(agent.combatPath);
        hash.WriteBool(agent.flowFieldPath);
        hashEntity(hash, agent.chaseTarget);
        hash.WriteI32(agent.chaseTargetPosition.x);
        hash.WriteI32(agent.chaseTargetPosition.y);
        hash.WriteU32(agent.blockedTicks);
        hash.WriteU32(agent.yieldOrdinal);
        hashCombat(hash, world, entity);
        if (includeVision) hashVision(hash, world, entity);
    }

    static void appendUnits(
        const ecs::World& world,
        foundation::CanonicalHash& hash,
        WorldSnapshot& snapshot,
        bool includeVision) {
        world.eachRef<Position, OrderQueue, MovementAgent>(
            [&](ecs::Entity entity,
                const Position& position,
                const OrderQueue& queue,
                const MovementAgent& agent) {
                const bool moving =
                    !agent.path.empty() ||
                    agent.flowFieldPath ||
                    !queue.pending.empty() ||
                    agent.combatPath;

                SnapshotEntity value;
                value.entity = entity;
                value.x = position.x;
                value.y = position.y;
                value.moving = moving;
                value.queuedOrders = static_cast<std::uint32_t>(
                    queue.pending.size());
                value.kind = SnapshotKind::Unit;
                value.movementBlockedTicks = agent.blockedTicks;
                value.movementYieldOrdinal = agent.yieldOrdinal;
                populateCombatSnapshot(world, entity, value);
                snapshot.entities.push_back(value);

                hash.WriteU8(static_cast<std::uint8_t>(SnapshotKind::Unit));
                hashUnit(
                    hash,
                    world,
                    entity,
                    position,
                    queue,
                    agent,
                    moving,
                    includeVision);
            });
    }

    static void appendConstruction(
        const ecs::World& world,
        foundation::CanonicalHash& hash,
        WorldSnapshot& snapshot,
        bool includeVision) {
        world.eachRef<ConstructionSite, BuildingFootprint>(
            [&](ecs::Entity entity,
                const ConstructionSite& site,
                const BuildingFootprint& footprint) {
                SnapshotEntity value;
                value.entity = entity;
                value.x = footprint.origin.x;
                value.y = footprint.origin.y;
                value.kind = SnapshotKind::Construction;
                value.definitionId = site.definitionId;
                value.progressTicks = site.progressTicks;
                value.requiredTicks = site.requiredTicks;
                populateCombatSnapshot(world, entity, value);
                snapshot.entities.push_back(value);

                hash.WriteU8(
                    static_cast<std::uint8_t>(SnapshotKind::Construction));
                hashEntity(hash, entity);
                hash.WriteU32(site.id);
                hash.WriteU32(site.definitionId);
                hash.WriteI32(site.reservedCost);
                hash.WriteU32(site.progressTicks);
                hash.WriteU32(site.requiredTicks);
                hash.WriteU32(site.baseRequiredTicks);
                hash.WriteBool(site.producer);
                hash.WriteU32(site.ownerTeam);
                hashFootprint(hash, footprint);
                hashCombat(hash, world, entity);
                if (includeVision) hashVision(hash, world, entity);
            });
    }

    static void appendBuildings(
        const ecs::World& world,
        foundation::CanonicalHash& hash,
        WorldSnapshot& snapshot,
        bool includeVision) {
        world.eachRef<Building, BuildingFootprint>(
            [&](ecs::Entity entity,
                const Building& building,
                const BuildingFootprint& footprint) {
                const auto* queue = world.try_get<ProductionQueue>(entity);
                const auto queueSize = queue
                    ? static_cast<std::uint32_t>(queue->items.size())
                    : 0;

                SnapshotEntity value;
                value.entity = entity;
                value.x = footprint.origin.x;
                value.y = footprint.origin.y;
                value.kind = SnapshotKind::Building;
                value.definitionId = building.definitionId;
                value.productionQueueSize = queueSize;
                populateCombatSnapshot(world, entity, value);
                snapshot.entities.push_back(value);

                hash.WriteU8(
                    static_cast<std::uint8_t>(SnapshotKind::Building));
                hashEntity(hash, entity);
                hash.WriteU32(building.definitionId);
                hash.WriteBool(building.producer);
                hashFootprint(hash, footprint);
                hash.WriteU32(queueSize);
                if (queue) {
                    for (const auto& item : queue->items) {
                        hash.WriteU32(item.id);
                        hash.WriteU32(item.unitDefinitionId);
                        hash.WriteI32(item.reservedCost);
                        hash.WriteU32(item.supplyCost);
                        hash.WriteU32(item.progressTicks);
                        hash.WriteU32(item.requiredTicks);
                        hash.WriteU32(item.baseRequiredTicks);
                    }
                }
                const auto* rally = world.try_get<RallyPoint>(entity);
                hash.WriteBool(rally != nullptr);
                if (rally) {
                    hash.WriteI32(rally->point.x);
                    hash.WriteI32(rally->point.y);
                }
                hashCombat(hash, world, entity);
                if (includeVision) hashVision(hash, world, entity);
            });
    }
};

} // namespace rts::gameplay
