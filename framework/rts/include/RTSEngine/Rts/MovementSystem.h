#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/MovementReservations.h>
#include <RTSEngine/Rts/OrderSystem.h>
#include <RTSEngine/Rts/RuntimeTelemetry.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

struct MovementSystemDependencies {
    const NavigationGrid& navigation;
    MovementReservationRuntime& reservations;
    std::vector<DomainEvent>& events;
    RuntimeTelemetry* telemetry{};

    MovementSystemDependencies(
        const NavigationGrid& navigationValue,
        MovementReservationRuntime& reservationsValue,
        std::vector<DomainEvent>& eventsValue)
        : navigation(navigationValue),
          reservations(reservationsValue),
          events(eventsValue) {}

    MovementSystemDependencies(
        const NavigationGrid& navigationValue,
        MovementReservationRuntime& reservationsValue,
        RuntimeTelemetry& telemetryValue,
        std::vector<DomainEvent>& eventsValue)
        : navigation(navigationValue),
          reservations(reservationsValue),
          events(eventsValue),
          telemetry(&telemetryValue) {}
};

class MovementSystem final {
public:
    static constexpr std::uint32_t kYieldThreshold = 4u;
    static constexpr std::uint32_t kRepathThreshold = 12u;

    static void run(
        ecs::World& world,
        const ecs::SystemContext& context,
        MovementSystemDependencies dependencies) {
        RuntimeTelemetry localTelemetry;
        if (!dependencies.telemetry) {
            dependencies.telemetry = &localTelemetry;
        }
        dependencies.telemetry->beginMovementTick(context.tick);

        std::int32_t maximumSteps = 0;
        world.eachRef<Position, MoveSpeed, OrderQueue, MovementAgent>(
            [&](ecs::Entity,
                Position&,
                MoveSpeed& speed,
                OrderQueue&,
                MovementAgent& agent) {
                if (agent.path.empty()) return;
                dependencies.telemetry->recordMovementAgent();
                maximumSteps = std::max(
                    maximumSteps,
                    std::max<std::int32_t>(1, speed.cellsPerTick));
            });

        // Occupancy is rebuilt once per Tick. Accepted simultaneous moves are
        // committed in two phases so later substeps reuse the current map.
        rebuildOccupancy(
            world, dependencies.reservations, *dependencies.telemetry);
        for (std::int32_t step = 0; step < maximumSteps; ++step) {
            dependencies.telemetry->recordMovementSubstep();
            runSubstep(world, context, step, dependencies);
        }
        completePaths(
            world,
            context,
            *dependencies.telemetry,
            dependencies.events);
    }

private:
    static void rebuildOccupancy(
        const ecs::World& world,
        MovementReservationRuntime& reservations,
        RuntimeTelemetry& telemetry) {
        reservations.clearOccupancy();
        world.eachRef<Position, MovementAgent>(
            [&](ecs::Entity entity,
                const Position& position,
                const MovementAgent&) {
                reservations.addOccupant(
                    entity, {position.x, position.y});
            });
        telemetry.recordOccupancyRebuild(
            reservations.occupiedCellCount(),
            reservations.maximumCellOccupancy());
    }

