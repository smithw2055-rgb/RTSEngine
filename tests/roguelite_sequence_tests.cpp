#include <RTSEngine/Roguelite/RunSimulationArchive.h>
#include <rts/foundation/BinaryArchive.h>

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

void configure(
    roguelite::RunSimulation& simulation,
    bool createActors) {
    gameplay::UnitDefinition enemy;
    enemy.id = 1;
    enemy.cellsPerTick = 1;
    enemy.combat = {20, 0, 1, 1, 1, 0};
    simulation.registerUnit(enemy);

    check(simulation.registerLane({1, {0, 2}, {10, 2}, 1}));

    tower_defense::WaveDefinition first;
    first.id = 1;
    first.budget = 1;
    first.spawnIntervalTicks = 1;
    first.laneIds = {1};
    first.enemies = {{1, 1, 1, 1}};
    first.rewardChoices = 0;
    check(simulation.registerWave(first));

    auto second = first;
    second.id = 2;
    check(simulation.registerWave(second));
    check(simulation.registerRun({7, {1, 2}}));

    const auto* sequence = simulation.waveSequence().definition(7);
    check(sequence != nullptr);
    check(sequence->waves == std::vector<tower_defense::WaveId>({1, 2}));
    check(sequence->initialPreparationTicks == 0);
    check(sequence->interWavePreparationTicks == 1);
    check(!sequence->allowEarlyStart);

    if (createActors) {
        simulation.createBaseCore(
            {10, 2}, 1, {500, 0, 0, 0, 1, 0});
        simulation.createDefender(
            {7, 2}, {0}, 1, {500, 0, 50, 6, 1, 0});
    }
}

void testBetweenWaveV1ProjectionRoundTrip() {
    roguelite::RunSimulation original(12, 5, 0x7171u);
    configure(original, true);
    check(original.submit(
        {0, 1, 1, roguelite::CommandType::StartRun, 7}));

    std::uint64_t transitionTick = 0;
    bool reachedPreparation = false;
    for (std::uint64_t tick = 0; tick < 80; ++tick) {
        check(original.step(tick));
        if (original.state().phase == roguelite::RunPhase::BetweenWaves &&
            original.state().waveIndex == 1) {
            transitionTick = tick;
            reachedPreparation = true;
            break;
        }
    }
    check(reachedPreparation);
    check(original.state().completedWaves == 1);
    check(original.state().currentWave == 2);
    check(original.waveSequence().state().phase ==
          tower_defense::WaveSequencePhase::Preparing);
    check(original.waveSequence().state().scheduledStartTick ==
          transitionTick + 1u);

    const auto bytes = roguelite::EncodeRunSimulation(original);
    check(!bytes.empty());
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    check(reader.readU32(magic));
    check(reader.readU16(version));
    check(magic == roguelite::RunSimulationArchive::kMagic);
    check(version == 1u);
    check(roguelite::RunSimulationArchive::kVersion == 1u);

    roguelite::RunSimulation restored(12, 5, 0x7171u);
    configure(restored, false);
    check(roguelite::DecodeRunSimulation(bytes, restored));
    check(restored.snapshot().worldHash == original.snapshot().worldHash);
    check(restored.state().phase == roguelite::RunPhase::BetweenWaves);
    check(restored.state().waveIndex == 1);
    check(restored.waveSequence().state().phase ==
          tower_defense::WaveSequencePhase::Preparing);
    check(restored.waveSequence().state().scheduledStartTick ==
          transitionTick + 1u);

    const auto nextTick = transitionTick + 1u;
    check(original.step(nextTick));
    check(restored.step(nextTick));
    check(original.state().phase == roguelite::RunPhase::WaveActive);
    check(restored.state().phase == roguelite::RunPhase::WaveActive);
    check(original.waveSequence().state().phase ==
          tower_defense::WaveSequencePhase::WaveActive);
    check(restored.waveSequence().state().phase ==
          tower_defense::WaveSequencePhase::WaveActive);
    check(original.snapshot().worldHash == restored.snapshot().worldHash);

    bool completed = false;
    for (std::uint64_t tick = nextTick + 1u; tick < 160; ++tick) {
        check(original.step(tick));
        check(restored.step(tick));
        check(original.snapshot().worldHash == restored.snapshot().worldHash);
        if (original.state().phase == roguelite::RunPhase::Complete) {
            check(restored.state().phase == roguelite::RunPhase::Complete);
            check(original.waveSequence().state().phase ==
                  tower_defense::WaveSequencePhase::Complete);
            completed = true;
            break;
        }
    }
    check(completed);
    check(roguelite::EncodeRunSimulation(original) ==
          roguelite::EncodeRunSimulation(restored));
}

} // namespace

int main() {
    testBetweenWaveV1ProjectionRoundTrip();
    std::cout << "roguelite sequence tests passed\n";
    return 0;
}
