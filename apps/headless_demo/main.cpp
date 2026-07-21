#include <RTSEngine/Rts/Simulation.h>

#include <cstdint>
#include <iostream>

int main() {
    rts::gameplay::RtsSimulation simulation;
    const auto unit = simulation.createUnit({0, 0}, {1});
    simulation.submit({1, 1, 1, rts::gameplay::CommandType::Move, unit, 8, 4});

    for (std::uint64_t tick = 0; tick < 12; ++tick) {
        simulation.step(tick);
    }

    const auto& snapshot = simulation.snapshot();
    if (snapshot.entities.size() != 1) {
        return 1;
    }

    const auto& entity = snapshot.entities.front();
    std::cout << "tick=" << snapshot.tick << '\n';
    std::cout << "entity=" << entity.entity.index << ':' << entity.entity.generation << '\n';
    std::cout << "position=" << entity.x << ',' << entity.y << '\n';
    std::cout << "moving=" << (entity.moving ? "yes" : "no") << '\n';
    std::cout << "hash=" << snapshot.worldHash << '\n';

    return entity.x == 8 && entity.y == 4 && !entity.moving ? 0 : 1;
}
