#include <RTSEngine/Rts/MovementSystem.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/Simulation.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace rts;
using namespace rts::gameplay;

std::vector<std::uint64_t> runScenario() {
    RtsSimulation simulation(12, 8);
    simulation.setBlocked({2, 0}, true);
    simulation.setBlocked({2, 1}, true);
    simulation.setBlocked({2, 2}, true);

    const auto unit = simulation.createUnit({0, 0}, {1});
    assert(simulation.submit({1, 1, 1, CommandType::Move, unit, 5, 0, false}));
    assert(simulation.submit({1, 1, 1, CommandType::Move, unit, 99, 99, false}));
    assert(simulation.submit({1, 1, 2, CommandType::Move, unit, 5, 3, true}));

    std::vector<std::uint64_t> hashes;
    for (std::uint64_t tick = 0; tick < 20; ++tick) {
        simulation.step(tick);
        hashes.push_back(simulation.snapshot().worldHash);
    }

    const auto& snapshot = simulation.snapshot();
    assert(snapshot.entities.size() == 1);
    assert(snapshot.entities.front().x == 5);
    assert(snapshot.entities.front().y == 3);
    assert(!snapshot.entities.front().moving);
    assert(snapshot.entities.front().queuedOrders == 0);
    assert(!simulation.submit({0, 1, 3, CommandType::Stop, unit, 0, 0, false}));
    return hashes;
}

void testStablePath() {
    NavigationGrid grid(6, 5);
    grid.setBlocked({2, 0}, true);
    grid.setBlocked({2, 1}, true);
    grid.setBlocked({2, 2}, true);

    const auto first = GridPathfinder::find(grid, {0, 0}, {5, 0});
    const auto second = GridPathfinder::find(grid, {0, 0}, {5, 0});
    assert(first.found && second.found);
    assert(first.points == second.points);
    for (const auto point : first.points) assert(!grid.blocked(point));
}

void testStopAndFailure() {
    RtsSimulation simulation(5, 5);
    const auto unit = simulation.createUnit({0, 0}, {1});
    simulation.submit({0, 1, 1, CommandType::Move, unit, 4, 4, false});
    simulation.step(0);
    simulation.submit({1, 1, 2, CommandType::Stop, unit, 0, 0, false});
    simulation.step(1);
    const auto stopped = simulation.snapshot().entities.front();
    simulation.step(2);
    assert(simulation.snapshot().entities.front().x == stopped.x);
    assert(simulation.snapshot().entities.front().y == stopped.y);
    assert(!simulation.snapshot().entities.front().moving);

    simulation.setBlocked({4, 4}, true);
    simulation.submit({3, 1, 3, CommandType::Move, unit, 4, 4, false});
    simulation.step(3);
    bool failed = false;
    for (const auto& event : simulation.events()) {
        failed = failed || event.type == DomainEventType::PathFailed;
    }
    assert(failed);
}

ecs::Entity createMovingAgent(
    ecs::World& world,
    GridPoint position,
    std::vector<GridPoint> path,
    GridPoint goal,
    std::uint32_t blockedTicks = 0) {
    const auto entity = world.create();
    world.emplace<Position>(entity, Position{position.x, position.y});
    world.emplace<MoveSpeed>(entity, MoveSpeed{1});
    world.emplace<OrderQueue>(
        entity, OrderQueue{{Order{OrderType::Move, goal}}});
    MovementAgent agent;
    agent.path = std::move(path);
    agent.pathGoal = goal;
    agent.hasPathGoal = true;
    agent.blockedTicks = blockedTicks;
    world.emplace<MovementAgent>(entity, std::move(agent));
    return entity;
}

