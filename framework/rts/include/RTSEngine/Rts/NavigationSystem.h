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

#include <algorithm>
#include <cstdint>
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
        for (const auto entity : world.view<Team, TunableStats>()) {
            dependencies.modifiers.applyEntity<MoveSpeed>(world, entity);
        }
    }

    static void synchronizeConstruction(
        ecs::World& world,
        const ecs::SystemContext& context,
        NavigationSystemDependencies dependencies) {
        for (const auto entity :
             world.view<ConstructionSite, BuildingFootprint>()) {
            const auto* site = world.try_get<ConstructionSite>(entity);
            const auto* footprint =
                world.try_get<BuildingFootprint>(entity);
            if (!site || !footprint) continue;

            if (!world.try_get<Position>(entity)) {
                dependencies.structuralCommands.add(
                    context,
                    entity,
                    Position{footprint->origin.x, footprint->origin.y});
            }
            if (!world.try_get<Team>(entity)) {
                dependencies.structuralCommands.add(
                    context, entity, Team{site->ownerTeam});
            }
            const auto* definition =
                dependencies.buildingDefinitions.find(site->definitionId);
            if (definition && !world.try_get<TunableStats>(entity)) {
                EntityFactory::queueCombatProfile(
                    context,
                    dependencies.structuralCommands,
                    dependencies.modifiers,
                    entity,
                    definition->combat,
                    true,
                    0,
                    site->ownerTeam);
            }
        }
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

        for (const auto entity :
             world.view<Position, OrderQueue, MovementAgent>()) {
            auto* position = world.try_get<Position>(entity);
            auto* queue = world.try_get<OrderQueue>(entity);
            auto* agent = world.try_get<MovementAgent>(entity);
            auto* directive =
                world.try_get<CombatDirective>(entity);
            if (!position || !queue || !agent) continue;

            if (directive &&
                directive->mode == CombatMode::AttackTarget) {
                updateAttackTarget(
                    world,
                    context,
                    entity,
                    *position,
                    *agent,
                    *directive,
                    dependencies);
                continue;
            }

            if (queue->pending.empty()) {
                if (directive &&
                    (directive->mode == CombatMode::PassiveMove ||
                     directive->mode == CombatMode::AttackMove)) {
                    directive->mode = CombatMode::Guard;
                }
                continue;
            }

            if (directive &&
                directive->mode != CombatMode::HoldPosition) {
                directive->forcedTarget = {};
                directive->mode =
                    queue->pending.front().type == OrderType::AttackMove
                        ? CombatMode::AttackMove
                        : CombatMode::PassiveMove;
            }

            if (agent->pathRevision != dependencies.navigation.revision()) {
                OrderSystem::clearPath(*agent);
            }

            const auto goal = queue->pending.front().target;
            if (position->x == goal.x && position->y == goal.y) {
                queue->pending.erase(queue->pending.begin());
                OrderSystem::clearPath(*agent);
                OrderSystem::applyFrontOrderMode(world, entity, *queue);
                dependencies.events.push_back(
                    {context.tick,
                     DomainEventType::MoveCompleted,
                     entity,
                     0,
                     0});
                continue;
            }

            if (hasReusableRegularPath(
                    *agent, goal, dependencies.navigation.revision())) {
                continue;
            }

            const GridPoint start{position->x, position->y};
            const bool useFlowField =
                dependencies.flowFields.demandCount(goal) >=
                kFlowFieldThreshold;
            agent->pathRevision = dependencies.navigation.revision();

            if (useFlowField) {
                const auto& field = dependencies.flowFields.resolve(
                    dependencies.navigation, goal);
                if (!dependencies.flowFields.extractPath(
                        field, start, agent->path)) {
                    failRegularPath(
                        world, context, entity, *queue, *agent, dependencies);
                    continue;
                }
                assignExtractedPath(*agent, goal, false);
                dependencies.telemetry.recordFlowAssignment(
                    agent->path.size());
            } else {
                const auto& path = dependencies.pathCache.resolve(
                    dependencies.navigation,
                    start,
                    goal,
                    dependencies.pathScratch);
                if (!path.found) {
                    failRegularPath(
                        world, context, entity, *queue, *agent, dependencies);
                    continue;
                }
                assignPath(*agent, path, goal, false);
                dependencies.telemetry.recordPathCacheAssignment(
                    path.points.size());
            }

            dependencies.events.push_back(
                {context.tick,
                 DomainEventType::PathReady,
                 entity,
                 0,
                 0});
        }
    }

