#include <RTSEngine/Roguelite/RewardRarityPlanner.h>
#include <RTSEngine/Roguelite/RunSimulationArchive.h>

#include <algorithm>
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

roguelite::ModifierDefinition modifier(
    roguelite::ModifierId id,
    roguelite::RewardRarity rarity,
    std::uint32_t weight = 1) {
    roguelite::ModifierDefinition result;
    result.id = id;
    result.weight = weight;
    result.maxStacks = 1;
    result.rarity = rarity;
    return result;
}

void testPlannerIsOrderIndependentAndBudgeted() {
    const roguelite::RewardOfferRule rule{
        7, roguelite::RewardRarity::Rare, 0,
        roguelite::RewardRarity::Rare};
    std::vector<roguelite::ModifierDefinition> candidates = {
        modifier(1, roguelite::RewardRarity::Common, 5),
        modifier(2, roguelite::RewardRarity::Uncommon, 3),
        modifier(3, roguelite::RewardRarity::Rare, 2),
        modifier(4, roguelite::RewardRarity::Common, 1)
    };
    const auto first = roguelite::RewardRarityPlanner::plan(
        0x1234u, 7, 0, 10, rule, 0, candidates, 3);
    std::reverse(candidates.begin(), candidates.end());
    const auto second = roguelite::RewardRarityPlanner::plan(
        0x1234u, 7, 0, 10, rule, 0, candidates, 3);

    check(first.accepted);
    check(first == second);
    check(first.choices.size() == 3);
    check(first.raritySpent <= first.rarityBudget);
    check(std::any_of(
        first.rarities.begin(), first.rarities.end(),
        [](roguelite::RewardRarity value) {
            return roguelite::RewardRarityAtLeast(
                value, roguelite::RewardRarity::Rare);
        }));
}

void testPityRaisesTheEffectiveGuarantee() {
    const roguelite::RewardOfferRule rule{
        5, roguelite::RewardRarity::Common, 2,
        roguelite::RewardRarity::Rare};
    const std::vector<roguelite::ModifierDefinition> candidates = {
        modifier(1, roguelite::RewardRarity::Common, 100),
        modifier(2, roguelite::RewardRarity::Rare, 1)
    };
    const auto plan = roguelite::RewardRarityPlanner::plan(
        0x4321u, 9, 3, 40, rule, 2, candidates, 2);

    check(plan.accepted);
    check(plan.pityTriggered);
    check(plan.pityBefore == 2);
    check(plan.pityAfter == 0);
    check(plan.effectiveGuaranteedRarity ==
          roguelite::RewardRarity::Rare);
    check(std::find(plan.choices.begin(), plan.choices.end(), 2) !=
          plan.choices.end());
}

void testUnsatisfiedPoliciesAreExplicit() {
    const roguelite::RewardOfferRule noRare{
        4, roguelite::RewardRarity::Rare, 0,
        roguelite::RewardRarity::Rare};
    auto result = roguelite::RewardRarityPlanner::plan(
        1, 1, 0, 1, noRare, 0,
        {modifier(1, roguelite::RewardRarity::Common)}, 1);
    check(!result.accepted);
    check(result.failure ==
          roguelite::RewardOfferPlanFailure::GuaranteeUnavailable);

    const roguelite::RewardOfferRule noRemainingBudget{
        4, roguelite::RewardRarity::Rare, 0,
        roguelite::RewardRarity::Rare};
    result = roguelite::RewardRarityPlanner::plan(
        1, 1, 0, 1, noRemainingBudget, 0,
        {modifier(1, roguelite::RewardRarity::Common),
         modifier(2, roguelite::RewardRarity::Rare)}, 2);
    check(!result.accepted);
    check(result.failure ==
          roguelite::RewardOfferPlanFailure::BudgetInsufficient);
}

void configureRarityRun(
    roguelite::RunSimulation& simulation,
    bool createActors,
    std::uint32_t secondBudget = 4) {
    gameplay::UnitDefinition enemy;
    enemy.id = 1;
    enemy.cellsPerTick = 1;
    enemy.combat = {10, 0, 1, 1, 1, 0};
    simulation.registerUnit(enemy);
    check(simulation.registerLane({1, {0, 2}, {10, 2}, 1}));

    check(simulation.registerModifier(
        modifier(1, roguelite::RewardRarity::Common, 10)));
    check(simulation.registerModifier(
        modifier(2, roguelite::RewardRarity::Common, 10)));
    check(simulation.registerModifier(
        modifier(3, roguelite::RewardRarity::Rare, 1)));

    tower_defense::WaveDefinition first;
    first.id = 1;
    first.budget = 1;
    first.spawnIntervalTicks = 1;
    first.laneIds = {1};
    first.enemies = {{1, 1, 1, 1}};
    first.rewardPool = {1, 2};
    first.rewardChoices = 2;
    check(simulation.registerWave(first));

    auto second = first;
    second.id = 2;
    second.rewardPool = {2, 3};
    second.rewardChoices = 1;
    check(simulation.registerWave(second));

    roguelite::RunDefinition run;
    run.id = 7;
    run.waves = {1, 2};
    run.rewardRules = {
        {2, roguelite::RewardRarity::Common, 1,
         roguelite::RewardRarity::Rare},
        {secondBudget, roguelite::RewardRarity::Common, 1,
         roguelite::RewardRarity::Rare}
    };
    check(simulation.registerRun(run));

    if (createActors) {
        simulation.createBaseCore(
            {10, 2}, 1, {500, 0, 0, 0, 1, 0});
        simulation.createDefender(
            {7, 2}, {0}, 1, {500, 0, 100, 4, 1, 0});
    }
}

