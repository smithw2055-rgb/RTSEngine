#include <RTSEngine/Roguelite/RunSaveSchema.h>
#include <RTSEngine/TowerDefense/SimulationArchive.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace rts;

void check(bool condition) {
    assert(condition);
    if (!condition) std::abort();
}

void configureTower(
    tower_defense::TowerDefenseSimulation& simulation,
    bool createActors) {
    simulation.setResources(50);
    simulation.setPlayerTeam(1);

    gameplay::UnitDefinition enemy;
    enemy.id = 1;
    enemy.cellsPerTick = 1;
    enemy.combat = {15, 0, 1, 1, 1, 0};
    simulation.registerUnit(enemy);

    check(simulation.upsertLaneNode({1, {0, 3}}));
    check(simulation.upsertLaneNode({2, {7, 2}}));
    check(simulation.upsertLaneNode({3, {14, 3}}));
    check(simulation.connectLaneNodes(1, 2, 2));
    check(simulation.connectLaneNodes(2, 3, 2));
    tower_defense::SpawnLane lane;
    lane.id = 1;
    lane.weight = 1;
    lane.startNodeId = 1;
    lane.goalNodeId = 3;
    check(simulation.registerLane(lane));
    check(simulation.registerReward({101, 1, 25}));

    tower_defense::WaveDefinition wave;
    wave.id = 1;
    wave.budget = 3;
    wave.spawnIntervalTicks = 4;
    wave.enemyTeamId = 2;
    wave.laneIds = {1};
    wave.enemies = {{1, 1, 1, 3}};
    wave.rewardPool = {101};
    wave.rewardChoices = 1;
    check(simulation.registerWave(wave));

    if (createActors) {
        simulation.createBaseCore(
            {14, 3}, 1, {100, 0, 0, 0, 1, 0});
        simulation.createDefender(
            {8, 3}, {0}, 1, {50, 0, 10, 20, 1, 0});
    }
}

void testTowerDefenseRoundTrip() {
    tower_defense::TowerDefenseSimulation original(16, 8, 0xabcdu);
    configureTower(original, true);

    tower_defense::TickCommand start;
    start.targetTick = 0;
    start.issuer = 1;
    start.sequence = 1;
    start.type = tower_defense::CommandType::StartWave;
    start.objectId = 1;
    check(original.submit(start));

    tower_defense::TickCommand choose;
    choose.targetTick = 50;
    choose.issuer = 1;
    choose.sequence = 2;
    choose.type = tower_defense::CommandType::ChooseReward;
    choose.objectId = 101;
    check(original.submit(choose));

    for (std::uint64_t tick = 0; tick <= 5; ++tick) {
        check(original.step(tick));
    }
    check(original.snapshot().wave.phase ==
          tower_defense::WavePhase::Spawning);
    check(original.snapshot().wave.spawned == 2);
    check(original.snapshot().wave.resolved == 2);
    check(original.director().plan().routes.size() == 1);
    check(original.director().plan().routes.front().nodeIds ==
          std::vector<tower_defense::LaneNodeId>({1, 2, 3}));
    const auto frozenRoutes = original.director().plan().routes;

    const auto bytes =
        tower_defense::EncodeTowerDefenseSimulation(original);
    check(!bytes.empty());

    tower_defense::TowerDefenseSimulation restored(16, 8, 0xabcdu);
    configureTower(restored, false);
    check(tower_defense::DecodeTowerDefenseSimulation(bytes, restored));
    check(restored.snapshot().worldHash == original.snapshot().worldHash);
    check(restored.commandStreamState().pending.size() == 1);
    check(restored.director().plan().routes == frozenRoutes);

    for (std::uint64_t tick = 6; tick <= 52; ++tick) {
        check(original.step(tick));
        check(restored.step(tick));
        check(original.snapshot().worldHash == restored.snapshot().worldHash);
        check(original.snapshot().rtsWorldHash ==
              restored.snapshot().rtsWorldHash);
    }
    check(original.snapshot().wave.phase ==
          tower_defense::WavePhase::Complete);
    check(tower_defense::EncodeTowerDefenseSimulation(original) ==
          tower_defense::EncodeTowerDefenseSimulation(restored));

    tower_defense::TowerDefenseSimulation wrongSeed(16, 8, 0xabceu);
    configureTower(wrongSeed, false);
    const auto beforeWrongSeed =
        tower_defense::EncodeTowerDefenseSimulation(wrongSeed);
    check(!tower_defense::DecodeTowerDefenseSimulation(bytes, wrongSeed));
    check(tower_defense::EncodeTowerDefenseSimulation(wrongSeed) ==
          beforeWrongSeed);

    auto truncated = bytes;
    truncated.pop_back();
    tower_defense::TowerDefenseSimulation preserved(16, 8, 0xabcdu);
    configureTower(preserved, false);
    const auto beforeTruncated =
        tower_defense::EncodeTowerDefenseSimulation(preserved);
    check(!tower_defense::DecodeTowerDefenseSimulation(
        truncated, preserved));
    check(tower_defense::EncodeTowerDefenseSimulation(preserved) ==
          beforeTruncated);
}

