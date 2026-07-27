#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/DefinitionCatalog.h>
#include <RTSEngine/Rts/EntityFactory.h>
#include <RTSEngine/Rts/GameplayModifierSystem.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <RTSEngine/Rts/TechTree.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::gameplay {

struct EconomyCommandDependencies {
    ecs::EntityCommandBuffer& structuralCommands;
    BaseBuildingRuntime& building;
    TeamEconomyRuntime& economy;
    TechTreeRuntime& tech;
    GameplayModifierSystem& modifiers;
    const DefinitionCatalog<BuildingDefinition>& buildingDefinitions;
    const DefinitionCatalog<UnitDefinition>& unitDefinitions;
    const DefinitionCatalog<ResearchDefinition>& researchDefinitions;
    const PrerequisiteCatalog& buildingPrerequisites;
    const PrerequisiteCatalog& unitPrerequisites;
    GridPoint requiredPathStart;
    GridPoint requiredPathGoal;
    ProductionId& nextProductionId;
    ResearchQueueId& nextResearchId;
    std::vector<DomainEvent>& events;
};

class EconomyCommandSystem final {
public:
    static bool handles(CommandType type) noexcept {
        return type == CommandType::Build ||
               type == CommandType::CancelConstruction ||
               type == CommandType::Train ||
               type == CommandType::CancelProduction ||
               type == CommandType::SetRally ||
               type == CommandType::Research ||
               type == CommandType::CancelResearch;
    }

    static void process(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        EconomyCommandDependencies dependencies) {
        switch (command.type) {
        case CommandType::Build:
            beginConstruction(world, context, command, dependencies);
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
        case CommandType::Research:
            beginResearch(world, context, command, dependencies);
            break;
        case CommandType::CancelResearch:
            cancelResearch(world, context, command, dependencies);
            break;
        default:
            break;
        }
    }

private:
    static bool owns(
        const ecs::World& world,
        ecs::Entity entity,
        std::uint32_t issuer) noexcept {
        const auto* team = world.try_get<Team>(entity);
        return issuer != 0 && team && team->id == issuer;
    }

