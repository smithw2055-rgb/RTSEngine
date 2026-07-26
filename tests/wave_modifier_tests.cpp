#include <RTSEngine/TowerDefense/SimulationArchive.h>

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

WaveDirector buildDirector(bool reverseRegistration) {
    WaveDirector director(0x51cedu);
    check(director.registerLane({1, {0, 2}, {10, 2}, 1}));

    std::vector<WaveAffixDefinition> affixes = {
        {101, 4, {1500, 1, 1000, 1000, 1000}},
        {102, 2, {1000, 2, 1250, 1000, 1000}},
        {103, 1, {1000, 0, 1000, 750, 1500}}
    };
    std::vector<BossDefinition> bosses = {
        {201, 2, 4, 3, {2000, 2, 1500, 800, 2000}},
        {202, 3, 5, 1, {2500, 4, 1200, 700, 2500}}
    };
    if (reverseRegistration) {
        std::reverse(affixes.begin(), affixes.end());
        std::reverse(bosses.begin(), bosses.end());
    }
    for (const auto& value : affixes) check(director.registerAffix(value));
    for (const auto& value : bosses) check(director.registerBoss(value));

    WaveDefinition wave;
    wave.id = 7;
    wave.budget = 10;
    wave.spawnIntervalTicks = 2;
    wave.laneIds = {1};
    wave.enemies = {{1, 2, 1, 0}};
    wave.bossPool = {201, 202};
    wave.bossCount = 1;
    wave.affixPool = {101, 102, 103};
    wave.affixChoices = 2;
    wave.rewardChoices = 0;
    check(director.registerWave(wave));
    return director;
}

void testRegistrationOrderIndependentPlan() {
    auto first = buildDirector(false);
    auto second = buildDirector(true);
    check(first.begin(7, 10).accepted);
    check(second.begin(7, 10).accepted);

    check(first.plan().affixes == second.plan().affixes);
    check(first.plan().spawns == second.plan().spawns);
    check(first.plan().unusedBudget == second.plan().unusedBudget);
    check(first.plan().affixes.size() == 2);
    check(!first.plan().spawns.empty());
    check(first.plan().spawns.front().bossId != 0);

    const auto bossCount = std::count_if(
        first.plan().spawns.begin(), first.plan().spawns.end(),
        [](const PlannedSpawn& spawn) { return spawn.bossId != 0; });
    check(bossCount == 1);
}

void configureModifiedBoss(
    TowerDefenseSimulation& simulation,
    bool createCore) {
    gameplay::UnitDefinition bossUnit;
    bossUnit.id = 1;
    bossUnit.cellsPerTick = 2;
    bossUnit.combat = {100, 2, 10, 1, 1, 5};
    simulation.registerUnit(bossUnit);

    check(simulation.registerAffix(
        {101, 1, {1500, 3, 1200, 500, 2000}}));
    check(simulation.registerBoss(
        {201, 1, 1, 1, {2000, 2, 1500, 1000, 1500}}));
    check(simulation.registerLane({1, {0, 2}, {10, 2}, 1}));

    WaveDefinition wave;
    wave.id = 1;
    wave.budget = 1;
    wave.spawnIntervalTicks = 1;
    wave.laneIds = {1};
    wave.bossPool = {201};
    wave.bossCount = 1;
    wave.affixPool = {101};
    wave.affixChoices = 1;
    wave.rewardChoices = 0;
    check(simulation.registerWave(wave));

    if (createCore) {
        simulation.createBaseCore(
            {10, 2}, 1, {1000, 0, 0, 0, 1, 0});
    }
}

bool hasEvent(const TowerDefenseSimulation& simulation, EventType type) {
    return std::any_of(
        simulation.events().begin(), simulation.events().end(),
        [type](const Event& event) { return event.type == type; });
}

void testBossStatsSnapshotAndRoundTrip() {
    TowerDefenseSimulation original(12, 5, 0xabcdu);
    configureModifiedBoss(original, true);
    check(original.submit({0, 1, 1, CommandType::StartWave, 1}));
    check(original.step(0));

    check(hasEvent(original, EventType::WaveAffixSelected));
    check(hasEvent(original, EventType::BossSpawned));
    check(original.snapshot().waveAffixes ==
          std::vector<WaveAffixId>({101}));
    check(original.snapshot().plannedBosses == 1);
    check(original.snapshot().enemies.size() == 1);

    const auto& enemy = original.snapshot().enemies.front();
    check(enemy.bossId == 201);
    check(enemy.spawnSequence == 0);
    check(enemy.healthCurrent == 300);
    check(enemy.healthMaximum == 300);
    check(enemy.armor == 7);
    check(enemy.weaponDamage == 18);
    check(enemy.cellsPerTick == 1);

    const auto* bounty =
        original.rts().world().try_get<gameplay::Bounty>(enemy.entity);
    check(bounty && bounty->amount == 15);

    const auto bytes = EncodeTowerDefenseSimulation(original);
    check(!bytes.empty());
    TowerDefenseSimulation restored(12, 5, 0xabcdu);
    configureModifiedBoss(restored, false);
    check(DecodeTowerDefenseSimulation(bytes, restored));
    check(restored.snapshot().worldHash == original.snapshot().worldHash);
    check(restored.snapshot().waveAffixes == original.snapshot().waveAffixes);
    check(restored.snapshot().enemies.size() == 1);
    check(restored.snapshot().enemies.front().bossId == 201);
    check(restored.snapshot().enemies.front().healthMaximum == 300);

    for (std::uint64_t tick = 1; tick < 8; ++tick) {
        check(original.step(tick));
        check(restored.step(tick));
        check(original.snapshot().worldHash == restored.snapshot().worldHash);
    }
}

void testBossConsumesBudgetBeforeRegularEnemies() {
    auto director = buildDirector(false);
    check(director.begin(7, 0).accepted);
    const auto& plan = director.plan();
    check(!plan.spawns.empty());
    check(plan.spawns.front().bossId != 0);
    const auto selectedBoss = director.boss(plan.spawns.front().bossId);
    check(selectedBoss != nullptr);
    const auto regularCount = static_cast<std::uint32_t>(
        std::count_if(
            plan.spawns.begin(), plan.spawns.end(),
            [](const PlannedSpawn& spawn) { return spawn.bossId == 0; }));
    check(selectedBoss->budgetCost + regularCount * 2u +
              plan.unusedBudget == 10u);
}

void testUnaffordableBossRejected() {
    WaveDirector director(99);
    check(director.registerLane({1, {0, 0}, {4, 0}, 1}));
    check(director.registerBoss(
        {1, 1, 5, 1, {2000, 0, 1000, 1000, 1000}}));
    WaveDefinition wave;
    wave.id = 9;
    wave.budget = 4;
    wave.laneIds = {1};
    wave.bossPool = {1};
    wave.bossCount = 1;
    wave.rewardChoices = 0;
    check(director.registerWave(wave));
    const auto result = director.begin(9, 0);
    check(!result.accepted);
    check(result.failure == WaveStartFailure::NoAffordableBoss);
}

} // namespace

int main() {
    testRegistrationOrderIndependentPlan();
    testBossStatsSnapshotAndRoundTrip();
    testBossConsumesBudgetBeforeRegularEnemies();
    testUnaffordableBossRejected();
    std::cout << "wave modifier tests passed\n";
    return 0;
}
