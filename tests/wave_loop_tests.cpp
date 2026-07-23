#include <RTSEngine/TowerDefense/WaveLoopArchive.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace rts;
using namespace rts::tower_defense;

void check(bool condition) {
    assert(condition);
    if (!condition) std::abort();
}

bool hasEvent(
    const WaveLoopSimulation& simulation,
    WaveLoopEventType type,
    std::uint32_t reason = 0) {
    return std::any_of(
        simulation.events().begin(), simulation.events().end(),
        [type, reason](const WaveLoopEvent& event) {
            return event.type == type &&
                   (reason == 0 || event.reason == reason);
        });
}

void configureTwoWaveLoop(
    WaveLoopSimulation& simulation,
    bool createActors,
    std::uint32_t interWaveTicks = 5,
    bool allowEarlyStart = true) {
    gameplay::UnitDefinition runner;
    runner.id = 1;
    runner.cellsPerTick = 1;
    runner.combat = {40, 0, 2, 1, 2, 0};
    simulation.registerUnit(runner);

    gameplay::UnitDefinition bossUnit;
    bossUnit.id = 2;
    bossUnit.cellsPerTick = 1;
    bossUnit.combat = {70, 1, 4, 1, 2, 0};
    simulation.registerUnit(bossUnit);

    check(simulation.registerBoss(
        {10, 2, 1, 1, {1500, 1, 1250, 1000, 1000}}));
    check(simulation.registerLane({1, {0, 2}, {10, 2}, 1}));

    WaveDefinition first;
    first.id = 1;
    first.budget = 1;
    first.spawnIntervalTicks = 1;
    first.laneIds = {1};
    first.enemies = {{1, 1, 1, 1}};
    first.rewardChoices = 0;
    check(simulation.registerWave(first));

    WaveDefinition second;
    second.id = 2;
    second.budget = 1;
    second.spawnIntervalTicks = 1;
    second.laneIds = {1};
    second.bossPool = {10};
    second.bossCount = 1;
    second.rewardChoices = 0;
    check(simulation.registerWave(second));

    check(simulation.registerSequence(
        {77, {1, 2}, 3, interWaveTicks, allowEarlyStart}));

    if (createActors) {
        simulation.createBaseCore(
            {10, 2}, 1, {500, 0, 0, 0, 1, 0});
        simulation.createDefender(
            {7, 2}, {0}, 1, {500, 0, 10, 6, 1, 0});
    }
}

void testAutomaticFirstWaveAndEarlySecondWaveRoundTrip() {
    WaveLoopSimulation original(12, 5, 0x5151u);
    configureTwoWaveLoop(original, true);
    check(original.submit(
        {0, 1, 1, WaveLoopCommandType::StartSequence, 77}));

    check(original.step(0));
    check(original.snapshot().sequence.phase == WaveSequencePhase::Preparing);
    check(original.snapshot().sequence.currentWave == 1);
    check(original.snapshot().preparationTicksRemaining == 3);
    check(hasEvent(original, WaveLoopEventType::SequenceStarted));
    check(hasEvent(original, WaveLoopEventType::PreparationStarted));

    check(original.step(1));
    check(original.snapshot().preparationTicksRemaining == 2);
    check(original.step(2));
    check(original.snapshot().preparationTicksRemaining == 1);
    check(original.step(3));
    check(original.snapshot().sequence.phase == WaveSequencePhase::WaveActive);
    check(original.snapshot().sequence.currentWave == 1);

    std::uint64_t preparationTick = 0;
    bool reachedSecondPreparation = false;
    for (std::uint64_t tick = 4; tick < 120; ++tick) {
        check(original.step(tick));
        if (original.snapshot().sequence.phase ==
                WaveSequencePhase::Preparing &&
            original.snapshot().sequence.waveIndex == 1) {
            preparationTick = tick;
            reachedSecondPreparation = true;
            break;
        }
    }
    check(reachedSecondPreparation);
    check(original.snapshot().sequence.completedWaves == 1);
    check(original.snapshot().sequence.currentWave == 2);
    check(original.snapshot().preparationTicksRemaining == 5);

    const auto bytes = EncodeWaveLoopSimulation(original);
    check(!bytes.empty());

    WaveLoopSimulation restored(12, 5, 0x5151u);
    configureTwoWaveLoop(restored, false);
    check(DecodeWaveLoopSimulation(bytes, restored));
    check(restored.snapshot().worldHash == original.snapshot().worldHash);
    check(restored.snapshot().sequence.phase == WaveSequencePhase::Preparing);
    check(restored.snapshot().preparationTicksRemaining == 5);

    const auto earlyTick = preparationTick + 1;
    const WaveLoopCommand early{
        earlyTick, 1, 2, WaveLoopCommandType::StartNextWave, 77};
    check(original.submit(early));
    check(restored.submit(early));

    bool skipped = false;
    bool bossSpawned = false;
    bool completed = false;
    for (std::uint64_t tick = earlyTick; tick < 240; ++tick) {
        check(original.step(tick));
        check(restored.step(tick));
        check(original.snapshot().worldHash == restored.snapshot().worldHash);
        check(original.snapshot().towerDefenseWorldHash ==
              restored.snapshot().towerDefenseWorldHash);
        skipped = skipped ||
            hasEvent(original, WaveLoopEventType::PreparationSkipped);
        bossSpawned = bossSpawned || std::any_of(
            original.tower().events().begin(),
            original.tower().events().end(),
            [](const Event& event) {
                return event.type == EventType::BossSpawned;
            });
        if (original.snapshot().sequence.phase ==
            WaveSequencePhase::Complete) {
            check(restored.snapshot().sequence.phase ==
                  WaveSequencePhase::Complete);
            completed = true;
            break;
        }
    }
    check(skipped);
    check(bossSpawned);
    check(completed);
    check(original.snapshot().sequence.completedWaves == 2);
    check(original.snapshot().sequence.currentWave == 0);
    check(EncodeWaveLoopSimulation(original) ==
          EncodeWaveLoopSimulation(restored));

    WaveLoopSimulation incompatible(12, 5, 0x5151u);
    configureTwoWaveLoop(incompatible, false, 6);
    const auto before = EncodeWaveLoopSimulation(incompatible);
    check(!DecodeWaveLoopSimulation(bytes, incompatible));
    check(EncodeWaveLoopSimulation(incompatible) == before);
}

