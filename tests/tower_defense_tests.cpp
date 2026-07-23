#include <RTSEngine/TowerDefense/Simulation.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace rts;
using namespace rts::tower_defense;

bool hasEvent(const TowerDefenseSimulation& simulation,
              EventType type) {
    return std::any_of(
        simulation.events().begin(),
        simulation.events().end(),
        [type](const Event& event) {
            return event.type == type;
        });
}

bool hasRejection(
    const TowerDefenseSimulation& simulation,
    WaveStartFailure failure) {
    return std::any_of(
        simulation.events().begin(),
        simulation.events().end(),
        [failure](const Event& event) {
            return event.type == EventType::WaveRejected &&
                   event.reason == static_cast<std::uint32_t>(failure);
        });
}

std::uint32_t countEvents(
    const TowerDefenseSimulation& simulation,
    EventType type) {
    return static_cast<std::uint32_t>(
        std::count_if(
            simulation.events().begin(),
            simulation.events().end(),
            [type](const Event& event) {
                return event.type == type;
            }));
}

std::int32_t rewardGrant(RewardId id) {
    switch (id) {
    case 101: return 20;
    case 102: return 30;
    case 103: return 40;
    case 104: return 50;
    default: return 0;
    }
}

struct ScenarioResult {
    std::vector<std::uint64_t> hashes;
    std::vector<PlannedSpawn> plan;
    std::vector<RewardId> offer;
    RewardId selected{};
    std::int32_t resourcesBeforeReward{};
    std::int32_t resourcesAfterReward{};
    std::int32_t coreHealth{};
    std::uint32_t waveStartedEvents{};
    WavePhase phase{WavePhase::Idle};
};

ScenarioResult runSuccessfulWave() {
    TowerDefenseSimulation simulation(16, 8, 0xA11CEu);
    simulation.setResources(10);
    simulation.setPlayerTeam(1);

    gameplay::UnitDefinition runner;
    runner.id = 10;
    runner.cellsPerTick = 1;
    runner.combat = {8, 0, 2, 1, 1, 2};
    simulation.registerUnit(runner);

    gameplay::UnitDefinition brute;
    brute.id = 11;
    brute.cellsPerTick = 1;
    brute.combat = {14, 1, 3, 1, 2, 4};
    simulation.registerUnit(brute);

    assert(simulation.registerLane(
        {1, {0, 2}, {14, 3}, 3}));
    assert(simulation.registerLane(
        {2, {0, 4}, {14, 3}, 1}));

    assert(simulation.registerReward({101, 4, 20}));
    assert(simulation.registerReward({102, 3, 30}));
    assert(simulation.registerReward({103, 2, 40}));
    assert(simulation.registerReward({104, 1, 50}));

    WaveDefinition wave;
    wave.id = 1;
    wave.budget = 10;
    wave.spawnIntervalTicks = 2;
    wave.enemyTeamId = 2;
    wave.laneIds = {1, 2};
    wave.enemies = {
        {10, 2, 4, 0},
        {11, 4, 1, 1}
    };
    wave.rewardPool = {101, 102, 103, 104};
    wave.rewardChoices = 3;
    assert(simulation.registerWave(wave));

    simulation.createBaseCore(
        {14, 3}, 1, {60, 1, 0, 0, 1, 0});
    simulation.createDefender(
        {9, 3}, {0}, 1, {100, 0, 20, 6, 1, 0});

    TickCommand start;
    start.targetTick = 0;
    start.issuer = 1;
    start.sequence = 1;
    start.type = CommandType::StartWave;
    start.objectId = 1;
    assert(simulation.submit(start));
    assert(simulation.submit(start));

    ScenarioResult result;
    bool rewardQueued = false;

    for (std::uint64_t tick = 0; tick < 120; ++tick) {
        assert(simulation.step(tick));
        result.hashes.push_back(
            simulation.snapshot().worldHash);
        result.waveStartedEvents +=
            countEvents(simulation, EventType::WaveStarted);

        if (tick == 0) {
            result.plan = simulation.director().plan().spawns;
            assert(!result.plan.empty());
        }

        if (!rewardQueued &&
            simulation.snapshot().wave.phase ==
                WavePhase::RewardPending) {
            result.offer =
                simulation.snapshot().rewardChoices;
            assert(result.offer.size() == 3);
            auto unique = result.offer;
            std::sort(unique.begin(), unique.end());
            assert(std::adjacent_find(
                       unique.begin(), unique.end()) ==
                   unique.end());

            result.selected = result.offer.front();
            result.resourcesBeforeReward =
                simulation.resources().available;

            TickCommand choose;
            choose.targetTick = tick + 1;
            choose.issuer = 1;
            choose.sequence = 2;
            choose.type = CommandType::ChooseReward;
            choose.objectId = result.selected;
            assert(simulation.submit(choose));
            rewardQueued = true;
        }

        if (rewardQueued &&
            simulation.snapshot().wave.phase ==
                WavePhase::Complete) {
            result.resourcesAfterReward =
                simulation.resources().available;
            result.coreHealth =
                simulation.snapshot().coreHealthCurrent;
            result.phase =
                simulation.snapshot().wave.phase;
            break;
        }
    }

    assert(result.phase == WavePhase::Complete);
    assert(result.waveStartedEvents == 1);
    assert(result.resourcesAfterReward ==
           result.resourcesBeforeReward +
               rewardGrant(result.selected));
    assert(result.coreHealth > 0);
    assert(simulation.snapshot().activeEnemies == 0);
    assert(simulation.snapshot().wave.spawned ==
           simulation.snapshot().wave.resolved);
    return result;
}

