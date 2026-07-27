#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/DefinitionCatalog.h>
#include <RTSEngine/Rts/EntityFactory.h>
#include <RTSEngine/Rts/GameplayModifierSystem.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

struct EconomyCommandDependencies {
    ecs::EntityCommandBuffer& structuralCommands;
    BaseBuildingRuntime& building;
    TeamEconomyRuntime& economies;
    GameplayModifierSystem& modifiers;
    const DefinitionCatalog<BuildingDefinition>& buildingDefinitions;
    const DefinitionCatalog<UnitDefinition>& unitDefinitions;
    NavigationGrid& navigation;
    GridPoint requiredPathStart;
    GridPoint requiredPathGoal;
    ProductionId& nextProductionId;
    std::vector<DomainEvent>& events;
};

class EconomySupplySystem final {
public:
    static void rebuild(
        const ecs::World& world,
        TeamEconomyRuntime& economies,
        const DefinitionCatalog<BuildingDefinition>& buildingDefinitions) {
        economies.beginSupplyRebuild();
        world.eachRef<Building, Team>(
            [&](ecs::Entity,
                const Building& building,
                const Team& team) {
                const auto* definition =
                    buildingDefinitions.find(building.definitionId);
                if (definition) {
                    economies.addSupplyCapacity(
                        team.id, definition->supplyProvided);
                }
            });
        world.eachRef<UnitSupply, Team>(
            [&](ecs::Entity,
                const UnitSupply& supply,
                const Team& team) {
                economies.addSupplyUsed(team.id, supply.amount);
            });
        world.eachRef<ProductionQueue, Team>(
            [&](ecs::Entity,
                const ProductionQueue& queue,
                const Team& team) {
                for (const auto& item : queue.items) {
                    economies.addSupplyReserved(team.id, item.supplyCost);
                }
            });
    }
};

class EconomyCommandSystem final {
public:
    static bool handles(CommandType type) noexcept {
        return type == CommandType::Build ||
               type == CommandType::CancelConstruction ||
               type == CommandType::Train ||
               type == CommandType::CancelProduction ||
               type == CommandType::SetRally;
    }

    static void process(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        EconomyCommandDependencies dependencies) {
        switch (command.type) {
        case CommandType::Build:
            beginConstruction(context, command, dependencies);
            break;
        case CommandType::CancelConstruction:
            cancelConstruction(world, context, command, dependencies);
            break;
        case CommandType::Train:
            beginProduction(world, context, command, dependencies);
            break;
        case CommandType::CancelProduction:
            cancelProduction(world, context, command, dependencies);
            break;
        case CommandType::SetRally:
            setRally(world, context, command, dependencies);
            break;
        default:
            break;
        }
    }

private:
    static bool owns(const ecs::World& world,
                     ecs::Entity entity,
                     std::uint32_t issuer) noexcept {
        const auto* team = world.try_get<Team>(entity);
        return issuer != 0 && team && team->id == issuer;
    }

    static void reject(const ecs::SystemContext& context,
                       const TickCommand& command,
                       CommandRejectionReason reason,
                       std::vector<DomainEvent>& events) {
        events.push_back(
            {context.tick,
             DomainEventType::CommandRejected,
             command.subject,
             command.objectId,
             static_cast<std::uint32_t>(reason),
             command.targetEntity,
             static_cast<std::int32_t>(command.type)});
    }