void testEarlyStartCanBeDisabled() {
    WaveLoopSimulation simulation(12, 5, 0x2222u);
    configureTwoWaveLoop(simulation, true, 5, false);
    check(simulation.submit(
        {0, 1, 1, WaveLoopCommandType::StartSequence, 77}));
    check(simulation.step(0));
    const auto scheduled = simulation.snapshot().sequence.scheduledStartTick;

    check(simulation.submit(
        {1, 1, 2, WaveLoopCommandType::StartNextWave, 77}));
    check(simulation.step(1));
    check(simulation.snapshot().sequence.phase ==
          WaveSequencePhase::Preparing);
    check(simulation.snapshot().sequence.scheduledStartTick == scheduled);
    check(hasEvent(
        simulation,
        WaveLoopEventType::SequenceRejected,
        static_cast<std::uint32_t>(
            WaveSequenceFailure::EarlyStartDisabled)));
}

void testBaseCoreFailureEndsSequence() {
    WaveLoopSimulation simulation(8, 5, 0x3333u);

    gameplay::UnitDefinition enemy;
    enemy.id = 1;
    enemy.cellsPerTick = 1;
    enemy.combat = {100, 0, 20, 1, 1, 0};
    simulation.registerUnit(enemy);
    check(simulation.registerLane({1, {0, 2}, {6, 2}, 1}));

    WaveDefinition wave;
    wave.id = 1;
    wave.budget = 1;
    wave.laneIds = {1};
    wave.enemies = {{1, 1, 1, 1}};
    wave.rewardChoices = 0;
    check(simulation.registerWave(wave));
    check(simulation.registerSequence({1, {1}, 0, 0, true}));
    simulation.createBaseCore(
        {6, 2}, 1, {5, 0, 0, 0, 1, 0});

    check(simulation.submit(
        {0, 1, 1, WaveLoopCommandType::StartSequence, 1}));
    bool failed = false;
    for (std::uint64_t tick = 0; tick < 80; ++tick) {
        check(simulation.step(tick));
        if (simulation.snapshot().sequence.phase ==
            WaveSequencePhase::Failed) {
            failed = true;
            check(simulation.sequence().lastFailure() ==
                  WaveSequenceFailure::BaseCoreDestroyed);
            check(hasEvent(
                simulation, WaveLoopEventType::SequenceFailed,
                static_cast<std::uint32_t>(
                    WaveSequenceFailure::BaseCoreDestroyed)));
            break;
        }
    }
    check(failed);
}

} // namespace

int main() {
    testAutomaticFirstWaveAndEarlySecondWaveRoundTrip();
    testEarlyStartCanBeDisabled();
    testBaseCoreFailureEndsSequence();
    std::cout << "wave loop tests passed\n";
    return 0;
}
