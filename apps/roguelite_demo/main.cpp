#include <RTSEngine/Roguelite/RunSimulation.h>

#include <cstdint>
#include <iostream>

int main() {
    using namespace rts;
    using namespace rts::roguelite;

    RunSimulation simulation(12, 8, 20260722);
    simulation.setResources(100);
    simulation.setPlayerTeam(1);

    gameplay::UnitDefinition enemy;
    enemy.id = 1;
    enemy.cellsPerTick = 1;
    enemy.combat.maximumHealth = 1;
    enemy.combat.weaponDamage = 1;
    enemy.combat.weaponRange = 1;
    enemy.combat.cooldownTicks = 1;
    simulation.registerUnit(enemy);

    simulation.registerLane({1, {10, 4}, {1, 4}, 1});

    ModifierDefinition salvage;
    salvage.id = 1;
    salvage.tags = {MakeTagId("build.economy")};
    salvage.effects = {
        {WaveCompletionResourceStat(),
         ModifierOperation::Add, 10}
    };
    simulation.registerModifier(salvage);

    ModifierDefinition compound;
    compound.id = 2;
    compound.requiredTags = {
        MakeTagId("build.economy")
    };
    compound.effects = {
        {WaveCompletionResourceStat(),
         ModifierOperation::Multiply, 1500}
    };
    simulation.registerModifier(compound);

    tower_defense::WaveDefinition wave;
    wave.id = 1;
    wave.budget = 1;
    wave.enemyTeamId = 2;
    wave.laneIds = {1};
    wave.enemies = {{1, 1, 1, 1}};
    wave.rewardPool = {1};
    wave.rewardChoices = 1;
    simulation.registerWave(wave);

    wave.id = 2;
    wave.rewardPool = {2};
    simulation.registerWave(wave);
    simulation.registerRun({1, {1, 2}});

    gameplay::CombatStats core;
    core.maximumHealth = 100;
    simulation.createBaseCore({1, 4}, 1, core);

    gameplay::CombatStats defender;
    defender.maximumHealth = 20;
    defender.weaponDamage = 10;
    defender.weaponRange = 20;
    defender.cooldownTicks = 1;
    simulation.createDefender({6, 4}, {0}, 1, defender);

    simulation.submit({0, 1, 1, CommandType::StartRun, 1});

    std::uint32_t sequence = 2;
    std::uint32_t queuedChoices = 0;
    for (std::uint64_t tick = 0; tick < 64; ++tick) {
        if (!simulation.step(tick)) return 1;

        if (simulation.state().phase ==
                RunPhase::RewardPending &&
            queuedChoices == simulation.state().completedWaves) {
            const auto& choices =
                simulation.tower().snapshot().rewardChoices;
            if (choices.empty()) return 1;
            simulation.submit(
                {tick + 1, 1, sequence++,
                 CommandType::ChooseModifier,
                 choices.front()});
            ++queuedChoices;
        }

        if (simulation.state().phase == RunPhase::Complete ||
            simulation.state().phase == RunPhase::Failed) {
            break;
        }
    }

    const auto& snapshot = simulation.snapshot();
    std::cout << "tick=" << snapshot.tick << '\n';
    std::cout << "run=" << snapshot.state.runId << '\n';
    std::cout << "completed_waves="
              << snapshot.state.completedWaves << '\n';
    std::cout << "modifier_stacks="
              << snapshot.modifiers.size() << '\n';
    std::cout << "wave_resource_bonus="
              << snapshot.waveCompletionResourceBonus << '\n';
    std::cout << "resources="
              << snapshot.availableResources << '\n';
    std::cout << "hash=" << snapshot.worldHash << '\n';

    return snapshot.state.phase == RunPhase::Complete &&
                   snapshot.state.completedWaves == 2 &&
                   snapshot.modifiers.size() == 2
        ? 0
        : 1;
}