void configureRun(
    roguelite::RunSimulation& simulation,
    bool createActors,
    std::int32_t modifierBonus = 5) {
    simulation.setResources(100);
    simulation.setPlayerTeam(1);

    gameplay::UnitDefinition enemy;
    enemy.id = 1;
    enemy.cellsPerTick = 1;
    enemy.combat = {15, 0, 1, 1, 1, 0};
    simulation.registerUnit(enemy);

    check(simulation.registerLane({1, {0, 3}, {14, 3}, 1}));

    roguelite::ModifierDefinition modifier;
    modifier.id = 1;
    modifier.weight = 1;
    modifier.maxStacks = 2;
    modifier.effects = {
        {roguelite::WaveCompletionResourceStat(),
         roguelite::ModifierOperation::Add,
         modifierBonus},
        {roguelite::UnitDamageStat(),
         roguelite::ModifierOperation::Multiply,
         1250}
    };
    check(simulation.registerModifier(modifier));

    tower_defense::WaveDefinition first;
    first.id = 1;
    first.budget = 3;
    first.spawnIntervalTicks = 4;
    first.enemyTeamId = 2;
    first.laneIds = {1};
    first.enemies = {{1, 1, 1, 3}};
    first.rewardPool = {1};
    first.rewardChoices = 1;
    check(simulation.registerWave(first));

    tower_defense::WaveDefinition second = first;
    second.id = 2;
    second.budget = 2;
    second.enemies.front().maxPerWave = 2;
    check(simulation.registerWave(second));
    check(simulation.registerRun({1, {1, 2}}));

    if (createActors) {
        simulation.createBaseCore(
            {14, 3}, 1, {100, 0, 0, 0, 1, 0});
        simulation.createDefender(
            {8, 3}, {0}, 1, {50, 0, 10, 20, 1, 0});
    }
}

void testRunSaveMidWaveContinuation() {
    roguelite::RunSimulation original(16, 8, 0x123456u);
    configureRun(original, true);

    check(original.submit(
        {0, 1, 1, roguelite::CommandType::StartRun, 1}));
    check(original.submit(
        {40, 1, 2, roguelite::CommandType::ChooseModifier, 999}));

    gameplay::TickCommand move;
    move.targetTick = 12;
    move.issuer = 1;
    move.sequence = 1;
    move.type = gameplay::CommandType::Move;
    move.subject = {2, 1};
    move.targetX = 10;
    move.targetY = 3;
    check(original.submitRts(move));

    check(original.step(0));
    check(original.step(1));
    check(original.state().phase == roguelite::RunPhase::WaveActive);

    auto save = roguelite::CaptureRunSave(
        original,
        {},
        {{1, original.snapshot().worldHash}});
    check(!save.authoritativeState.empty());
    const auto saveBytes = roguelite::EncodeRunSave(save);
    check(!saveBytes.empty());

    roguelite::RunSaveSchema decoded;
    check(roguelite::DecodeRunSave(saveBytes, decoded));
    check(decoded.authoritativeState == save.authoritativeState);
    check(decoded.runCommands.pending.size() == 1);
    check(decoded.rtsCommands.pending.size() == 1);

    roguelite::RunSimulation restored(16, 8, 0x123456u);
    configureRun(restored, false);
    check(roguelite::RestoreRunSave(decoded, restored));
    check(restored.snapshot().worldHash == original.snapshot().worldHash);

    std::uint32_t externalSequence = 100;
    std::uint32_t choicesQueued = 0;
    bool completed = false;
    for (std::uint64_t tick = 2; tick < 180; ++tick) {
        check(original.step(tick));
        check(restored.step(tick));
        check(original.snapshot().worldHash == restored.snapshot().worldHash);
        check(original.snapshot().towerDefenseWorldHash ==
              restored.snapshot().towerDefenseWorldHash);
        check(original.tower().snapshot().rtsWorldHash ==
              restored.tower().snapshot().rtsWorldHash);

        if (original.state().phase == roguelite::RunPhase::RewardPending &&
            choicesQueued < original.state().completedWaves + 1u) {
            const auto modifierId =
                original.tower().director().offer().choices.front();
            roguelite::TickCommand chooseModifier;
            chooseModifier.targetTick = tick + 1;
            chooseModifier.issuer = 1;
            chooseModifier.sequence = externalSequence++;
            chooseModifier.type = roguelite::CommandType::ChooseModifier;
            chooseModifier.objectId = modifierId;
            check(original.submit(chooseModifier));
            check(restored.submit(chooseModifier));
            ++choicesQueued;
        }

        if (original.state().phase == roguelite::RunPhase::Complete) {
            check(restored.state().phase == roguelite::RunPhase::Complete);
            completed = true;
            break;
        }
    }
    check(completed);
    const auto originalFinal = roguelite::CaptureRunSave(original);
    const auto restoredFinal = roguelite::CaptureRunSave(restored);
    check(originalFinal.authoritativeState ==
          restoredFinal.authoritativeState);

    roguelite::RunSimulation incompatible(16, 8, 0x123456u);
    configureRun(incompatible, false, 6);
    const auto incompatibleBefore =
        roguelite::EncodeRunSimulation(incompatible);
    check(!roguelite::RestoreRunSave(decoded, incompatible));
    check(roguelite::EncodeRunSimulation(incompatible) ==
          incompatibleBefore);

    auto corrupted = decoded;
    corrupted.authoritativeState.pop_back();
    roguelite::RunSimulation preserved(16, 8, 0x123456u);
    configureRun(preserved, false);
    const auto preservedBefore = roguelite::EncodeRunSimulation(preserved);
    check(!roguelite::RestoreRunSave(corrupted, preserved));
    check(roguelite::EncodeRunSimulation(preserved) == preservedBefore);
}

} // namespace

int main() {
    testTowerDefenseRoundTrip();
    testRunSaveMidWaveContinuation();
    std::cout << "roguelite persistence tests passed\n";
    return 0;
}
