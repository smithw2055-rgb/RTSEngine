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
    static void beginConstruction(
        const ecs::SystemContext& context,
        const TickCommand& command,
        EconomyCommandDependencies dependencies) {
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
        auto* queue = world.try_get<ProductionQueue>(command.subject);
        const auto* building = world.try_get<Building>(command.subject);
        const auto* ownerTeam = world.try_get<Team>(command.subject);
        const auto* definition =
            dependencies.unitDefinitions.find(command.definitionId);
        if (!queue || !building || !building->producer || !definition ||
            definition->id == 0 || definition->cost < 0 ||
            !dependencies.resources.reserve(definition->cost)) {
            dependencies.events.push_back(
                {context.tick,
                 DomainEventType::ProductionRejected,
                 command.subject,
                 0,
                 command.definitionId});
            return;
        }

        const ProductionId id = ++dependencies.nextProductionId;
        const auto baseTicks =
            std::max<std::uint32_t>(1, definition->trainTicks);
        const auto teamId = ownerTeam ? ownerTeam->id : 0;
        const auto requiredTicks =
            dependencies.modifiers.productionTicks(teamId, baseTicks);
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
        auto* queue = world.try_get<ProductionQueue>(command.subject);
        if (!queue) {
            dependencies.events.push_back(
                {context.tick,
                 DomainEventType::ProductionRejected,
                 command.subject,
                 command.objectId,
                 0});
            return;
        }
        const auto iterator = std::find_if(
            queue->items.begin(),
            queue->items.end(),
            [&](const ProductionItem& item) {
                return item.id == command.objectId;
            });
        if (iterator == queue->items.end()) {
            dependencies.events.push_back(
                {context.tick,
                 DomainEventType::ProductionRejected,
                 command.subject,
                 command.objectId,
                 0});
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
        auto* rally = world.try_get<RallyPoint>(command.subject);
        if (!rally) return;
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
        for (const auto entity : world.view<ConstructionSite>()) {
            const auto* site = world.try_get<ConstructionSite>(entity);
            if (site &&
                site->progressTicks + 1 >= site->requiredTicks) {
                completing.push_back(site->id);
            }
        }
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
        for (const auto entity :
             world.view<Building, ProductionQueue, RallyPoint>()) {
            auto* queue = world.try_get<ProductionQueue>(entity);
            const auto* rally = world.try_get<RallyPoint>(entity);
            if (!queue || !rally || queue->items.empty()) continue;

            auto& item = queue->items.front();
            ++item.progressTicks;
            if (item.progressTicks < item.requiredTicks) continue;

            const auto* definition =
                dependencies.unitDefinitions.find(item.unitDefinitionId);
            if (!definition) {
                const auto rejectedId = item.id;
                const auto rejectedDefinition = item.unitDefinitionId;
                dependencies.resources.release(item.reservedCost);
                queue->items.erase(queue->items.begin());
                dependencies.events.push_back(
                    {context.tick,
                     DomainEventType::ProductionRejected,
                     entity,
                     rejectedId,
                     rejectedDefinition});
                continue;
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
                Position{rally->point.x, rally->point.y},
                definition->cellsPerTick,
                teamId,
                definition->combat,
                definition->visionRange);
            queue->items.erase(queue->items.begin());
            dependencies.events.push_back(
                {context.tick,
                 DomainEventType::ProductionCompleted,
                 entity,
                 producedId,
                 definition->id});
        }
    }
};

} // namespace rts::gameplay