    static void beginConstruction(
        const ecs::SystemContext& context,
        const TickCommand& command,
        EconomyCommandDependencies dependencies) {
        if (command.issuer == 0) {
            reject(context, command, CommandRejectionReason::NotOwner,
                   dependencies.events);
            return;
        }

        const auto* definition =
            dependencies.buildingDefinitions.find(command.definitionId);
        BuildResult result;
        if (definition) {
            auto resolvedDefinition = *definition;
            resolvedDefinition.buildTicks =
                dependencies.modifiers.constructionTicks(
                    command.issuer, definition->buildTicks);
            result = dependencies.building.begin(
                context,
                dependencies.structuralCommands,
                resolvedDefinition,
                {command.targetX, command.targetY},
                dependencies.requiredPathStart,
                dependencies.requiredPathGoal,
                command.issuer,
                definition->buildTicks);
        } else {
            result = {false, BuildFailure::InvalidDefinition, 0};
        }
        dependencies.events.push_back(
            {context.tick,
             result.accepted
                 ? DomainEventType::ConstructionAccepted
                 : DomainEventType::ConstructionRejected,
             {},
             result.constructionId,
             static_cast<std::uint32_t>(result.failure)});
    }

    static void cancelConstruction(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        EconomyCommandDependencies dependencies) {
        bool found = false;
        bool owned = false;
        world.eachRef<ConstructionSite>(
            [&](ecs::Entity, ConstructionSite& site) {
                if (found || site.id != command.objectId) return;
                found = true;
                owned = site.ownerTeam == command.issuer &&
                        command.issuer != 0;
            });
        if (!found) {
            reject(context, command, CommandRejectionReason::InvalidEntity,
                   dependencies.events);
            return;
        }
        if (!owned) {
            reject(context, command, CommandRejectionReason::NotOwner,
                   dependencies.events);
            return;
        }

        const auto failure = dependencies.building.cancel(
            context,
            dependencies.structuralCommands,
            world,
            command.objectId);
        dependencies.events.push_back(
            {context.tick,
             failure == BuildFailure::None
                 ? DomainEventType::ConstructionCancelled
                 : DomainEventType::ConstructionRejected,
             {},
             command.objectId,
             static_cast<std::uint32_t>(failure)});
    }

