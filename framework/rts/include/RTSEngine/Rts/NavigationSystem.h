#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/DefinitionCatalog.h>
#include <RTSEngine/Rts/EntityFactory.h>
#include <RTSEngine/Rts/FlowField.h>
#include <RTSEngine/Rts/GameplayModifierSystem.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/OrderSystem.h>
#include <RTSEngine/Rts/PathCache.h>
#include <RTSEngine/Rts/RuntimeTelemetry.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct NavigationSystemDependencies {
    NavigationGrid& navigation;
    GridPathCache& pathCache;
    GridPathfinderScratch& pathScratch;
    GridFlowFieldCache& flowFields;
    RuntimeTelemetry& telemetry;
    ecs::EntityCommandBuffer& structuralCommands;
    GameplayModifierSystem& modifiers;
    const DefinitionCatalog<BuildingDefinition>& buildingDefinitions;
    std::vector<DomainEvent>& events;
};

class NavigationSystem final {
public:
    static constexpr std::uint32_t kFlowFieldThreshold = 8u;

    static void synchronizeTeamModifiers(
        ecs::World& world,
        NavigationSystemDependencies dependencies) {
        world.eachRef<Team, TunableStats>(
            [&](ecs::Entity entity, Team&, TunableStats&) {
                dependencies.modifiers.applyEntity<MoveSpeed>(world, entity);
            });
    }

    static void synchronizeConstruction(
        ecs::World& world,
        const ecs::SystemContext& context,
        NavigationSystemDependencies dependencies) {
        world.eachRef<ConstructionSite, BuildingFootprint>(
            [&](ecs::Entity entity,
                ConstructionSite& site,
                BuildingFootprint& footprint) {
                if (!world.try_get<Position>(entity)) {
                    dependencies.structuralCommands.add(
                        context,
                        entity,
                        Position{footprint.origin.x, footprint.origin.y});
                }
                if (!world.try_get<Team>(entity)) {
                    dependencies.structuralCommands.add(
                        context, entity, Team{site.ownerTeam});
                }
                const auto* definition =
                    dependencies.buildingDefinitions.find(site.definitionId);
                if (definition && !world.try_get<TunableStats>(entity)) {
                    EntityFactory::queueCombatProfile(
                        context,
                        dependencies.structuralCommands,
                        dependencies.modifiers,
                        entity,
                        definition->combat,
                        true,
                        0,
                        site.ownerTeam);
                }
            });
    }

