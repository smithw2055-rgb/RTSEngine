#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/Simulation.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

std::vector<std::uint64_t> runScenario() {
    rts::gameplay::RtsSimulation simulation(12, 8);
    simulation.setBlocked({2, 0}, true);
    simulation.setBlocked({2, 1}, true);
    simulation.setBlocked({2, 2}, true);

    const auto unit = simulation.createUnit({0, 0}, {1});
    assert(simulation.submit({1, 1, 1, rts::gameplay::CommandType::Move, unit, 5, 0, false}));
    assert(simulation.submit({1, 1, 1, rts::gameplay::CommandType::Move, unit, 99, 99, false}));
    assert(simulation.submit({1, 1, 2, rts::gameplay::CommandType::Move, unit, 5, 3, true}));

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
    assert(!simulation.submit({0, 1, 3, rts::gameplay::CommandType::Stop, unit, 0, 0, false}));
    return hashes;
}

void testStablePath() {
    rts::gameplay::NavigationGrid grid(6, 5);
    grid.setBlocked({2, 0}, true);
    grid.setBlocked({2, 1}, true);
    grid.setBlocked({2, 2}, true);

    const auto first = rts::gameplay::GridPathfinder::find(grid, {0, 0}, {5, 0});
    const auto second = rts::gameplay::GridPathfinder::find(grid, {0, 0}, {5, 0});
    assert(first.found && second.found);
    assert(first.points == second.points);
    for (const auto point : first.points) {
        assert(!grid.blocked(point));
    }
}

void testStopAndFailure() {
    rts::gameplay::RtsSimulation simulation(5, 5);
    const auto unit = simulation.createUnit({0, 0}, {1});
    simulation.submit({0, 1, 1, rts::gameplay::CommandType::Move, unit, 4, 4, false});
    simulation.step(0);
    simulation.submit({1, 1, 2, rts::gameplay::CommandType::Stop, unit, 0, 0, false});
    simulation.step(1);
    const auto stopped = simulation.snapshot().entities.front();
    simulation.step(2);
    assert(simulation.snapshot().entities.front().x == stopped.x);
    assert(simulation.snapshot().entities.front().y == stopped.y);
    assert(!simulation.snapshot().entities.front().moving);

    simulation.setBlocked({4, 4}, true);
    simulation.submit({3, 1, 3, rts::gameplay::CommandType::Move, unit, 4, 4, false});
    simulation.step(3);
    bool failed = false;
    for (const auto& event : simulation.events()) {
        failed = failed || event.type == rts::gameplay::DomainEventType::PathFailed;
    }
    assert(failed);
}

} // namespace

int main() {
    testStablePath();
    testStopAndFailure();
    const auto first = runScenario();
    const auto second = runScenario();
    assert(first == second);
    std::cout << "rts navigation tests passed\n";
    return 0;
}
