#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/Combat.h>
#include <RTSEngine/Rts/GameplayModifierSystem.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

struct CombatDeathSystemDependencies {
    CombatRuntime& combat;
    ecs::EntityCommandBuffer& structuralCommands;
    BaseBuildingRuntime& building;
    ResourceLedger& resources;
    const GameplayModifierSystem& modifiers;
    std::uint32_t playerTeamId;
    std::vector<DomainEvent>& events;
    std::vector<DomainEvent>& deathSideEffects;
};

class CombatDeathSystem final {
public:
    static void run(
        ecs::World& world,
        const ecs::SystemContext& context,
        CombatDeathSystemDependencies dependencies) {
        dependencies.deathSideEffects.clear();
        dependencies.combat.advance<Position>(
            context,
            dependencies.structuralCommands,
            world,
            [&world, &context, &dependencies](
                ecs::Entity victim,
                ecs::Entity killer) {
                handleDeath(
                    world, context, victim, killer, dependencies);
            });
        forwardEvents(dependencies);
    }

private:
    static void handleDeath(
        ecs::World& world,
        const ecs::SystemContext& context,
        ecs::Entity victim,
        ecs::Entity killer,
        CombatDeathSystemDependencies dependencies) {
        const auto* footprint =
            world.try_get<BuildingFootprint>(victim);
        if (footprint) {
            dependencies.building.releaseFootprint(*footprint);
        }

        const auto* site = world.try_get<ConstructionSite>(victim);
        if (site) {
            dependencies.resources.release(site->reservedCost);
            dependencies.deathSideEffects.push_back(
                {context.tick,
                 DomainEventType::ConstructionDestroyed,
                 victim,
                 site->id,
                 0,
                 killer,
                 0});
        }

        const auto* production =
            world.try_get<ProductionQueue>(victim);
        if (production) {
            for (const auto& item : production->items) {
                dependencies.resources.release(item.reservedCost);
            }
        }

        const auto* bounty = world.try_get<Bounty>(victim);
        const auto* killerTeam = world.try_get<Team>(killer);
        const auto* victimTeam = world.try_get<Team>(victim);
        if (bounty && bounty->amount > 0 && killerTeam && victimTeam &&
            killerTeam->id == dependencies.playerTeamId &&
            killerTeam->id != victimTeam->id) {
            const auto awarded = dependencies.modifiers.bounty(
                killerTeam->id, bounty->amount);
            if (awarded > 0) {
                const auto next = std::min<std::int64_t>(
                    std::numeric_limits<std::int32_t>::max(),
                    static_cast<std::int64_t>(
                        dependencies.resources.available) + awarded);
                dependencies.resources.available =
                    static_cast<std::int32_t>(next);
                dependencies.deathSideEffects.push_back(
                    {context.tick,
                     DomainEventType::BountyAwarded,
                     killer,
                     0,
                     0,
                     victim,
                     awarded});
            }
        }
    }

    static void forwardEvents(
        CombatDeathSystemDependencies dependencies) {
        for (const auto& event : dependencies.combat.events()) {
            switch (event.type) {
            case CombatEventType::TargetAcquired:
                dependencies.events.push_back(
                    {event.tick,
                     DomainEventType::TargetAcquired,
                     event.source,
                     0,
                     0,
                     event.target,
                     event.value});
                break;
            case CombatEventType::WeaponFired:
                dependencies.events.push_back(
                    {event.tick,
                     DomainEventType::WeaponFired,
                     event.source,
                     0,
                     0,
                     event.target,
                     event.value});
                break;
            case CombatEventType::DamageApplied:
                dependencies.events.push_back(
                    {event.tick,
                     DomainEventType::DamageApplied,
                     event.source,
                     0,
                     0,
                     event.target,
                     event.value});
                break;
            case CombatEventType::EntityDied:
                dependencies.events.push_back(
                    {event.tick,
                     DomainEventType::EntityDied,
                     event.target,
                     0,
                     0,
                     event.source,
                     0});
                break;
            }
        }
        dependencies.events.insert(
            dependencies.events.end(),
            dependencies.deathSideEffects.begin(),
            dependencies.deathSideEffects.end());
    }
};

} // namespace rts::gameplay
