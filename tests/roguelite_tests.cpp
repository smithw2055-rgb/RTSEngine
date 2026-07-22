#include <RTSEngine/Roguelite/RunSimulation.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace rts;
using namespace rts::roguelite;

void testSparseStackLookup() {
    ModifierRuntime runtime;
    const auto stat = MakeStatId("test.lookup");

    ModifierDefinition low;
    low.id = 1;
    low.effects = {{stat, ModifierOperation::Add, 1}};
    assert(runtime.registerDefinition(low));

    ModifierDefinition high;
    high.id = 9;
    high.effects = {{stat, ModifierOperation::Add, 9}};
    assert(runtime.registerDefinition(high));

    assert(runtime.apply(9).accepted);
    assert(runtime.stackCount(1) == 0);
    assert(runtime.stackCount(9) == 1);
    assert(runtime.canApply(1) == ApplyFailure::None);
}

void testModifierRules() {
    ModifierRuntime runtime;
    const auto power = MakeStatId("test.power");
    const auto offense = MakeTagId("build.offense");

    ModifierDefinition additive;
    additive.id = 1;
    additive.maxStacks = 2;
    additive.tags = {offense};
    additive.effects = {
        {power, ModifierOperation::Add, 10}
    };
    assert(runtime.registerDefinition(additive));

    ModifierDefinition multiplier;
    multiplier.id = 2;
    multiplier.requiredTags = {offense};
    multiplier.effects = {
        {power, ModifierOperation::Multiply, 1500}
    };
    assert(runtime.registerDefinition(multiplier));

    ModifierDefinition excluded;
    excluded.id = 3;
    excluded.excludedModifiers = {2};
    excluded.effects = {
        {power, ModifierOperation::Override, 99}
    };
    assert(runtime.registerDefinition(excluded));

    ModifierDefinition finalOverride;
    finalOverride.id = 4;
    finalOverride.requiredModifiers = {2};
    finalOverride.effects = {
        {power, ModifierOperation::Override, 7}
    };
    assert(runtime.registerDefinition(finalOverride));

    assert(runtime.canApply(2) ==
           ApplyFailure::MissingPrerequisite);
    assert(runtime.apply(1).accepted);
    assert(runtime.apply(1).accepted);
    assert(runtime.apply(1).failure ==
           ApplyFailure::MaximumStacks);
    assert(runtime.resolve(power, 10) == 30);

    assert(runtime.apply(2).accepted);
    assert(runtime.resolve(power, 10) == 45);
    assert(runtime.apply(3).failure == ApplyFailure::Excluded);

    assert(runtime.apply(4).accepted);
    assert(runtime.resolve(power, 10) == 7);
    assert(runtime.hasTag(offense));

    foundation::CanonicalHash first;
    foundation::CanonicalHash second;
    runtime.appendHash(first);
    runtime.appendHash(second);
    assert(first.Value() == second.Value());
}

struct ScenarioResult {
    std::vector<std::uint64_t> hashes;
    std::vector<ModifierId> choices;
    std::vector<ModifierStack> modifiers;
    std::int32_t resources{};
    std::int32_t finalBonus{};
    RunState state{};
};

