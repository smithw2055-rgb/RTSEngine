#include <RTSEngine/Roguelite/RunSimulationArchive.h>
#include <rts/foundation/BinaryArchive.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

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

    if (createActors) {
        simulation.createBaseCore(
            {10, 2}, 1, {500, 0, 0, 0, 1, 0});
        simulation.createDefender(
            {7, 2}, {0}, 1, {500, 0, 50, 6, 1, 0});
    }
}

std::uint64_t advanceToSecondPreparation(
    roguelite::RunSimulation& simulation) {
    check(simulation.submit(
        {0, 1, 1, roguelite::CommandType::StartRun, 7}));
    for (std::uint64_t tick = 0; tick < 80; ++tick) {
        check(simulation.step(tick));
        if (simulation.state().phase == roguelite::RunPhase::BetweenWaves &&
            simulation.state().waveIndex == 1) {
            return tick;
        }
    }
    std::abort();
}

void checkProjection(
    const roguelite::RunSimulation& simulation,
    std::uint64_t transitionTick) {
    const auto* definition = simulation.waveSequence().definition(7);
    check(definition != nullptr);
    check(definition->waves ==
          std::vector<tower_defense::WaveId>({1, 2}));
    check(definition->initialPreparationTicks == 0);
    check(definition->interWavePreparationTicks == 1);
    check(!definition->allowEarlyStart);

    check(simulation.state().phase == roguelite::RunPhase::BetweenWaves);
    check(simulation.state().waveIndex == 1);
    check(simulation.state().completedWaves == 1);
    check(simulation.state().currentWave == 2);
    check(simulation.waveSequence().state().phase ==
          tower_defense::WaveSequencePhase::Preparing);
    check(simulation.waveSequence().state().scheduledStartTick ==
          transitionTick + 1u);
}

void testProjection() {
    roguelite::RunSimulation simulation(12, 5, 0x7171u);
    configure(simulation, true);
    const auto transitionTick = advanceToSecondPreparation(simulation);
    checkProjection(simulation, transitionTick);
}

struct RestoredPair {
    roguelite::RunSimulation original{12, 5, 0x7171u};
    roguelite::RunSimulation restored{12, 5, 0x7171u};
    std::uint64_t transitionTick{};
};

void prepareRoundTrip(RestoredPair& pair) {
    configure(pair.original, true);
    pair.transitionTick = advanceToSecondPreparation(pair.original);
    checkProjection(pair.original, pair.transitionTick);

    const auto bytes = roguelite::EncodeRunSimulation(pair.original);
    check(!bytes.empty());
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    check(reader.readU32(magic));
    check(reader.readU16(version));
    check(magic == roguelite::RunSimulationArchive::kMagic);
    check(version == 1u);
    check(roguelite::RunSimulationArchive::kVersion == 1u);

    configure(pair.restored, false);
    check(roguelite::DecodeRunSimulation(bytes, pair.restored));
    check(pair.restored.snapshot().worldHash ==
          pair.original.snapshot().worldHash);
    checkProjection(pair.restored, pair.transitionTick);
}

void testArchiveProjection() {
    RestoredPair pair;
    prepareRoundTrip(pair);
}

void testContinuation() {
    RestoredPair pair;
    prepareRoundTrip(pair);

    const auto nextTick = pair.transitionTick + 1u;
    check(pair.original.step(nextTick));
    check(pair.restored.step(nextTick));
    check(pair.original.state().phase == roguelite::RunPhase::WaveActive);
    check(pair.restored.state().phase == roguelite::RunPhase::WaveActive);
    check(pair.original.waveSequence().state().phase ==
          tower_defense::WaveSequencePhase::WaveActive);
    check(pair.restored.waveSequence().state().phase ==
          tower_defense::WaveSequencePhase::WaveActive);
    check(pair.original.snapshot().worldHash ==
          pair.restored.snapshot().worldHash);

    bool completed = false;
    for (std::uint64_t tick = nextTick + 1u; tick < 160; ++tick) {
        check(pair.original.step(tick));
        check(pair.restored.step(tick));
        check(pair.original.snapshot().worldHash ==
              pair.restored.snapshot().worldHash);
        if (pair.original.state().phase == roguelite::RunPhase::Complete) {
            check(pair.restored.state().phase == roguelite::RunPhase::Complete);
            check(pair.original.waveSequence().state().phase ==
                  tower_defense::WaveSequencePhase::Complete);
            completed = true;
            break;
        }
    }
    check(completed);
    check(roguelite::EncodeRunSimulation(pair.original) ==
          roguelite::EncodeRunSimulation(pair.restored));
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "all";
    if (mode == "projection" || mode == "all") testProjection();
    if (mode == "archive" || mode == "all") testArchiveProjection();
    if (mode == "continuation" || mode == "all") testContinuation();
    std::cout << "roguelite sequence " << mode << " tests passed\n";
    return 0;
}
