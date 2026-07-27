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
#include <vector>

namespace rts::gameplay {

struct EconomyCommandDependencies {
    ecs::EntityCommandBuffer& structuralCommands;
    BaseBuildingRuntime& building;
    ResourceLedger& resources;
    GameplayModifierSystem& modifiers;
    const DefinitionCatalog<BuildingDefinition>& buildingDefinitions;
    const DefinitionCatalog<UnitDefinition>& unitDefinitions;
    GridPoint requiredPathStart;
    GridPoint requiredPathGoal;
    ProductionId& nextProductionId;
    std::vector<DomainEvent>& events;
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
            setRally(world, context, command, dependencies.events);
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
        const auto* definition =
            dependencies.unitDefinitions.find(command.definitionId);
        if (!queue || !building || !building->producer || !ownerTeam) {
            reject(context, command, CommandRejectionReason::MissingCapability,
                   dependencies.events);
            return;
        }
        if (!definition || definition->id == 0 || definition->cost < 0) {
            reject(context, command, CommandRejectionReason::InvalidDefinition,
                   dependencies.events);
            return;
        }
        if (!dependencies.resources.reserve(definition->cost)) {
            reject(context, command,
                   CommandRejectionReason::InsufficientResources,
                   dependencies.events);
            return;
        }

        const ProductionId id = ++dependencies.nextProductionId;
        const auto baseTicks =
            std::max<std::uint32_t>(1, definition->trainTicks);
        const auto requiredTicks =
            dependencies.modifiers.productionTicks(ownerTeam->id, baseTicks);
        queue->items.push_back(
            {id,
             definition->id,
             definition->cost,
             0,
             requiredTicks,
             baseTicks});
        dependencies.events.push_back(
            {context.tick,
             DomainEventType::ProductionAccepted,
             command.subject,
             id,
             definition->id});
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
        if (!queue) {
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
        dependencies.resources.release(iterator->reservedCost);
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
        std::vector<DomainEvent>& events) {
        if (!world.alive(command.subject)) {
            reject(context, command, CommandRejectionReason::InvalidEntity,
                   events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(context, command, CommandRejectionReason::NotOwner, events);
            return;
        }
        auto* rally = world.try_get<RallyPoint>(command.subject);
        if (!rally) {
            reject(context, command, CommandRejectionReason::MissingCapability,
                   events);
            return;
        }
        rally->point = {command.targetX, command.targetY};
        events.push_back(
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
    std::vector<DomainEvent>& events;
};

class ConstructionSystem final {
public:
    static void run(
        ecs::World& world,
        const ecs::SystemContext& context,
        ConstructionSystemDependencies dependencies) {
        std::vector<ConstructionId> completing;
        world.eachRef<ConstructionSite>(
            [&](ecs::Entity, ConstructionSite& site) {
                if (site.progressTicks + 1 >= site.requiredTicks) {
                    completing.push_back(site.id);
                }
            });
        dependencies.building.advance(
            context, dependencies.structuralCommands, world);
        for (const auto id : completing) {
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
    ResourceLedger& resources;
    const GameplayModifierSystem& modifiers;
    const DefinitionCatalog<UnitDefinition>& unitDefinitions;
    std::vector<DomainEvent>& events;
};

class ProductionSystem final {
public:
    static void run(
        ecs::World& world,
        const ecs::SystemContext& context,
        ProductionSystemDependencies dependencies) {
        world.eachRef<Building, ProductionQueue, RallyPoint>(
            [&](ecs::Entity entity,
                Building&,
                ProductionQueue& queue,
                RallyPoint& rally) {
                if (queue.items.empty()) return;

                auto& item = queue.items.front();
                ++item.progressTicks;
                if (item.progressTicks < item.requiredTicks) return;

                const auto* definition =
                    dependencies.unitDefinitions.find(item.unitDefinitionId);
                if (!definition) {
                    const auto rejectedId = item.id;
                    const auto rejectedDefinition = item.unitDefinitionId;
                    dependencies.resources.release(item.reservedCost);
                    queue.items.erase(queue.items.begin());
                    dependencies.events.push_back(
                        {context.tick,
                         DomainEventType::ProductionRejected,
                         entity,
                         rejectedId,
                         rejectedDefinition});
                    return;
                }

                dependencies.resources.commit(item.reservedCost);
                const auto producedId = item.id;
                const auto* ownerTeam = world.try_get<Team>(entity);
                const auto teamId = ownerTeam ? ownerTeam->id : 0;
                const auto deferred =
                    dependencies.structuralCommands.create(context);
                EntityFactory::queueUnit(
                    context,
                    dependencies.structuralCommands,
                    dependencies.modifiers,
                    deferred,
                    Position{rally.point.x, rally.point.y},
                    definition->cellsPerTick,
                    teamId,
                    definition->combat,
                    definition->visionRange);
                queue.items.erase(queue.items.begin());
                dependencies.events.push_back(
                    {context.tick,
                     DomainEventType::ProductionCompleted,
                     entity,
                     producedId,
                     definition->id});
            });
    }
};

} // namespace rts::gameplay
