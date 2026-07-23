#pragma once

#include <RTSEngine/Roguelite/ModifierRuntime.h>
#include <RTSEngine/Roguelite/RewardRarity.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/foundation/Random.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::roguelite {

enum class RewardOfferPlanFailure : std::uint8_t {
    None,
    InvalidRule,
    InsufficientCandidates,
    GuaranteeUnavailable,
    BudgetInsufficient,
    InvalidWeight
};

struct PlannedModifierOffer final {
    bool accepted{};
    RewardOfferPlanFailure failure{RewardOfferPlanFailure::None};
    std::uint32_t rarityBudget{};
    std::uint32_t raritySpent{};
    RewardRarity guaranteedRarity{RewardRarity::Common};
    RewardRarity effectiveGuaranteedRarity{RewardRarity::Common};
    std::uint32_t pityBefore{};
    std::uint32_t pityAfter{};
    bool pityTriggered{};
    std::vector<ModifierId> choices;
    std::vector<RewardRarity> rarities;

    friend bool operator==(const PlannedModifierOffer& a,
                           const PlannedModifierOffer& b) noexcept {
        return a.accepted == b.accepted &&
               a.failure == b.failure &&
               a.rarityBudget == b.rarityBudget &&
               a.raritySpent == b.raritySpent &&
               a.guaranteedRarity == b.guaranteedRarity &&
               a.effectiveGuaranteedRarity ==
                   b.effectiveGuaranteedRarity &&
               a.pityBefore == b.pityBefore &&
               a.pityAfter == b.pityAfter &&
               a.pityTriggered == b.pityTriggered &&
               a.choices == b.choices &&
               a.rarities == b.rarities;
    }
};

class RewardRarityPlanner final {
public:
    static PlannedModifierOffer plan(
        std::uint64_t rootSeed,
        std::uint32_t runId,
        std::uint32_t waveIndex,
        std::uint32_t waveId,
        const RewardOfferRule& rule,
        std::uint32_t pityMisses,
        std::vector<ModifierDefinition> candidates,
        std::uint32_t requestedChoices) {
        PlannedModifierOffer result;
        result.rarityBudget = rule.rarityBudget;
        result.guaranteedRarity = rule.guaranteedRarity;
        result.pityBefore = pityMisses;

        if (!rule.enabled() || !rule.valid()) {
            result.failure = RewardOfferPlanFailure::InvalidRule;
            return result;
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const ModifierDefinition& a,
                     const ModifierDefinition& b) {
                      return a.id < b.id;
                  });
        candidates.erase(
            std::unique(candidates.begin(), candidates.end(),
                        [](const ModifierDefinition& a,
                           const ModifierDefinition& b) {
                            return a.id == b.id;
                        }),
            candidates.end());
        if (requestedChoices == 0) {
            result.accepted = true;
            result.pityAfter = pityMisses;
            return result;
        }
        if (candidates.size() < requestedChoices) {
            result.failure = RewardOfferPlanFailure::InsufficientCandidates;
            return result;
        }

        result.pityTriggered = rule.pityAfterMisses != 0 &&
                               pityMisses >= rule.pityAfterMisses;
        result.effectiveGuaranteedRarity = MaxRewardRarity(
            rule.guaranteedRarity,
            result.pityTriggered ? rule.pityRarity
                                 : RewardRarity::Common);

        if (!hasGuaranteedCandidate(
                candidates,
                rule.rarityBudget,
                result.effectiveGuaranteedRarity)) {
            result.failure = RewardOfferPlanFailure::GuaranteeUnavailable;
            return result;
        }

        foundation::RandomStream random(
            rootSeed,
            streamId(runId, waveIndex, waveId));
        std::uint32_t remaining = rule.rarityBudget;

        auto selected = selectCandidate(
            random,
            candidates,
            remaining,
            result.effectiveGuaranteedRarity,
            true,
            requestedChoices - 1u);
        if (selected.invalidWeight) {
            result.failure = RewardOfferPlanFailure::InvalidWeight;
            return result;
        }
        if (selected.index == candidates.size()) {
            result.failure = RewardOfferPlanFailure::BudgetInsufficient;
            return result;
        }
        appendSelection(result, candidates[selected.index]);
        remaining -= RewardRarityCost(candidates[selected.index].rarity);
        candidates.erase(candidates.begin() +
                         static_cast<std::ptrdiff_t>(selected.index));

        while (result.choices.size() < requestedChoices) {
            const auto slotsAfter = requestedChoices -
                static_cast<std::uint32_t>(result.choices.size()) - 1u;
            selected = selectCandidate(
                random,
                candidates,
                remaining,
                RewardRarity::Common,
                false,
                slotsAfter);
            if (selected.invalidWeight) {
                result.failure = RewardOfferPlanFailure::InvalidWeight;
                result.choices.clear();
                result.rarities.clear();
                result.raritySpent = 0;
                return result;
            }
            if (selected.index == candidates.size()) {
                result.failure = RewardOfferPlanFailure::BudgetInsufficient;
                result.choices.clear();
                result.rarities.clear();
                result.raritySpent = 0;
                return result;
            }
            appendSelection(result, candidates[selected.index]);
            remaining -= RewardRarityCost(candidates[selected.index].rarity);
            candidates.erase(candidates.begin() +
                             static_cast<std::ptrdiff_t>(selected.index));
        }