    static void beginProduction(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        EconomyCommandDependencies dependencies) {
        if (!world.alive(command.subject)) {
            reject(context, command, CommandRejectionReason::InvalidEntity,
                   dependencies.events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(context, command, CommandRejectionReason::NotOwner,
                   dependencies.events);
            return;
        }

        auto* queue = world.try_get<ProductionQueue>(command.subject);
        const auto* building = world.try_get<Building>(command.subject);
        const auto* ownerTeam = world.try_get<Team>(command.subject);
        const auto* unitDefinition =
            dependencies.unitDefinitions.find(command.definitionId);
        if (!queue || !building || !building->producer || !ownerTeam) {
            reject(context, command, CommandRejectionReason::MissingCapability,
                   dependencies.events);
            return;
        }
        const auto* buildingDefinition =
            dependencies.buildingDefinitions.find(building->definitionId);
        if (!buildingDefinition || !buildingDefinition->producer) {
            reject(context, command, CommandRejectionReason::InvalidDefinition,
                   dependencies.events);
            return;
        }
        if (queue->items.size() >=
            buildingDefinition->productionQueueCapacity) {
            reject(context, command, CommandRejectionReason::QueueFull,
                   dependencies.events);
            return;
        }
        if (!buildingDefinition->trainableUnits.empty() &&
            std::find(
                buildingDefinition->trainableUnits.begin(),
                buildingDefinition->trainableUnits.end(),
                command.definitionId) ==
                buildingDefinition->trainableUnits.end()) {
            reject(context, command, CommandRejectionReason::UnsupportedUnit,
                   dependencies.events);
            return;
        }
        if (!unitDefinition || unitDefinition->id == 0 ||
            unitDefinition->cost < 0) {
            reject(context, command, CommandRejectionReason::InvalidDefinition,
                   dependencies.events);
            return;
        }
        if (!dependencies.economies.reserveSupply(
                ownerTeam->id, unitDefinition->supplyCost)) {
            reject(context, command, CommandRejectionReason::SupplyBlocked,
                   dependencies.events);
            return;
        }
        if (!dependencies.economies.reserveResources(
                ownerTeam->id, unitDefinition->cost)) {
            dependencies.economies.releaseReservedSupply(
                ownerTeam->id, unitDefinition->supplyCost);
            reject(context, command,
                   CommandRejectionReason::InsufficientResources,
                   dependencies.events);
            return;
        }

        const ProductionId id = ++dependencies.nextProductionId;
        const auto baseTicks =
            std::max<std::uint32_t>(1, unitDefinition->trainTicks);
        const auto requiredTicks =
            dependencies.modifiers.productionTicks(ownerTeam->id, baseTicks);
        queue->items.push_back(
            {id,
             unitDefinition->id,
             unitDefinition->cost,
             unitDefinition->supplyCost,
             0,
             requiredTicks,
             baseTicks});
        dependencies.events.push_back(
            {context.tick,
             DomainEventType::ProductionAccepted,
             command.subject,
             id,
             unitDefinition->id});
    }

    static void cancelProduction(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        EconomyCommandDependencies dependencies) {
        if (!world.alive(command.subject)) {
            reject(context, command, CommandRejectionReason::InvalidEntity,
                   dependencies.events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(context, command, CommandRejectionReason::NotOwner,
                   dependencies.events);
            return;
        }

        auto* queue = world.try_get<ProductionQueue>(command.subject);
        const auto* team = world.try_get<Team>(command.subject);
        if (!queue || !team) {
            reject(context, command, CommandRejectionReason::MissingCapability,
                   dependencies.events);
            return;
        }
        const auto iterator = std::find_if(
            queue->items.begin(),
            queue->items.end(),
            [&](const ProductionItem& item) {
                return item.id == command.objectId;
            });
        if (iterator == queue->items.end()) {
            reject(context, command, CommandRejectionReason::InvalidEntity,
                   dependencies.events);
            return;
        }
        dependencies.economies.releaseResources(
            team->id, iterator->reservedCost);
        dependencies.economies.releaseReservedSupply(
            team->id, iterator->supplyCost);
        queue->items.erase(iterator);
        dependencies.events.push_back(
            {context.tick,
             DomainEventType::ProductionCancelled,
             command.subject,
             command.objectId,
             0});
    }

    static void setRally(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        EconomyCommandDependencies dependencies) {
        if (!world.alive(command.subject)) {
            reject(context, command, CommandRejectionReason::InvalidEntity,
                   dependencies.events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(context, command, CommandRejectionReason::NotOwner,
                   dependencies.events);
            return;
        }
        const GridPoint target{command.targetX, command.targetY};
        if (!dependencies.navigation.contains(target) ||
            dependencies.navigation.blocked(target)) {
            reject(context, command, CommandRejectionReason::InvalidTarget,
                   dependencies.events);
            return;
        }
        auto* rally = world.try_get<RallyPoint>(command.subject);
        if (!rally) {
            reject(context, command, CommandRejectionReason::MissingCapability,
                   dependencies.events);
            return;
        }
        rally->point = target;
        dependencies.events.push_back(
            {context.tick,
             DomainEventType::RallyPointChanged,
             command.subject,
             0,
             0});
    }
};

struct ConstructionSystemDependencies {
    BaseBuildingRuntime& building;
    ecs::EntityCommandBuffer& structuralCommands;
    std::vector<ConstructionId>& completing;
    std::vector<DomainEvent>& events;
};

class ConstructionSystem final {
public:
    static void run(
        ecs::World& world,
        const ecs::SystemContext& context,
        ConstructionSystemDependencies dependencies) {
        dependencies.completing.clear();
        world.eachRef<ConstructionSite>(
            [&](ecs::Entity, ConstructionSite& site) {
                if (site.progressTicks + 1 >= site.requiredTicks) {
                    dependencies.completing.push_back(site.id);
                }
            });
        dependencies.building.advance(
            context, dependencies.structuralCommands, world);
        for (const auto id : dependencies.completing) {
            dependencies.events.push_back(
                {context.tick,
                 DomainEventType::ConstructionCompleted,
                 {},
                 id,
                 0});
        }
    }
};

struct ProductionSystemDependencies {
    ecs::EntityCommandBuffer& structuralCommands;
    TeamEconomyRuntime& economies;
    const GameplayModifierSystem& modifiers;
    const DefinitionCatalog<UnitDefinition>& unitDefinitions;
    const NavigationGrid& navigation;
    std::vector<DomainEvent>& events;
};

class ProductionSystem final {
public:
    static void run(
        ecs::World& world,
        const ecs::SystemContext& context,
        ProductionSystemDependencies dependencies) {
        world.eachRef<Building, ProductionQueue, RallyPoint, Team>(
            [&](ecs::Entity entity,
                Building&,
                ProductionQueue& queue,
                RallyPoint& rally,
                Team& team) {
                if (queue.items.empty()) return;

                auto& item = queue.items.front();
                if (item.progressTicks < item.requiredTicks) {
                    ++item.progressTicks;
                }
                if (item.progressTicks < item.requiredTicks) return;

                const auto* definition =
                    dependencies.unitDefinitions.find(item.unitDefinitionId);
                if (!definition) {
                    const auto rejectedId = item.id;
                    const auto rejectedDefinition = item.unitDefinitionId;
                    dependencies.economies.releaseResources(
                        team.id, item.reservedCost);
                    dependencies.economies.releaseReservedSupply(
                        team.id, item.supplyCost);
                    queue.items.erase(queue.items.begin());
                    dependencies.events.push_back(
                        {context.tick,
                         DomainEventType::ProductionRejected,
                         entity,
                         rejectedId,
                         rejectedDefinition});
                    return;
                }

                GridPoint spawn;
                if (!resolveSpawn(
                        world,
                        dependencies.navigation,
                        rally.point,
                        spawn)) {
                    return;
                }
                if (!dependencies.economies.commitResources(
                        team.id, item.reservedCost) ||
                    !dependencies.economies.commitSupply(
                        team.id, item.supplyCost)) {
                    return;
                }

                const auto producedId = item.id;
                const auto deferred =
                    dependencies.structuralCommands.create(context);
                EntityFactory::queueUnit(
                    context,
                    dependencies.structuralCommands,
                    dependencies.modifiers,
                    deferred,
                    Position{spawn.x, spawn.y},
                    definition->cellsPerTick,
                    team.id,
                    definition->combat,
                    definition->visionRange,
                    definition->supplyCost);
                queue.items.erase(queue.items.begin());
                dependencies.events.push_back(
                    {context.tick,
                     DomainEventType::ProductionCompleted,
                     entity,
                     producedId,
                     definition->id});
            });
    }

private:
    static bool occupiedByUnit(
        const ecs::World& world,
        GridPoint point) {
        bool occupied = false;
        world.eachRef<Position, MovementAgent>(
            [&](ecs::Entity,
                const Position& position,
                const MovementAgent&) {
                if (position.x == point.x && position.y == point.y) {
                    occupied = true;
                }
            });
        return occupied;
    }

    static bool resolveSpawn(
        const ecs::World& world,
        const NavigationGrid& navigation,
        GridPoint rally,
        GridPoint& output) {
        static constexpr GridPoint offsets[] = {
            {0, 0},
            {0, -1}, {1, 0}, {0, 1}, {-1, 0},
            {0, -2}, {1, -1}, {2, 0}, {1, 1},
            {0, 2}, {-1, 1}, {-2, 0}, {-1, -1},
            {0, -3}, {1, -2}, {2, -1}, {3, 0},
            {2, 1}, {1, 2}, {0, 3}, {-1, 2},
            {-2, 1}, {-3, 0}, {-2, -1}, {-1, -2}
        };
        for (const auto offset : offsets) {
            const GridPoint candidate{
                rally.x + offset.x, rally.y + offset.y};
            if (!navigation.contains(candidate) ||
                navigation.blocked(candidate) ||
                occupiedByUnit(world, candidate)) {
                continue;
            }
            output = candidate;
            return true;
        }
        return false;
    }
};

} // namespace rts::gameplay
