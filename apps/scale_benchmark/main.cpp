#include <RTSEngine/Rts/Simulation.h>

#include <chrono>
#include <cstdint>
#include <iostream>

int main() {
    using namespace rts::gameplay;

    constexpr std::int32_t width = 64;
    constexpr std::int32_t height = 64;
    constexpr std::uint32_t unitCount = 1000;
    constexpr std::uint64_t ticks = 96;

    RtsSimulation simulation(width, height);
    for (std::int32_t y = 0; y < height; ++y) {
        if (y >= 30 && y <= 34) continue;
        if (!simulation.setBlocked({48, y}, true)) return 1;
    }

    std::uint32_t sequence = 1;
    for (std::int32_t y = 0; y < 25; ++y) {
        for (std::int32_t x = 0; x < 40; ++x) {
            const auto unit = simulation.createUnit({x, y}, {1});
            TickCommand move;
            move.targetTick = 0;
            move.issuer = 1;
            move.sequence = sequence++;
            move.type = CommandType::Move;
            move.subject = unit;
            move.targetX = 63;
            move.targetY = 63;
            if (!simulation.submit(move)) return 2;
        }
    }
    if (sequence != unitCount + 1u) return 3;

    std::uint64_t blockedEvents = 0;
    std::uint64_t yieldedEvents = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        simulation.step(tick);
        for (const auto& event : simulation.events()) {
            if (event.type == DomainEventType::MoveBlocked) ++blockedEvents;
            if (event.type == DomainEventType::MoveYielded) ++yieldedEvents;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);

    const auto& flow = simulation.flowFields().stats();
    const auto& paths = simulation.pathCache().stats();
    const auto& reservations = simulation.movementReservations();

    std::cout << "RTSEngine 1000-agent benchmark\n";
    std::cout << "ticks=" << ticks << '\n';
    std::cout << "final_world_hash=" << simulation.snapshot().worldHash << '\n';
    std::cout << "elapsed_us=" << elapsed.count() << '\n';
    std::cout << "flow_builds=" << flow.builds << '\n';
    std::cout << "flow_hits=" << flow.hits << '\n';
    std::cout << "flow_path_extractions=" << flow.pathExtractions << '\n';
    std::cout << "path_cache_hits=" << paths.hits << '\n';
    std::cout << "path_cache_misses=" << paths.misses << '\n';
    std::cout << "move_blocked_events=" << blockedEvents << '\n';
    std::cout << "move_yielded_events=" << yieldedEvents << '\n';
    std::cout << "reservation_intent_capacity="
              << reservations.intentCapacity() << '\n';
    std::cout << "reservation_rejected_capacity="
              << reservations.rejectedCapacity() << '\n';
    return 0;
}