private:
    static void prepareFlowDemands(
        const ecs::World& world,
        NavigationSystemDependencies dependencies) {
        dependencies.flowFields.beginDemands();
        for (const auto entity :
             world.view<Position, OrderQueue, MovementAgent>()) {
            const auto* position = world.try_get<Position>(entity);
            const auto* queue = world.try_get<OrderQueue>(entity);
            const auto* agent = world.try_get<MovementAgent>(entity);
            const auto* directive =
                world.try_get<CombatDirective>(entity);
            if (!position || !queue || !agent) continue;

            dependencies.telemetry.recordNavigationAgent();
            if (queue->pending.empty() ||
                (directive &&
                 directive->mode == CombatMode::AttackTarget)) {
                continue;
            }

            const auto goal = queue->pending.front().target;
            if ((position->x == goal.x && position->y == goal.y) ||
                hasReusableRegularPath(
                    *agent, goal, dependencies.navigation.revision())) {
                continue;
            }
            dependencies.flowFields.addDemand(goal);
            dependencies.telemetry.recordPathRequest();
        }
    }

    static bool hasReusableRegularPath(
        const MovementAgent& agent,
        GridPoint goal,
        std::uint64_t navigationRevision) noexcept {
        return !agent.path.empty() && agent.hasPathGoal &&
               agent.pathGoal == goal &&
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
        if (!queue.pending.empty()) queue.pending.erase(queue.pending.begin());
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

    static std::int32_t distance(GridPoint a, GridPoint b) noexcept {
        const auto dx = a.x > b.x ? a.x - b.x : b.x - a.x;
        const auto dy = a.y > b.y ? a.y - b.y : b.y - a.y;
        return dx + dy;
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
        if (distance(start, targetPoint) <= weapon->range) {
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
        const auto* path = findAttackPath(
            dependencies.navigation,
            start,
            targetPoint,
            weapon->range,
            dependencies.pathCache,
            dependencies.pathScratch);
        if (!path || !path->found) {
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

        const auto goal =
            path->points.empty() ? start : path->points.back();
        agent.pathRevision = dependencies.navigation.revision();
        assignPath(agent, *path, goal, true);
        agent.chaseTarget = directive.forcedTarget;
        agent.chaseTargetPosition = targetPoint;
        dependencies.telemetry.recordAttackAssignment(
            path->points.size());
        dependencies.events.push_back(
            {context.tick,
             DomainEventType::PathReady,
             entity,
             0,
             0,
             directive.forcedTarget,
             0});
    }

    static const PathResult* findAttackPath(
        const NavigationGrid& navigation,
        GridPoint start,
        GridPoint target,
        std::int32_t range,
        GridPathCache& pathCache,
        GridPathfinderScratch& pathScratch) {
        std::vector<GridPoint> candidates;
        const auto boundedRange = std::max<std::int32_t>(0, range);
        for (std::int32_t y = 0; y < navigation.height(); ++y) {
            for (std::int32_t x = 0; x < navigation.width(); ++x) {
                const GridPoint candidate{x, y};
                if (!navigation.blocked(candidate) &&
                    distance(candidate, target) <= boundedRange) {
                    candidates.push_back(candidate);
                }
            }
        }
        std::stable_sort(
            candidates.begin(),
            candidates.end(),
            [start](GridPoint a, GridPoint b) {
                const auto aDistance = distance(start, a);
                const auto bDistance = distance(start, b);
                if (aDistance != bDistance) {
                    return aDistance < bDistance;
                }
                if (a.y != b.y) return a.y < b.y;
                return a.x < b.x;
            });

        for (const auto candidate : candidates) {
            const auto& path = pathCache.resolve(
                navigation, start, candidate, pathScratch);
            if (path.found) return &path;
        }
        return nullptr;
    }

    static void assignExtractedPath(
        MovementAgent& agent,
        GridPoint goal,
        bool combatPath) {
        agent.nextPoint = 0;
        agent.pathGoal = goal;
        agent.hasPathGoal = true;
        agent.combatPath = combatPath;
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