    static void run(
        ecs::World& world,
        const ecs::SystemContext& context,
        NavigationSystemDependencies dependencies) {
        dependencies.telemetry.beginNavigationTick(context.tick);
        prepareFlowDemands(world, dependencies);
        for (const auto& demand : dependencies.flowFields.demands()) {
            dependencies.telemetry.recordDemandGroup(
                demand.count,
                demand.count >= kFlowFieldThreshold);
        }

        world.eachRef<Position, OrderQueue, MovementAgent>(
            [&](ecs::Entity entity,
                Position& position,
                OrderQueue& queue,
                MovementAgent& agent) {
                auto* directive = world.try_get<CombatDirective>(entity);

                if (directive &&
                    directive->mode == CombatMode::AttackTarget) {
                    updateAttackTarget(
                        world,
                        context,
                        entity,
                        position,
                        agent,
                        *directive,
                        dependencies);
                    return;
                }

                if (queue.pending.empty()) {
                    if (directive &&
                        (directive->mode == CombatMode::PassiveMove ||
                         directive->mode == CombatMode::AttackMove)) {
                        directive->mode = CombatMode::Guard;
                    }
                    return;
                }

                if (directive &&
                    directive->mode != CombatMode::HoldPosition) {
                    directive->forcedTarget = {};
                    directive->mode =
                        queue.pending.front().type == OrderType::AttackMove
                            ? CombatMode::AttackMove
                            : CombatMode::PassiveMove;
                }

                if (agent.pathRevision !=
                    dependencies.navigation.revision()) {
                    OrderSystem::clearPath(agent);
                }

                const auto goal = queue.pending.front().target;
                if (position.x == goal.x && position.y == goal.y) {
                    queue.pending.erase(queue.pending.begin());
                    OrderSystem::clearPath(agent);
                    OrderSystem::applyFrontOrderMode(world, entity, queue);
                    dependencies.events.push_back(
                        {context.tick,
                         DomainEventType::MoveCompleted,
                         entity,
                         0,
                         0});
                    return;
                }

                if (hasReusableRegularPath(
                        agent,
                        goal,
                        dependencies.navigation.revision())) {
                    return;
                }

                const GridPoint start{position.x, position.y};
                const bool useFlowField =
                    dependencies.flowFields.demandCount(goal) >=
                    kFlowFieldThreshold;
                agent.pathRevision = dependencies.navigation.revision();

                if (useFlowField) {
                    const auto& field = dependencies.flowFields.resolve(
                        dependencies.navigation, goal);
                    if (!dependencies.flowFields.assignDirect(field, start)) {
                        failRegularPath(
                            world,
                            context,
                            entity,
                            queue,
                            agent,
                            dependencies);
                        return;
                    }
                    assignFlowField(
                        agent, goal, dependencies.flowFields);
                    dependencies.telemetry.recordFlowAssignment(
                        static_cast<std::uint64_t>(field.distance(start)));
                } else {
                    const auto& path = dependencies.pathCache.resolve(
                        dependencies.navigation,
                        start,
                        goal,
                        dependencies.pathScratch);
                    if (!path.found) {
                        failRegularPath(
                            world,
                            context,
                            entity,
                            queue,
                            agent,
                            dependencies);
                        return;
                    }
                    assignPath(agent, path, goal, false);
                    dependencies.telemetry.recordPathCacheAssignment(
                        path.points.size());
                }

                dependencies.events.push_back(
                    {context.tick,
                     DomainEventType::PathReady,
                     entity,
                     0,
                     0});
            });
    }

private:
    static bool sampleFlow(
        void* context,
        const NavigationGrid& navigation,
        GridPoint goal,
        GridPoint current,
        GridPoint& next) {
        return context && static_cast<GridFlowFieldCache*>(context)->sample(
            navigation, goal, current, next);
    }

    static void prepareFlowDemands(
        const ecs::World& world,
        NavigationSystemDependencies dependencies) {
        dependencies.flowFields.beginDemands();
        world.eachRef<Position, OrderQueue, MovementAgent>(
            [&](ecs::Entity entity,
                const Position& position,
                const OrderQueue& queue,
                const MovementAgent& agent) {
                const auto* directive =
                    world.try_get<CombatDirective>(entity);

                dependencies.telemetry.recordNavigationAgent();
                if (queue.pending.empty() ||
                    (directive &&
                     directive->mode == CombatMode::AttackTarget)) {
                    return;
                }

                const auto goal = queue.pending.front().target;
                if ((position.x == goal.x && position.y == goal.y) ||
                    hasReusableRegularPath(
                        agent,
                        goal,
                        dependencies.navigation.revision())) {
                    return;
                }
                dependencies.flowFields.addDemand(goal);
                dependencies.telemetry.recordPathRequest();
            });
    }

    static bool hasReusableRegularPath(
        const MovementAgent& agent,
        GridPoint goal,
        std::uint64_t navigationRevision) noexcept {
        const bool reusableFlow =
            agent.flowFieldPath && agent.flowContext && agent.flowSample;
        return (reusableFlow || !agent.path.empty()) &&
               agent.hasPathGoal && agent.pathGoal == goal &&
               agent.pathRevision == navigationRevision &&
               !agent.combatPath;
    }

    static void failRegularPath(
        ecs::World& world,
        const ecs::SystemContext& context,
        ecs::Entity entity,
        OrderQueue& queue,
        MovementAgent& agent,
        NavigationSystemDependencies dependencies) {
        if (!queue.pending.empty()) {
            queue.pending.erase(queue.pending.begin());
        }
        OrderSystem::clearPath(agent);
        OrderSystem::applyFrontOrderMode(world, entity, queue);
        dependencies.telemetry.recordPathFailure();
        dependencies.events.push_back(
            {context.tick,
             DomainEventType::PathFailed,
             entity,
             0,
             0});
    }

