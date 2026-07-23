#include <RTSEngine/Roguelite/RunSimulationArchive.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

using namespace rts;

void check(bool condition) {
    assert(condition);
    if (!condition) std::abort();
}

void configureHistoryRun(
    roguelite::RunSimulation& simulation,
    bool createActors) {
    simulation.setResources(100);
    simulation.setPlayerTeam(1);

    gameplay::UnitDefinition regular;
    regular.id = 1;
    regular.cellsPerTick = 1;
    regular.combat = {10, 0, 1, 1, 1, 3};
    simulation.registerUnit(regular);

    gameplay::UnitDefinition bossUnit;
    bossUnit.id = 2;
    bossUnit.cellsPerTick = 1;
    bossUnit.combat = {30, 1, 4, 1, 2, 10};
    simulation.registerUnit(bossUnit);

    check(simulation.registerAffix(
        {101, 1, {1500, 1, 1000, 1000, 2000}}));
    check(simulation.registerBoss(
        {201, 2, 1, 1, {2000, 2, 1250, 1000, 1500}}));
    check(simulation.registerLane({1, {0, 2}, {10, 2}, 1}));

    roguelite::ModifierDefinition salvage;
    salvage.id = 1;
    salvage.weight = 1;
    salvage.maxStacks = 1;
    salvage.effects = {
        {roguelite::WaveCompletionResourceStat(),
         roguelite::ModifierOperation::Add,
         5}
    };
    check(simulation.registerModifier(salvage));

    tower_defense::WaveDefinition first;
    first.id = 1;
    first.budget = 1;
    first.spawnIntervalTicks = 1;
    first.laneIds = {1};
    first.enemies = {{1, 1, 1, 1}};
    first.rewardPool = {1};
    first.rewardChoices = 1;
    check(simulation.registerWave(first));

    tower_defense::WaveDefinition second;
    second.id = 2;
    second.budget = 1;
    second.spawnIntervalTicks = 1;
    second.laneIds = {1};
    second.bossPool = {201};
    second.bossCount = 1;
    second.affixPool = {101};
    second.affixChoices = 1;
    second.rewardChoices = 0;
    check(simulation.registerWave(second));
    check(simulation.registerRun({7, {1, 2}}));

    if (createActors) {
        simulation.createBaseCore(
            {10, 2}, 1, {500, 0, 0, 0, 1, 0});
        simulation.createDefender(
            {7, 2}, {0}, 1, {500, 0, 100, 4, 1, 0});
    }
}

std::uint64_t advanceToFirstReward(
    roguelite::RunSimulation& simulation) {
    check(simulation.submit(
        {0, 1, 1, roguelite::CommandType::StartRun, 7}));
    for (std::uint64_t tick = 0; tick < 80; ++tick) {
        check(simulation.step(tick));
        if (simulation.state().phase == roguelite::RunPhase::RewardPending) {
            return tick;
        }
    }
    std::abort();
}

void checkFirstWaveRewardHistory(
    const roguelite::RunSimulation& simulation) {
    const auto& history = simulation.history();
    check(history.runId == 7);
    check(history.phase == roguelite::RunHistoryPhase::Active);
    check(!history.legacyImported);
    check(history.waves.size() == 1);

    const auto& wave = history.waves.front();
    check(wave.waveId == 1);
    check(wave.waveIndex == 0);
    check(wave.phase == roguelite::WaveResultPhase::RewardPending);
    check(wave.plannedEnemies == 1);
    check(wave.plannedBosses == 0);
    check(wave.enemiesDefeated == 1);
    check(wave.bossesDefeated == 0);
    check(wave.coreHealthStart == 500);
    check(wave.coreHealthEnd == 500);
    check(wave.coreHealthMaximum == 500);
    check(wave.resourcesStart == 100);
    check(wave.resourcesEnd == 103);
    check(wave.resourceDelta == 3);
    check(wave.resourceBonus == 0);
    check(wave.affixes.empty());
    check(wave.bosses.empty());
    check(wave.rewardChoices == std::vector<roguelite::ModifierId>({1}));
    check(wave.selectedModifier == 0);
    check(!wave.modifierApplied);
}

