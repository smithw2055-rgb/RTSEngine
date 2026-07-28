#include <RTSEngine/RtsDesktop/LoopbackMultiplayerRuntime.h>

#include <cstdlib>
#include <iostream>
#include <vector>

int main() {
    using namespace rts;

    rts_desktop::LoopbackMultiplayerConfig config;
    config.lockstep.sessionId = 20260728u;
    config.lockstep.inputDelayTicks = 2;
    config.network.baseLatencyMs = 3;
    config.network.jitterMs = 2;
    config.network.reorderDelayMs = 3;
    config.network.lossBasisPoints = 300;
    config.network.duplicateBasisPoints = 200;

    rts_desktop::LoopbackMultiplayerRuntime runtime(config);
    gameplay::CombatStats combat;
    combat.maximumHealth = 20;
    combat.weaponDamage = 2;
    combat.weaponRange = 2;

    const auto hostUnit = runtime.hostSession().createUnit(
        {2, 2}, {1}, 1, combat, 8);
    const auto clientUnit = runtime.clientSession().createUnit(
        {2, 2}, {1}, 1, combat, 8);
    if (!hostUnit.valid() || hostUnit != clientUnit ||
        !runtime.connect() || !runtime.readyAndStart()) {
        std::cerr << "multiplayer initialization failed\n";
        return EXIT_FAILURE;
    }

    for (std::uint64_t tick = 0; tick < 12; ++tick) {
        std::vector<gameplay::TickCommand> hostCommands;
        std::vector<gameplay::TickCommand> clientCommands;
        if (tick == 0) {
            gameplay::TickCommand move;
            move.type = gameplay::CommandType::Move;
            move.subject = hostUnit;
            move.targetX = 10;
            move.targetY = 2;
            hostCommands.push_back(move);
        }
        if (!runtime.advanceTick(
                std::move(hostCommands),
                std::move(clientCommands))) {
            std::cerr << "multiplayer Tick failed at " << tick << '\n';
            return EXIT_FAILURE;
        }
    }

    const auto hash = gameplay::RtsGameSessionArchive::authoritativeHash(
        runtime.hostSession());
    std::cout << "RTS multiplayer loopback complete: Tick "
              << runtime.hostSession().simulation().lastCompletedTick()
              << ", hash " << hash << '\n';
    return runtime.hashesMatch() ? EXIT_SUCCESS : EXIT_FAILURE;
}