std::uint64_t advanceToPityReward(
    roguelite::RunSimulation& simulation) {
    check(simulation.submit(
        {0, 1, 1, roguelite::CommandType::StartRun, 7}));
    bool firstChoiceQueued = false;
    for (std::uint64_t tick = 0; tick < 160; ++tick) {
        check(simulation.step(tick));
        if (simulation.state().phase == roguelite::RunPhase::RewardPending &&
            simulation.state().waveIndex == 0 && !firstChoiceQueued) {
            auto choices = simulation.tower().snapshot().rewardChoices;
            std::sort(choices.begin(), choices.end());
            check(choices ==
                  std::vector<roguelite::ModifierId>({1, 2}));
            check(simulation.submit(
                {tick + 1u, 1, 2,
                 roguelite::CommandType::ChooseModifier, 1}));
            firstChoiceQueued = true;
        }
        if (simulation.state().phase == roguelite::RunPhase::RewardPending &&
            simulation.state().waveIndex == 1) {
            return tick;
        }
    }
    std::abort();
}

void checkPityHistory(const roguelite::RunSimulation& simulation) {
    const auto& history = simulation.history();
    check(history.waves.size() == 2);
    const auto& first = history.waves[0];
    check(first.rewardRarityBudget == 2);
    check(first.rewardRaritySpent == 2);
    check(first.guaranteedRarity == roguelite::RewardRarity::Common);
    check(first.effectiveGuaranteedRarity ==
          roguelite::RewardRarity::Common);
    check(first.pityBefore == 0);
    check(first.pityAfter == 1);
    check(!first.pityTriggered);
    check(first.rewardChoices ==
          std::vector<roguelite::ModifierId>({1, 2}));
    check(first.rewardRarities ==
          std::vector<roguelite::RewardRarity>({
              roguelite::RewardRarity::Common,
              roguelite::RewardRarity::Common}));
    check(first.selectedModifier == 1);
    check(first.modifierApplied);

    const auto& second = history.waves[1];
    check(second.phase == roguelite::WaveResultPhase::RewardPending);
    check(second.rewardRarityBudget == 4);
    check(second.rewardRaritySpent == 4);
    check(second.guaranteedRarity == roguelite::RewardRarity::Common);
    check(second.effectiveGuaranteedRarity ==
          roguelite::RewardRarity::Rare);
    check(second.pityBefore == 1);
    check(second.pityAfter == 0);
    check(second.pityTriggered);
    check(second.rewardChoices ==
          std::vector<roguelite::ModifierId>({3}));
    check(second.rewardRarities ==
          std::vector<roguelite::RewardRarity>({
              roguelite::RewardRarity::Rare}));
    check(history.rewardPityMisses == 0);
    check(simulation.snapshot().rewardPityMisses == 0);
}

void testIntegratedPityRoundTrip() {
    roguelite::RunSimulation original(12, 5, 0x7788u);
    configureRarityRun(original, true);
    const auto rewardTick = advanceToPityReward(original);
    checkPityHistory(original);

    const auto bytes = roguelite::EncodeRunSimulation(original);
    check(!bytes.empty());
    roguelite::RunSimulation restored(12, 5, 0x7788u);
    configureRarityRun(restored, false);
    check(roguelite::DecodeRunSimulation(bytes, restored));
    check(restored.snapshot().worldHash == original.snapshot().worldHash);
    check(restored.history() == original.history());
    checkPityHistory(restored);

    roguelite::RunSimulation incompatible(12, 5, 0x7788u);
    configureRarityRun(incompatible, false, 5);
    const auto before = roguelite::EncodeRunSimulation(incompatible);
    check(!roguelite::DecodeRunSimulation(bytes, incompatible));
    check(roguelite::EncodeRunSimulation(incompatible) == before);

    const roguelite::TickCommand choose{
        rewardTick + 1u, 1, 3,
        roguelite::CommandType::ChooseModifier, 3};
    check(original.submit(choose));
    check(restored.submit(choose));
    bool completed = false;
    for (std::uint64_t tick = rewardTick + 1u; tick < 220; ++tick) {
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
    check(original.history().phase == roguelite::RunHistoryPhase::Complete);
    check(original.history().waves.back().selectedModifier == 3);
    check(original.history().waves.back().modifierApplied);
    check(roguelite::EncodeRunSimulation(original) ==
          roguelite::EncodeRunSimulation(restored));
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "all";
    if (mode == "planner" || mode == "all") {
        testPlannerIsOrderIndependentAndBudgeted();
    }
    if (mode == "pity" || mode == "all") {
        testPityRaisesTheEffectiveGuarantee();
    }
    if (mode == "failures" || mode == "all") {
        testUnsatisfiedPoliciesAreExplicit();
    }
    if (mode == "integration" || mode == "all") {
        testIntegratedPityRoundTrip();
    }
    std::cout << "reward rarity " << mode << " tests passed\n";
    return 0;
}
