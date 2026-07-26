#include <RTSEngine/TowerDefense/Simulation.h>

#include <cstdint>
#include <iostream>

int main() {
    using namespace rts;
    using namespace rts::tower_defense;

    TowerDefenseSimulation simulation(16, 8, 20260722);
    simulation.setResources(25);
    simulation.setPlayerTeam(1);

    gameplay::UnitDefinition raider;
    raider.id = 10;
    raider.cellsPerTick = 1;
    raider.combat = {10, 0, 2, 1, 1, 3};
    simulation.registerUnit(raider);

    simulation.registerLane({1, {0, 2}, {14, 3}, 3});
    simulation.registerLane({2, {0, 4}, {14, 3}, 1});

    simulation.registerReward({101, 3, 20});
    simulation.registerReward({102, 2, 35});
    simulation.registerReward({103, 1, 50});

    WaveDefinition wave;
    wave.id = 1;
    wave.budget = 8;
    wave.spawnIntervalTicks = 2;
    wave.enemyTeamId = 2;
    wave.laneIds = {1, 2};
    wave.enemies = {{10, 2, 1, 0}};
    wave.rewardPool = {101, 102, 103};
    wave.rewardChoices = 3;
    simulation.registerWave(wave);

    simulation.createBaseCore(
        {14, 3}, 1, {50, 1, 0, 0, 1, 0});
    simulation.createDefender(
        {9, 3}, {0}, 1, {80, 0, 15, 6, 1, 0});

    TickCommand start;
    start.targetTick = 0;
    start.issuer = 1;
    start.sequence = 1;
    start.type = CommandType::StartWave;
    start.objectId = 1;
    simulation.submit(start);

    bool rewardSubmitted = false;
    for (std::uint64_t tick = 0; tick < 120; ++tick) {
        if (!simulation.step(tick)) return 1;

        if (!rewardSubmitted &&
            simulation.snapshot().wave.phase ==
                WavePhase::RewardPending &&
            !simulation.snapshot().rewardChoices.empty()) {
            TickCommand choose;
            choose.targetTick = tick + 1;
            choose.issuer = 1;
            choose.sequence = 2;
            choose.type = CommandType::ChooseReward;
            choose.objectId =
                simulation.snapshot().rewardChoices.front();
            simulation.submit(choose);
            rewardSubmitted = true;
        }

        if (simulation.snapshot().wave.phase ==
            WavePhase::Complete) {
            break;
        }
    }

    const auto& snapshot = simulation.snapshot();
    std::cout << "tick=" << snapshot.tick << '\n';
    std::cout << "wave=" << snapshot.wave.waveId << '\n';
    std::cout << "phase="
              << static_cast<std::uint32_t>(snapshot.wave.phase)
              << '\n';
    std::cout << "planned=" << snapshot.plannedSpawns << '\n';
    std::cout << "spawned=" << snapshot.wave.spawned << '\n';
    std::cout << "resolved=" << snapshot.wave.resolved << '\n';
    std::cout << "core_health="
              << snapshot.coreHealthCurrent << '/'
              << snapshot.coreHealthMaximum << '\n';
    std::cout << "resources="
              << simulation.resources().available << '\n';
    std::cout << "hash=" << snapshot.worldHash << '\n';

    return snapshot.wave.phase == WavePhase::Complete &&
                   snapshot.coreHealthCurrent > 0
        ? 0
        : 1;
}
