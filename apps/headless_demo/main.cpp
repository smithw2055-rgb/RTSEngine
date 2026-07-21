#include <RTSEngine/Rts/Simulation.h>

#include <cstdint>
#include <iostream>

int main() {
    rts::gameplay::RtsSimulation simulation(16, 10);
    for (std::int32_t y = 0; y < 5; ++y) {
        simulation.setBlocked({4, y}, true);
    }

    const auto unit = simulation.createUnit({1, 1}, {1});
    simulation.submit({1, 1, 1, rts::gameplay::CommandType::Move, unit, 9, 1, false});
    simulation.submit({1, 1, 2, rts::gameplay::CommandType::Move, unit, 9, 6, true});

    for (std::uint64_t tick = 0; tick < 32; ++tick) {
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
    std::cout << "queued_orders=" << entity.queuedOrders << '\n';
    std::cout << "navigation_revision=" << simulation.navigation().revision() << '\n';
    std::cout << "hash=" << snapshot.worldHash << '\n';

    return entity.x == 9 && entity.y == 6 && !entity.moving ? 0 : 1;
}
