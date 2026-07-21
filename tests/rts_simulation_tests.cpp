#include <RTSEngine/Rts/Simulation.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

std::vector<std::uint64_t> runScenario() {
    rts::gameplay::RtsSimulation simulation;
    const auto unit = simulation.createUnit({0, 0}, {2});

    assert(simulation.submit({1, 1, 1, rts::gameplay::CommandType::Move, unit, 5, 3}));
    assert(simulation.submit({1, 1, 1, rts::gameplay::CommandType::Move, unit, 99, 99}));

    std::vector<std::uint64_t> hashes;
    for (std::uint64_t tick = 0; tick < 6; ++tick) {
        simulation.step(tick);
        hashes.push_back(simulation.snapshot().worldHash);
    }

    const auto& snapshot = simulation.snapshot();
    assert(snapshot.entities.size() == 1);
    assert(snapshot.entities.front().x == 5);
    assert(snapshot.entities.front().y == 3);
    assert(!snapshot.entities.front().moving);
    assert(!simulation.submit({0, 1, 2, rts::gameplay::CommandType::Move, unit, 0, 0}));
    return hashes;
}

} // namespace

int main() {
    const auto first = runScenario();
    const auto second = runScenario();
    assert(first == second);
    std::cout << "rts simulation tests passed\n";
    return 0;
}