    static void runSubstep(
        ecs::World& world,
        const ecs::SystemContext& context,
        std::int32_t step,
        MovementSystemDependencies dependencies) {
        auto& reservations = dependencies.reservations;
        reservations.beginIntents();

        world.eachRef<Position, MoveSpeed, OrderQueue, MovementAgent>(
            [&](ecs::Entity entity,
                Position& position,
                MoveSpeed& speed,
                OrderQueue&,
                MovementAgent& agent) {
                if (agent.path.empty() ||
                    agent.nextPoint >= agent.path.size() ||
                    step >= std::max<std::int32_t>(1, speed.cellsPerTick) ||
                    shouldPauseForCombat(world, entity, position)) {
                    return;
                }

                const auto destination = agent.path[agent.nextPoint];
                if (!dependencies.navigation.contains(destination) ||
                    dependencies.navigation.blocked(destination)) {
                    clearPathForReplan(agent);
                    agent.blockedTicks = 0;
                    dependencies.telemetry->recordBlockedSignal();
                    dependencies.events.push_back(
                        {context.tick,
                         DomainEventType::MoveBlocked,
                         entity,
                         0,
                         2});
                    return;
                }

                reservations.addIntent(
                    {entity,
                     {position.x, position.y},
                     destination,
                     agent.blockedTicks});
            });

        reservations.arbitrate();
        std::uint32_t resolvedAccepted = 0;
        for (std::size_t index = 0;
             index < reservations.intentCount(); ++index) {
            if (reservations.accepted(index)) ++resolvedAccepted;
        }
        const auto committedAccepted = reservations.commitAcceptedMoves();
        const bool occupancyCommitValid =
            committedAccepted == resolvedAccepted;
        std::uint32_t accepted = 0;
        if (occupancyCommitValid) {
            for (std::size_t index = 0;
                 index < reservations.intentCount(); ++index) {
                if (!reservations.accepted(index)) continue;
                const auto& intent = reservations.intent(index);
                auto* position = world.try_get<Position>(intent.entity);
                auto* agent = world.try_get<MovementAgent>(intent.entity);
                if (!position || !agent ||
                    position->x != intent.source.x ||
                    position->y != intent.source.y ||
                    agent->nextPoint >= agent->path.size()) {
                    continue;
                }
                position->x = intent.destination.x;
                position->y = intent.destination.y;
                ++agent->nextPoint;
                agent->blockedTicks = 0;
                ++accepted;
            }
        }
        dependencies.telemetry->recordReservationBatch(
            static_cast<std::uint32_t>(reservations.intentCount()),
            accepted,
            static_cast<std::uint32_t>(reservations.rejected().size()));

        for (const auto index : reservations.rejected()) {
            const auto& intent = reservations.intent(index);
            auto* agent = world.try_get<MovementAgent>(intent.entity);
            if (!agent || agent->path.empty()) continue;
            if (agent->blockedTicks !=
                std::numeric_limits<std::uint32_t>::max()) {
                ++agent->blockedTicks;
            }
            if (agent->blockedTicks == 1u ||
                agent->blockedTicks == kYieldThreshold ||
                agent->blockedTicks == kRepathThreshold) {
                dependencies.telemetry->recordBlockedSignal();
                dependencies.events.push_back(
                    {context.tick,
                     DomainEventType::MoveBlocked,
                     intent.entity,
                     0,
                     0,
                     {},
                     static_cast<std::int32_t>(agent->blockedTicks)});
            }
        }

        for (const auto index : reservations.rejected()) {
            const auto& intent = reservations.intent(index);
            auto* position = world.try_get<Position>(intent.entity);
            auto* agent = world.try_get<MovementAgent>(intent.entity);
            if (!position || !agent || agent->path.empty() ||
                agent->blockedTicks < kYieldThreshold) {
                continue;
            }

            if (tryYield(
                    intent.entity,
                    *position,
                    *agent,
                    dependencies.navigation,
                    reservations)) {
                dependencies.telemetry->recordYieldedMove();
                dependencies.events.push_back(
                    {context.tick,
                     DomainEventType::MoveYielded,
                     intent.entity,
                     0,
                     0,
                     {},
                     static_cast<std::int32_t>(agent->yieldOrdinal)});
                continue;
            }

            if (agent->blockedTicks >= kRepathThreshold) {
                clearPathForReplan(*agent);
                agent->blockedTicks = 0;
                if (agent->yieldOrdinal !=
                    std::numeric_limits<std::uint32_t>::max()) {
                    ++agent->yieldOrdinal;
                }
                dependencies.telemetry->recordRepathRecovery();
                dependencies.events.push_back(
                    {context.tick,
                     DomainEventType::MoveYielded,
                     intent.entity,
                     0,
                     1,
                     {},
                     static_cast<std::int32_t>(agent->yieldOrdinal)});
            }
        }
    }

