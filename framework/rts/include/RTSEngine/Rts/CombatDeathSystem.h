#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/Combat.h>
#include <RTSEngine/Rts/GameplayModifierSystem.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::gameplay {

struct CombatDeathSystemDependencies {
    CombatRuntime& combat;
    ecs::EntityCommandBuffer& structuralCommands;
    BaseBuildingRuntime& building;
    TeamEconomyRuntime& economy;
    const GameplayModifierSystem& modifiers;
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
        DeathContext deathContext{&world, &context, dependencies};
        dependencies.combat.advance<Position>(
            context,
            dependencies.structuralCommands,
            world,
            &deathContext,
            &handleDeathCallback);
        forwardEvents(dependencies);
    }

private:
    struct DeathContext final {
        ecs::World* world{};
        const ecs::SystemContext* context{};
        CombatDeathSystemDependencies dependencies;
    };

    static void handleDeathCallback(
        void* rawContext,
        ecs::Entity victim,
        ecs::Entity killer) {
        auto& value = *static_cast<DeathContext*>(rawContext);
        handleDeath(
            *value.world,
            *value.context,
            victim,
            killer,
            value.dependencies);
    }

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
            dependencies.economy.release(
                site->ownerTeam,
                kPrimaryResourceType,
                site->reservedCost);
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
        const auto* victimTeam = world.try_get<Team>(victim);
        if (production && victimTeam) {
            for (const auto& item : production->items) {
                dependencies.economy.release(
                    victimTeam->id,
                    kPrimaryResourceType,
                    item.reservedCost);
            }
        }

        const auto* bounty = world.try_get<Bounty>(victim);
        const auto* killerTeam = world.try_get<Team>(killer);
        if (bounty && bounty->amount > 0 && killerTeam && victimTeam &&
            killerTeam->id != victimTeam->id) {
            const auto awarded = dependencies.modifiers.bounty(
                killerTeam->id, bounty->amount);
            if (awarded > 0 && dependencies.economy.credit(
                    killerTeam->id,
                    kPrimaryResourceType,
                    awarded)) {
                dependencies.deathSideEffects.push_back(
                    {context.tick,
                     DomainEventType::BountyAwarded,
                     killer,
                     kPrimaryResourceType,
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