    static void reject(
        const ecs::SystemContext& context,
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

    static bool meets(
        const ecs::World& world,
        const TechTreeRuntime& tech,
        std::uint32_t teamId,
        const PrerequisiteCatalog& catalog,
        std::uint32_t definitionId) {
        const auto* requirements = catalog.find(definitionId);
        return !requirements || tech.meets(world, teamId, *requirements);
    }

    static bool reserveCosts(
        TeamEconomyRuntime& economy,
        std::uint32_t teamId,
        const std::vector<ResourceCost>& costs) {
        std::size_t reservedCount = 0;
        for (const auto& cost : costs) {
            if (!economy.reserve(teamId, cost.resourceType, cost.amount)) {
                while (reservedCount != 0) {
                    --reservedCount;
                    const auto& reserved = costs[reservedCount];
                    economy.release(
                        teamId, reserved.resourceType, reserved.amount);
                }
                return false;
            }
            ++reservedCount;
        }
        return true;
    }

    static void releaseCosts(
        TeamEconomyRuntime& economy,
        std::uint32_t teamId,
        const std::vector<ResourceCost>& costs) {
        for (const auto& cost : costs) {
            economy.release(teamId, cost.resourceType, cost.amount);
        }
    }

    static void beginConstruction(
        const ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        EconomyCommandDependencies dependencies) {
        if (command.issuer == 0) {
            reject(
                context,
                command,
                CommandRejectionReason::NotOwner,
                dependencies.events);
            return;
        }
        if (!meets(
                world,
                dependencies.tech,
                command.issuer,
                dependencies.buildingPrerequisites,
                command.definitionId)) {
            reject(
                context,
                command,
                CommandRejectionReason::PrerequisiteMissing,
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
            reject(
                context,
                command,
                CommandRejectionReason::InvalidEntity,
                dependencies.events);
            return;
        }
        if (!owned) {
            reject(
                context,
                command,
                CommandRejectionReason::NotOwner,
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
            reject(
                context,
                command,
                CommandRejectionReason::InvalidEntity,
                dependencies.events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(
                context,
                command,
                CommandRejectionReason::NotOwner,
                dependencies.events);
            return;
        }
        if (!meets(
                world,
                dependencies.tech,
                command.issuer,
                dependencies.unitPrerequisites,
                command.definitionId)) {
            reject(
                context,
                command,
                CommandRejectionReason::PrerequisiteMissing,
                dependencies.events);
            return;
        }

        auto* queue = world.try_get<ProductionQueue>(command.subject);
        const auto* building = world.try_get<Building>(command.subject);
        const auto* ownerTeam = world.try_get<Team>(command.subject);
        const auto* definition =
            dependencies.unitDefinitions.find(command.definitionId);
        if (!queue || !building || !building->producer || !ownerTeam) {
            reject(
                context,
                command,
                CommandRejectionReason::MissingCapability,
                dependencies.events);
            return;
        }
        if (!definition || definition->id == 0 || definition->cost < 0) {
            reject(
                context,
                command,
                CommandRejectionReason::InvalidDefinition,
                dependencies.events);
            return;
        }
        if (!dependencies.economy.reserve(
                ownerTeam->id,
                kPrimaryResourceType,
                definition->cost)) {
            reject(
                context,
                command,
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
            reject(
                context,
                command,
                CommandRejectionReason::InvalidEntity,
                dependencies.events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(
                context,
                command,
                CommandRejectionReason::NotOwner,
                dependencies.events);
            return;
        }

        auto* queue = world.try_get<ProductionQueue>(command.subject);
        const auto* team = world.try_get<Team>(command.subject);
        if (!queue || !team) {
            reject(
                context,
                command,
                CommandRejectionReason::MissingCapability,
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
            reject(
                context,
                command,
                CommandRejectionReason::InvalidEntity,
                dependencies.events);
            return;
        }
        dependencies.economy.release(
            team->id,
            kPrimaryResourceType,
            iterator->reservedCost);
        queue->items.erase(iterator);
        dependencies.events.push_back(
            {context.tick,
             DomainEventType::ProductionCancelled,
             command.subject,
             command.objectId,
             0});
    }

    static bool researchQueuedForTeam(
        const ecs::World& world,
        std::uint32_t teamId,
        ResearchDefinitionId definitionId) {
        bool queued = false;
        world.eachRef<Team, ResearchQueue>(
            [&](ecs::Entity,
                const Team& team,
                const ResearchQueue& queue) {
                if (team.id != teamId || queued) return;
                queued = std::any_of(
                    queue.items.begin(),
                    queue.items.end(),
                    [definitionId](const ResearchItem& item) {
                        return item.researchDefinitionId == definitionId;
                    });
            });
        return queued;
    }

    static void beginResearch(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        EconomyCommandDependencies dependencies) {
        if (!world.alive(command.subject)) {
            reject(
                context,
                command,
                CommandRejectionReason::InvalidEntity,
                dependencies.events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(
                context,
                command,
                CommandRejectionReason::NotOwner,
                dependencies.events);
            return;
        }
        const auto* building = world.try_get<Building>(command.subject);
        const auto* team = world.try_get<Team>(command.subject);
        const auto* definition =
            dependencies.researchDefinitions.find(command.definitionId);
        if (!building || !building->producer || !team) {
            reject(
                context,
                command,
                CommandRejectionReason::MissingCapability,
                dependencies.events);
            return;
        }
        if (!definition || definition->id == 0 ||
            !ValidateResourceCosts(definition->costs) ||
            !ValidatePrerequisites(definition->prerequisites)) {
            reject(
                context,
                command,
                CommandRejectionReason::InvalidDefinition,
                dependencies.events);
            return;
        }
        if (dependencies.tech.completed(team->id, definition->id)) {
            reject(
                context,
                command,
                CommandRejectionReason::AlreadyCompleted,
                dependencies.events);
            return;
        }
        if (researchQueuedForTeam(world, team->id, definition->id)) {
            reject(
                context,
                command,
                CommandRejectionReason::AlreadyQueued,
                dependencies.events);
            return;
        }
        if (!dependencies.tech.meets(
                world, team->id, definition->prerequisites)) {
            reject(
                context,
                command,
                CommandRejectionReason::PrerequisiteMissing,
                dependencies.events);
            return;
        }
        if (!reserveCosts(
                dependencies.economy, team->id, definition->costs)) {
            reject(
                context,
                command,
                CommandRejectionReason::InsufficientResources,
                dependencies.events);
            return;
        }

        auto* queue = world.try_get<ResearchQueue>(command.subject);
        if (!queue) {
            queue = &world.emplace<ResearchQueue>(
                command.subject, ResearchQueue{});
        }
        const auto id = ++dependencies.nextResearchId;
        const auto baseTicks =
            std::max<std::uint32_t>(1, definition->researchTicks);
        const auto requiredTicks =
            dependencies.modifiers.productionTicks(team->id, baseTicks);
        queue->items.push_back(
            {id,
             definition->id,
             definition->costs,
             0,
             requiredTicks,
             baseTicks});
        dependencies.events.push_back(
            {context.tick,
             DomainEventType::ResearchAccepted,
             command.subject,
             id,
             definition->id});
    }

    static void cancelResearch(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        EconomyCommandDependencies dependencies) {
        if (!world.alive(command.subject)) {
            reject(
                context,
                command,
                CommandRejectionReason::InvalidEntity,
                dependencies.events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(
                context,
                command,
                CommandRejectionReason::NotOwner,
                dependencies.events);
            return;
        }
        auto* queue = world.try_get<ResearchQueue>(command.subject);
        const auto* team = world.try_get<Team>(command.subject);
        if (!queue || !team) {
            reject(
                context,
                command,
                CommandRejectionReason::MissingCapability,
                dependencies.events);
            return;
        }
        const auto found = std::find_if(
            queue->items.begin(), queue->items.end(),
            [&](const ResearchItem& item) {
                return item.id == command.objectId;
            });
        if (found == queue->items.end()) {
            reject(
                context,
                command,
                CommandRejectionReason::InvalidEntity,
                dependencies.events);
            return;
        }
        releaseCosts(dependencies.economy, team->id, found->reservedCosts);
        queue->items.erase(found);
        dependencies.events.push_back(
            {context.tick,
             DomainEventType::ResearchCancelled,
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
            reject(
                context,
                command,
                CommandRejectionReason::InvalidEntity,
                events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(
                context,
                command,
                CommandRejectionReason::NotOwner,
                events);
            return;
        }
        auto* rally = world.try_get<RallyPoint>(command.subject);
        if (!rally) {
            reject(
                context,
                command,
                CommandRejectionReason::MissingCapability,
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
    TeamEconomyRuntime& economy;
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
                const auto* ownerTeam = world.try_get<Team>(entity);
                const auto teamId = ownerTeam ? ownerTeam->id : 0;
                if (!definition || teamId == 0) {
                    const auto rejectedId = item.id;
                    const auto rejectedDefinition = item.unitDefinitionId;
                    if (teamId != 0) {
                        dependencies.economy.release(
                            teamId,
                            kPrimaryResourceType,
                            item.reservedCost);
                    }
                    queue.items.erase(queue.items.begin());
                    dependencies.events.push_back(
                        {context.tick,
                         DomainEventType::ProductionRejected,
                         entity,
                         rejectedId,
                         rejectedDefinition});
                    return;
                }

                dependencies.economy.commit(
                    teamId,
                    kPrimaryResourceType,
                    item.reservedCost);
                const auto producedId = item.id;
                const auto deferred =
                    dependencies.structuralCommands.create(context);
                EntityFactory::queueUnitDefinition(
                    context,
                    dependencies.structuralCommands,
                    dependencies.modifiers,
                    deferred,
                    Position{rally.point.x, rally.point.y},
                    teamId,
                    *definition);
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

struct ResearchSystemDependencies {
    TeamEconomyRuntime& economy;
    TechTreeRuntime& tech;
    GameplayModifierSystem& modifiers;
    const DefinitionCatalog<ResearchDefinition>& researchDefinitions;
    std::vector<DomainEvent>& events;
};

class ResearchSystem final {
public:
    static void run(
        ecs::World& world,
        const ecs::SystemContext& context,
        ResearchSystemDependencies dependencies) {
        world.eachRef<Team, ResearchQueue>(
            [&](ecs::Entity entity,
                Team& team,
                ResearchQueue& queue) {
                if (queue.items.empty()) return;
                auto& item = queue.items.front();
                ++item.progressTicks;
                if (item.progressTicks < item.requiredTicks) return;

                const auto* definition =
                    dependencies.researchDefinitions.find(
                        item.researchDefinitionId);
                if (!definition ||
                    dependencies.tech.completed(
                        team.id, item.researchDefinitionId)) {
                    for (const auto& cost : item.reservedCosts) {
                        dependencies.economy.release(
                            team.id, cost.resourceType, cost.amount);
                    }
                    const auto rejectedId = item.id;
                    const auto rejectedDefinition =
                        item.researchDefinitionId;
                    queue.items.erase(queue.items.begin());
                    dependencies.events.push_back(
                        {context.tick,
                         DomainEventType::ResearchRejected,
                         entity,
                         rejectedId,
                         rejectedDefinition});
                    return;
                }

                for (const auto& cost : item.reservedCosts) {
                    dependencies.economy.commit(
                        team.id, cost.resourceType, cost.amount);
                }
                dependencies.tech.unlock(
                    team.id, item.researchDefinitionId);
                const auto updated = ApplyTeamModifierDelta(
                    dependencies.modifiers.profile(team.id),
                    definition->modifiers);
                dependencies.modifiers.setProfile<MoveSpeed>(
                    world, team.id, updated);
                const auto completedId = item.id;
                const auto completedDefinition =
                    item.researchDefinitionId;
                queue.items.erase(queue.items.begin());
                dependencies.events.push_back(
                    {context.tick,
                     DomainEventType::ResearchCompleted,
                     entity,
                     completedId,
                     completedDefinition});
            });
    }
};

} // namespace rts::gameplay