    static bool tryYield(
        ecs::Entity entity,
        Position& position,
        MovementAgent& agent,
        const NavigationGrid& navigation,
        MovementReservationRuntime& reservations) {
        static constexpr GridPoint directions[] = {
            {0, -1}, {1, 0}, {0, 1}, {-1, 0}
        };
        const GridPoint source{position.x, position.y};
        const auto sourceDistance = distance(source, agent.pathGoal);
        const auto firstDirection = static_cast<std::size_t>(
            (entity.index + agent.yieldOrdinal) % 4u);

        for (std::size_t offset = 0; offset < 4u; ++offset) {
            const auto direction =
                directions[(firstDirection + offset) % 4u];
            const GridPoint candidate{
                source.x + direction.x,
                source.y + direction.y};
            if (!navigation.contains(candidate) ||
                navigation.blocked(candidate) ||
                reservations.occupied(candidate) ||
                distance(candidate, agent.pathGoal) > sourceDistance + 1) {
                continue;
            }
            if (!reservations.moveOccupant(entity, source, candidate)) {
                continue;
            }

            position.x = candidate.x;
            position.y = candidate.y;
            clearPathForReplan(agent);
            agent.blockedTicks = 0;
            if (agent.yieldOrdinal !=
                std::numeric_limits<std::uint32_t>::max()) {
                ++agent.yieldOrdinal;
            }
            return true;
        }
        return false;
    }

    static void clearPathForReplan(MovementAgent& agent) {
        agent.path.clear();
        agent.nextPoint = 0;
        agent.hasPathGoal = false;
        agent.combatPath = false;
        agent.chaseTarget = {};
        agent.chaseTargetPosition = {};
    }

    static void completePaths(
        ecs::World& world,
        const ecs::SystemContext& context,
        RuntimeTelemetry& telemetry,
        std::vector<DomainEvent>& events) {
        world.eachRef<Position, OrderQueue, MovementAgent>(
            [&](ecs::Entity entity,
                Position&,
                OrderQueue& queue,
                MovementAgent& agent) {
                if (agent.path.empty() ||
                    agent.nextPoint != agent.path.size()) {
                    return;
                }

                const bool completedCombatPath = agent.combatPath;
                OrderSystem::clearPath(agent);
                agent.blockedTicks = 0;
                if (completedCombatPath) return;

                if (!queue.pending.empty()) {
                    queue.pending.erase(queue.pending.begin());
                }
                OrderSystem::applyFrontOrderMode(world, entity, queue);
                telemetry.recordCompletedMove();
                events.push_back(
                    {context.tick,
                     DomainEventType::MoveCompleted,
                     entity,
                     0,
                     0});
            });
    }

    static std::int32_t distance(GridPoint a, GridPoint b) noexcept {
        const auto dx = a.x > b.x ? a.x - b.x : b.x - a.x;
        const auto dy = a.y > b.y ? a.y - b.y : b.y - a.y;
        return dx + dy;
    }

    static bool shouldPauseForCombat(
        const ecs::World& world,
        ecs::Entity entity,
        const Position& position) {
        const auto* directive = world.try_get<CombatDirective>(entity);
        const auto* target = world.try_get<CombatTarget>(entity);
        const auto* weapon = world.try_get<Weapon>(entity);
        if (!directive || !target || !weapon ||
            directive->mode == CombatMode::PassiveMove ||
            !world.alive(target->entity)) {
            return false;
        }
        const auto* targetPosition =
            world.try_get<Position>(target->entity);
        const auto* targetHealth = world.try_get<Health>(target->entity);
        return targetPosition && targetHealth && targetHealth->current > 0 &&
               distance(
                   {position.x, position.y},
                   {targetPosition->x, targetPosition->y}) <= weapon->range;
    }
};

} // namespace rts::gameplay