        sortSelections(result);
        const bool containsPityRarity = std::any_of(
            result.rarities.begin(), result.rarities.end(),
            [&](RewardRarity value) {
                return RewardRarityAtLeast(value, rule.pityRarity);
            });
        if (rule.pityAfterMisses == 0) {
            result.pityAfter = pityMisses;
        } else if (containsPityRarity) {
            result.pityAfter = 0;
        } else {
            result.pityAfter = pityMisses ==
                    std::numeric_limits<std::uint32_t>::max()
                ? pityMisses : pityMisses + 1u;
        }
        result.accepted = true;
        result.failure = RewardOfferPlanFailure::None;
        return result;
    }

private:
    struct Selection final {
        std::size_t index{};
        bool invalidWeight{};
    };

    static foundation::RandomStreamId streamId(
        std::uint32_t runId,
        std::uint32_t waveIndex,
        std::uint32_t waveId) noexcept {
        foundation::CanonicalHash hash;
        hash.WriteString("roguelite.reward-rarity");
        hash.WriteU32(runId);
        hash.WriteU32(waveIndex);
        hash.WriteU32(waveId);
        return hash.Value();
    }

    static bool hasGuaranteedCandidate(
        const std::vector<ModifierDefinition>& candidates,
        std::uint32_t budget,
        RewardRarity minimum) noexcept {
        return std::any_of(
            candidates.begin(), candidates.end(),
            [&](const ModifierDefinition& value) {
                return RewardRarityAtLeast(value.rarity, minimum) &&
                       RewardRarityCost(value.rarity) <= budget;
            });
    }

    static bool canCompleteAfter(
        const std::vector<ModifierDefinition>& candidates,
        std::size_t selected,
        std::uint32_t remainingBudget,
        std::uint32_t slotsAfter) {
        const auto selectedCost = RewardRarityCost(
            candidates[selected].rarity);
        if (selectedCost > remainingBudget) return false;
        if (slotsAfter == 0) return true;

        std::vector<std::uint32_t> costs;
        costs.reserve(candidates.size() - 1u);
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (index != selected) {
                costs.push_back(RewardRarityCost(candidates[index].rarity));
            }
        }
        if (costs.size() < slotsAfter) return false;
        std::sort(costs.begin(), costs.end());
        std::uint64_t required = selectedCost;
        for (std::uint32_t index = 0; index < slotsAfter; ++index) {
            required += costs[index];
        }
        return required <= remainingBudget;
    }

    static Selection selectCandidate(
        foundation::RandomStream& random,
        const std::vector<ModifierDefinition>& candidates,
        std::uint32_t remainingBudget,
        RewardRarity minimum,
        bool requireMinimum,
        std::uint32_t slotsAfter) {
        std::vector<std::size_t> eligible;
        std::uint64_t totalWeight = 0;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            if (requireMinimum &&
                !RewardRarityAtLeast(candidate.rarity, minimum)) {
                continue;
            }
            if (!canCompleteAfter(
                    candidates, index, remainingBudget, slotsAfter)) {
                continue;
            }
            eligible.push_back(index);
            totalWeight += candidate.weight;
        }
        if (eligible.empty()) return {candidates.size(), false};
        if (totalWeight == 0 ||
            totalWeight > std::numeric_limits<std::uint32_t>::max()) {
            return {candidates.size(), true};
        }
        auto roll = random.NextBounded(
            static_cast<std::uint32_t>(totalWeight));
        for (const auto index : eligible) {
            if (roll < candidates[index].weight) return {index, false};
            roll -= candidates[index].weight;
        }
        return {eligible.back(), false};
    }

    static void appendSelection(
        PlannedModifierOffer& result,
        const ModifierDefinition& value) {
        result.choices.push_back(value.id);
        result.rarities.push_back(value.rarity);
        result.raritySpent += RewardRarityCost(value.rarity);
    }

    static void sortSelections(PlannedModifierOffer& result) {
        std::vector<std::pair<ModifierId, RewardRarity>> values;
        values.reserve(result.choices.size());
        for (std::size_t index = 0; index < result.choices.size(); ++index) {
            values.emplace_back(result.choices[index], result.rarities[index]);
        }
        std::sort(values.begin(), values.end(),
                  [](const auto& a, const auto& b) {
                      return a.first < b.first;
                  });
        for (std::size_t index = 0; index < values.size(); ++index) {
            result.choices[index] = values[index].first;
            result.rarities[index] = values[index].second;
        }
    }
};

} // namespace rts::roguelite