ScenarioResult runScenario() {
    RunSimulation simulation(12, 8, 424242);
    simulation.setResources(100);
    simulation.setPlayerTeam(1);

    gameplay::UnitDefinition enemy;
    enemy.id = 1;
    enemy.cellsPerTick = 1;
    enemy.combat.maximumHealth = 1;
    enemy.combat.weaponDamage = 1;
    enemy.combat.weaponRange = 1;
    enemy.combat.cooldownTicks = 1;
    assert(enemy.combat.attackCapable());
    simulation.registerUnit(enemy);

    assert(simulation.registerLane(
        {1, {10, 4}, {1, 4}, 1}));

    ModifierDefinition salvage;
    salvage.id = 1;
    salvage.weight = 1;
    salvage.maxStacks = 1;
    salvage.tags = {MakeTagId("build.economy")};
    salvage.effects = {
        {WaveCompletionResourceStat(),
         ModifierOperation::Add, 7}
    };
    assert(simulation.registerModifier(salvage));

    ModifierDefinition compound;
    compound.id = 2;
    compound.weight = 1;
    compound.maxStacks = 1;
    compound.requiredTags = {
        MakeTagId("build.economy")
    };
    compound.effects = {
        {WaveCompletionResourceStat(),
         ModifierOperation::Multiply, 2000}
    };
    assert(simulation.registerModifier(compound));

    ModifierDefinition incompatible;
    incompatible.id = 3;
    incompatible.weight = 100;
    incompatible.maxStacks = 1;
    incompatible.excludedTags = {
        MakeTagId("build.economy")
    };
    incompatible.effects = {
        {WaveCompletionResourceStat(),
         ModifierOperation::Override, 100}
    };
    assert(simulation.registerModifier(incompatible));

    tower_defense::WaveDefinition firstWave;
    firstWave.id = 1;
    firstWave.budget = 1;
    firstWave.spawnIntervalTicks = 1;
    firstWave.enemyTeamId = 2;
    firstWave.laneIds = {1};
    firstWave.enemies = {{1, 1, 1, 1}};
    firstWave.rewardPool = {1};
    firstWave.rewardChoices = 1;
    assert(simulation.registerWave(firstWave));

    tower_defense::WaveDefinition secondWave = firstWave;
    secondWave.id = 2;
    secondWave.rewardPool = {2, 3};
    assert(simulation.registerWave(secondWave));

    assert(simulation.registerRun({1, {1, 2}}));

    gameplay::CombatStats coreStats;
    coreStats.maximumHealth = 100;
    simulation.createBaseCore({1, 4}, 1, coreStats);

    gameplay::CombatStats defenderStats;
    defenderStats.maximumHealth = 20;
    defenderStats.weaponDamage = 10;
    defenderStats.weaponRange = 20;
    defenderStats.cooldownTicks = 1;
    simulation.createDefender(
        {6, 4}, {0}, 1, defenderStats);

    assert(simulation.submit(
        {0, 1, 1, CommandType::StartRun, 1}));
    assert(simulation.submit(
        {0, 1, 1, CommandType::StartRun, 1}));

    ScenarioResult result;
    std::uint32_t submittedChoices = 0;
    std::uint32_t sequence = 2;
    for (std::uint64_t tick = 0; tick < 32; ++tick) {
        assert(simulation.step(tick));
        result.hashes.push_back(simulation.snapshot().worldHash);

        if (simulation.state().phase ==
                RunPhase::RewardPending &&
            submittedChoices ==
                simulation.state().completedWaves) {
            const auto& offered =
                simulation.tower().snapshot().rewardChoices;
            assert(offered.size() == 1);
            result.choices.push_back(offered.front());
            assert(simulation.submit(
                {tick + 1, 1, sequence++,
                 CommandType::ChooseModifier,
                 offered.front()}));
            ++submittedChoices;
        }

        if (simulation.state().phase == RunPhase::Complete ||
            simulation.state().phase == RunPhase::Failed) {
            break;
        }
    }

    result.modifiers = simulation.snapshot().modifiers;
    result.resources = simulation.snapshot().availableResources;
    result.finalBonus =
        simulation.snapshot().waveCompletionResourceBonus;
    result.state = simulation.state();
    return result;
}

} // namespace

int main() {
    testSparseStackLookup();
    testModifierRules();

    const auto first = runScenario();
    const auto second = runScenario();

    assert(first.hashes == second.hashes);
    assert(first.choices == second.choices);
    assert(first.modifiers == second.modifiers);
    assert(first.choices == std::vector<ModifierId>({1, 2}));
    assert(first.modifiers ==
           std::vector<ModifierStack>({{1, 1}, {2, 1}}));
    assert(first.resources == 107);
    assert(first.finalBonus == 14);
    assert(first.state.phase == RunPhase::Complete);
    assert(first.state.completedWaves == 2);

    std::cout << "roguelite tests passed\n";
    return 0;
}