    static void updateAttackTarget(
        ecs::World& world,
        const ecs::SystemContext& context,
        ecs::Entity entity,
        const Position& position,
        MovementAgent& agent,
        CombatDirective& directive,
        NavigationSystemDependencies dependencies) {
        const auto* ownTeam = world.try_get<Team>(entity);
        const auto* weapon = world.try_get<Weapon>(entity);
        auto* target = world.try_get<CombatTarget>(entity);
        const auto* targetTeam =
            world.try_get<Team>(directive.forcedTarget);
        const auto* targetHealth =
            world.try_get<Health>(directive.forcedTarget);
        const auto* targetPosition =
            world.try_get<Position>(directive.forcedTarget);

        const bool valid =
            ownTeam && weapon && target && targetTeam &&
            targetHealth && targetPosition &&
            world.alive(directive.forcedTarget) &&
            targetHealth->current > 0 &&
            targetTeam->id != ownTeam->id;
        if (!valid) {
            const auto lost = directive.forcedTarget;
            directive.mode = CombatMode::Guard;
            directive.forcedTarget = {};
            if (target) target->entity = {};
            OrderSystem::clearPath(agent);
            dependencies.events.push_back(
                {context.tick,
                 DomainEventType::AttackTargetLost,
                 entity,
                 0,
                 0,
                 lost,
                 0});
            return;
        }

        target->entity = directive.forcedTarget;
        const GridPoint start{position.x, position.y};
        const GridPoint targetPoint{
            targetPosition->x, targetPosition->y};
        if (ManhattanDistance(start, targetPoint) <= weapon->range) {
            OrderSystem::clearPath(agent);
            return;
        }

        const bool targetChanged =
            agent.chaseTarget != directive.forcedTarget ||
            !(agent.chaseTargetPosition == targetPoint);
        const bool needsPath =
            agent.path.empty() || !agent.combatPath ||
            agent.pathRevision != dependencies.navigation.revision() ||
            targetChanged;
        if (!needsPath) return;

        dependencies.telemetry.recordPathRequest();
        auto path = GridPathfinder::findToRange(
            dependencies.navigation,
            start,
            targetPoint,
            weapon->range,
            dependencies.pathScratch);
        if (!path.found) {
            const auto lost = directive.forcedTarget;
            directive.mode = CombatMode::Guard;
            directive.forcedTarget = {};
            target->entity = {};
            OrderSystem::clearPath(agent);
            dependencies.telemetry.recordPathFailure();
            dependencies.events.push_back(
                {context.tick,
                 DomainEventType::PathFailed,
                 entity,
                 0,
                 0,
                 lost,
                 0});
            return;
        }

        const auto pointCount = path.points.size();
        const auto goal = path.points.empty()
            ? start
            : path.points.back();
        agent.pathRevision = dependencies.navigation.revision();
        agent.path = std::move(path.points);
        assignExtractedPath(agent, goal, true);
        agent.chaseTarget = directive.forcedTarget;
        agent.chaseTargetPosition = targetPoint;
        dependencies.telemetry.recordAttackAssignment(pointCount);
        dependencies.events.push_back(
            {context.tick,
             DomainEventType::PathReady,
             entity,
             0,
             0,
             directive.forcedTarget,
             0});
    }

    static void clearFlowBinding(MovementAgent& agent) noexcept {
        agent.flowFieldPath = false;
        agent.flowContext = nullptr;
        agent.flowSample = nullptr;
    }

    static void assignExtractedPath(
        MovementAgent& agent,
        GridPoint goal,
        bool combatPath) {
        agent.nextPoint = 0;
        agent.pathGoal = goal;
        agent.hasPathGoal = true;
        agent.combatPath = combatPath;
        clearFlowBinding(agent);
    }

    static void assignFlowField(
        MovementAgent& agent,
        GridPoint goal,
        GridFlowFieldCache& cache) {
        agent.path.clear();
        agent.nextPoint = 0;
        agent.pathGoal = goal;
        agent.hasPathGoal = true;
        agent.combatPath = false;
        agent.flowFieldPath = true;
        agent.flowContext = &cache;
        agent.flowSample = &sampleFlow;
        agent.chaseTarget = {};
        agent.chaseTargetPosition = {};
    }

    static void assignPath(
        MovementAgent& agent,
        const PathResult& path,
        GridPoint goal,
        bool combatPath) {
        agent.path = path.points;
        assignExtractedPath(agent, goal, combatPath);
    }
};

} // namespace rts::gameplay