void testDeterministicSuccessfulWave() {
    const auto first = runSuccessfulWave();
    const auto second = runSuccessfulWave();

    assert(first.hashes == second.hashes);
    assert(first.plan == second.plan);
    assert(first.offer == second.offer);
    assert(first.selected == second.selected);
    assert(first.resourcesBeforeReward ==
           second.resourcesBeforeReward);
    assert(first.resourcesAfterReward ==
           second.resourcesAfterReward);
    assert(first.coreHealth == second.coreHealth);
}

void testBaseCoreFailure() {
    TowerDefenseSimulation simulation(10, 5, 77);

    gameplay::UnitDefinition raider;
    raider.id = 20;
    raider.cellsPerTick = 1;
    raider.combat = {20, 0, 10, 1, 1, 0};
    simulation.registerUnit(raider);

    assert(simulation.registerLane(
        {1, {0, 2}, {8, 2}, 1}));

    WaveDefinition wave;
    wave.id = 2;
    wave.budget = 1;
    wave.spawnIntervalTicks = 1;
    wave.enemyTeamId = 2;
    wave.laneIds = {1};
    wave.enemies = {{20, 1, 1, 1}};
    wave.rewardChoices = 0;
    assert(simulation.registerWave(wave));

    simulation.createBaseCore(
        {8, 2}, 1, {5, 0, 0, 0, 1, 0});

    TickCommand start;
    start.targetTick = 0;
    start.issuer = 1;
    start.sequence = 1;
    start.type = CommandType::StartWave;
    start.objectId = 2;
    assert(simulation.submit(start));

    bool coreDestroyed = false;
    for (std::uint64_t tick = 0; tick < 40; ++tick) {
        assert(simulation.step(tick));
        coreDestroyed = coreDestroyed ||
            hasEvent(simulation, EventType::BaseCoreDestroyed);
        if (simulation.snapshot().wave.phase ==
            WavePhase::Failed) {
            break;
        }
    }

    assert(coreDestroyed);
    assert(simulation.snapshot().wave.phase ==
           WavePhase::Failed);
}

void testBlockedLaneRejectsWave() {
    TowerDefenseSimulation simulation(8, 5, 99);

    gameplay::UnitDefinition enemy;
    enemy.id = 30;
    enemy.cellsPerTick = 1;
    enemy.combat = {10, 0, 2, 1, 1, 0};
    simulation.registerUnit(enemy);

    assert(simulation.registerLane(
        {1, {0, 2}, {6, 2}, 1}));

    WaveDefinition wave;
    wave.id = 3;
    wave.budget = 1;
    wave.laneIds = {1};
    wave.enemies = {{30, 1, 1, 1}};
    wave.rewardChoices = 0;
    assert(simulation.registerWave(wave));

    simulation.createBaseCore(
        {6, 2}, 1, {20, 0, 0, 0, 1, 0});
    assert(simulation.setBlocked({0, 2}, true));

    TickCommand start;
    start.targetTick = 0;
    start.issuer = 1;
    start.sequence = 1;
    start.type = CommandType::StartWave;
    start.objectId = 3;
    assert(simulation.submit(start));
    assert(simulation.step(0));

    assert(hasEvent(simulation, EventType::WaveRejected));
    assert(simulation.snapshot().wave.phase ==
           WavePhase::Idle);
}