void testHistoryRoundTripAndCompletion() {
    roguelite::RunSimulation original(12, 5, 0x8181u);
    configureHistoryRun(original, true);
    const auto rewardTick = advanceToFirstReward(original);
    checkFirstWaveRewardHistory(original);

    const auto bytes = roguelite::EncodeRunSimulation(original);
    check(!bytes.empty());
    roguelite::RunSimulation restored(12, 5, 0x8181u);
    configureHistoryRun(restored, false);
    check(roguelite::DecodeRunSimulation(bytes, restored));
    check(restored.snapshot().worldHash == original.snapshot().worldHash);
    check(restored.history() == original.history());

    const roguelite::TickCommand choose{
        rewardTick + 1u, 1, 2,
        roguelite::CommandType::ChooseModifier, 1};
    check(original.submit(choose));
    check(restored.submit(choose));

    bool completed = false;
    for (std::uint64_t tick = rewardTick + 1u; tick < 180; ++tick) {
        check(original.step(tick));
        check(restored.step(tick));
        check(original.snapshot().worldHash == restored.snapshot().worldHash);
        check(original.history() == restored.history());
        if (original.state().phase == roguelite::RunPhase::Complete) {
            completed = true;
            break;
        }
    }
    check(completed);

    const auto& history = original.history();
    check(history.phase == roguelite::RunHistoryPhase::Complete);
    check(history.finishedTick >= history.startedTick);
    check(history.waves.size() == 2);

    const auto& first = history.waves[0];
    check(first.phase == roguelite::WaveResultPhase::Complete);
    check(first.selectedModifier == 1);
    check(first.modifierApplied);

    const auto& second = history.waves[1];
    check(second.waveId == 2);
    check(second.waveIndex == 1);
    check(second.phase == roguelite::WaveResultPhase::Complete);
    check(second.plannedEnemies == 1);
    check(second.plannedBosses == 1);
    check(second.enemiesDefeated == 1);
    check(second.bossesDefeated == 1);
    check(second.affixes ==
          std::vector<tower_defense::WaveAffixId>({101}));
    check(second.bosses ==
          std::vector<tower_defense::BossId>({201}));
    check(second.coreHealthStart == 500);
    check(second.coreHealthEnd == 500);
    check(second.resourcesStart == 103);
    check(second.resourcesEnd == 138);
    check(second.resourceDelta == 35);
    check(second.resourceBonus == 5);
    check(second.rewardChoices.empty());
    check(second.selectedModifier == 0);
    check(!second.modifierApplied);
    check(original.snapshot().availableResources == 138);
    check(roguelite::EncodeRunSimulation(original) ==
          roguelite::EncodeRunSimulation(restored));
}

void testFailedWaveIsSealed() {
    roguelite::RunSimulation simulation(8, 5, 0x9191u);

    gameplay::UnitDefinition enemy;
    enemy.id = 1;
    enemy.cellsPerTick = 1;
    enemy.combat = {100, 0, 20, 1, 1, 0};
    simulation.registerUnit(enemy);
    check(simulation.registerLane({1, {0, 2}, {6, 2}, 1}));

    tower_defense::WaveDefinition wave;
    wave.id = 1;
    wave.budget = 1;
    wave.laneIds = {1};
    wave.enemies = {{1, 1, 1, 1}};
    wave.rewardChoices = 0;
    check(simulation.registerWave(wave));
    check(simulation.registerRun({1, {1}}));
    simulation.createBaseCore(
        {6, 2}, 1, {5, 0, 0, 0, 1, 0});

    check(simulation.submit(
        {0, 1, 1, roguelite::CommandType::StartRun, 1}));
    bool failed = false;
    for (std::uint64_t tick = 0; tick < 80; ++tick) {
        check(simulation.step(tick));
        if (simulation.state().phase == roguelite::RunPhase::Failed) {
            failed = true;
            break;
        }
    }
    check(failed);
    const auto& history = simulation.history();
    check(history.phase == roguelite::RunHistoryPhase::Failed);
    check(history.finishedTick != 0);
    check(history.waves.size() == 1);
    check(history.waves.front().phase ==
          roguelite::WaveResultPhase::Failed);
    check(history.waves.front().coreHealthStart == 5);
    check(history.waves.front().coreHealthEnd == 0);
    check(history.waves.front().resourcesStart == 0);
    check(history.waves.front().resourcesEnd == 0);
    check(history.waves.front().resourceDelta == 0);
}

} // namespace

int main() {
    testHistoryRoundTripAndCompletion();
    testFailedWaveIsSealed();
    std::cout << "roguelite history tests passed\n";
    return 0;
}