void testReservationChainsAndSwapRejection() {
    NavigationGrid navigation(4, 2);
    MovementReservationRuntime reservations(4, 2);
    std::vector<DomainEvent> events;

    ecs::World chainWorld;
    const auto follower = createMovingAgent(
        chainWorld, {0, 0}, {{1, 0}}, {1, 0});
    const auto leader = createMovingAgent(
        chainWorld, {1, 0}, {{2, 0}}, {2, 0});
    MovementSystem::run(
        chainWorld,
        {0, 0, ecs::Stage::Simulation},
        {navigation, reservations, events});
    const auto* followerPosition = chainWorld.try_get<Position>(follower);
    const auto* leaderPosition = chainWorld.try_get<Position>(leader);
    assert(followerPosition && followerPosition->x == 1);
    assert(leaderPosition && leaderPosition->x == 2);

    ecs::World swapWorld;
    const auto left = createMovingAgent(
        swapWorld, {0, 1}, {{1, 1}}, {1, 1});
    const auto right = createMovingAgent(
        swapWorld, {1, 1}, {{0, 1}}, {0, 1});
    events.clear();
    MovementSystem::run(
        swapWorld,
        {1, 0, ecs::Stage::Simulation},
        {navigation, reservations, events});
    const auto* leftPosition = swapWorld.try_get<Position>(left);
    const auto* rightPosition = swapWorld.try_get<Position>(right);
    const auto* leftAgent = swapWorld.try_get<MovementAgent>(left);
    const auto* rightAgent = swapWorld.try_get<MovementAgent>(right);
    assert(leftPosition && leftPosition->x == 0);
    assert(rightPosition && rightPosition->x == 1);
    assert(leftAgent && leftAgent->blockedTicks == 1);
    assert(rightAgent && rightAgent->blockedTicks == 1);
}

void testWaitingPriorityAndYield() {
    NavigationGrid navigation(4, 3);
    MovementReservationRuntime reservations(4, 3);
    std::vector<DomainEvent> events;

    ecs::World priorityWorld;
    const auto fresh = createMovingAgent(
        priorityWorld, {0, 0}, {{1, 0}}, {1, 0});
    const auto waiting = createMovingAgent(
        priorityWorld, {2, 0}, {{1, 0}}, {1, 0}, 3);
    MovementSystem::run(
        priorityWorld,
        {0, 0, ecs::Stage::Simulation},
        {navigation, reservations, events});
    const auto* freshPosition = priorityWorld.try_get<Position>(fresh);
    const auto* waitingPosition = priorityWorld.try_get<Position>(waiting);
    const auto* freshAgent = priorityWorld.try_get<MovementAgent>(fresh);
    assert(freshPosition && freshPosition->x == 0);
    assert(waitingPosition && waitingPosition->x == 1);
    assert(freshAgent && freshAgent->blockedTicks == 1);

    ecs::World yieldWorld;
    const auto blocker = createMovingAgent(
        yieldWorld, {1, 1}, {}, {1, 1});
    const auto mover = createMovingAgent(
        yieldWorld, {0, 1}, {{1, 1}}, {2, 1});
    (void)blocker;
    bool yielded = false;
    for (std::uint64_t tick = 0; tick < MovementSystem::kYieldThreshold; ++tick) {
        events.clear();
        MovementSystem::run(
            yieldWorld,
            {tick, 0, ecs::Stage::Simulation},
            {navigation, reservations, events});
        for (const auto& event : events) {
            yielded = yielded || event.type == DomainEventType::MoveYielded;
        }
    }
    const auto* moverPosition = yieldWorld.try_get<Position>(mover);
    const auto* moverAgent = yieldWorld.try_get<MovementAgent>(mover);
    const auto* moverQueue = yieldWorld.try_get<OrderQueue>(mover);
    assert(yielded);
    assert(moverPosition && !(moverPosition->x == 0 && moverPosition->y == 1));
    assert(moverAgent && moverAgent->path.empty());
    assert(moverAgent->blockedTicks == 0);
    assert(moverAgent->yieldOrdinal == 1);
    assert(moverQueue && moverQueue->pending.size() == 1);
}

} // namespace

int main() {
    testStablePath();
    testStopAndFailure();
    testReservationChainsAndSwapRejection();
    testWaitingPriorityAndYield();
    const auto first = runScenario();
    const auto second = runScenario();
    assert(first == second);
    std::cout << "rts navigation tests passed\n";
    return 0;
}