void testGraphLaneRouteIsFrozenAtWaveStart() {
    TowerDefenseSimulation simulation(12, 7, 0x51cedu);

    gameplay::UnitDefinition enemy;
    enemy.id = 40;
    enemy.cellsPerTick = 1;
    enemy.combat = {10, 0, 2, 1, 1, 0};
    simulation.registerUnit(enemy);

    assert(simulation.upsertLaneNode({10, {0, 2}}));
    assert(simulation.upsertLaneNode({20, {4, 2}}));
    assert(simulation.upsertLaneNode({30, {4, 4}}));
    assert(simulation.upsertLaneNode({40, {10, 3}}));
    assert(simulation.connectLaneNodes(10, 20, 1));
    assert(simulation.connectLaneNodes(20, 40, 1));
    assert(simulation.connectLaneNodes(10, 30, 3));
    assert(simulation.connectLaneNodes(30, 40, 3));

    SpawnLane lane;
    lane.id = 4;
    lane.weight = 1;
    lane.startNodeId = 10;
    lane.goalNodeId = 40;
    assert(simulation.registerLane(lane));

    WaveDefinition wave;
    wave.id = 4;
    wave.budget = 1;
    wave.laneIds = {4};
    wave.enemies = {{40, 1, 1, 1}};
    wave.rewardChoices = 0;
    assert(simulation.registerWave(wave));
    simulation.createBaseCore(
        {10, 3}, 1, {50, 0, 0, 0, 1, 0});

    assert(simulation.submit(
        {0, 1, 1, CommandType::StartWave, 4}));
    assert(simulation.step(0));
    assert(simulation.director().plan().routes.size() == 1);

    const auto frozen = simulation.director().plan().routes.front();
    const std::vector<gameplay::GridPoint> expected = {
        {0, 2}, {4, 2}, {10, 3}
    };
    assert(frozen.id == 4);
    assert(frozen.nodeIds == std::vector<LaneNodeId>({10, 20, 40}));
    assert(frozen.points == expected);

    assert(simulation.setLaneConnectionEnabled(20, 40, false));
    const auto live = simulation.director().laneGraph().findRoute(10, 40);
    assert(live.found);
    assert(live.nodeIds == std::vector<LaneNodeId>({10, 30, 40}));
    assert(simulation.director().plan().routes.front() == frozen);
}

void testDisconnectedGraphLaneRejectsWave() {
    TowerDefenseSimulation simulation(8, 5, 0xdeadbeefu);

    gameplay::UnitDefinition enemy;
    enemy.id = 50;
    enemy.cellsPerTick = 1;
    enemy.combat = {10, 0, 2, 1, 1, 0};
    simulation.registerUnit(enemy);

    assert(simulation.upsertLaneNode({1, {0, 2}}));
    assert(simulation.upsertLaneNode({2, {6, 2}}));
    SpawnLane lane;
    lane.id = 5;
    lane.startNodeId = 1;
    lane.goalNodeId = 2;
    assert(simulation.registerLane(lane));

    WaveDefinition wave;
    wave.id = 5;
    wave.budget = 1;
    wave.laneIds = {5};
    wave.enemies = {{50, 1, 1, 1}};
    wave.rewardChoices = 0;
    assert(simulation.registerWave(wave));
    simulation.createBaseCore(
        {6, 2}, 1, {20, 0, 0, 0, 1, 0});

    assert(simulation.submit(
        {0, 1, 1, CommandType::StartWave, 5}));
    assert(simulation.step(0));
    assert(hasRejection(simulation, WaveStartFailure::InvalidLaneRoute));
    assert(simulation.snapshot().wave.phase == WavePhase::Idle);
}

} // namespace

int main() {
    testDeterministicSuccessfulWave();
    testBaseCoreFailure();
    testBlockedLaneRejectsWave();
    testGraphLaneRouteIsFrozenAtWaveStart();
    testDisconnectedGraphLaneRejectsWave();
    std::cout << "tower defense tests passed\n";
    return 0;
}
