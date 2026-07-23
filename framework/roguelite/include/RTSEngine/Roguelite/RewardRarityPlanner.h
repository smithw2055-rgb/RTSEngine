#pragma once

#include <RTSEngine/Roguelite/ModifierRuntime.h>
#include <RTSEngine/Roguelite/RewardRarity.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/foundation/Random.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
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

        foundation::RandomStream random(
            rootSeed,
            streamId(runId, waveIndex, waveId));
        std::uint32_t remaining = rule.rarityBudget;

        const auto guarantee = selectCandidate(
            random,
            candidates,
            remaining,
            result.effectiveGuaranteedRarity,
            true);
        if (guarantee == candidates.size()) {
            result.failure = RewardOfferPlanFailure::GuaranteeUnavailable;
            return result;
        }
        appendSelection(result, candidates[guarantee]);
        remaining -= RewardRarityCost(candidates[guarantee].rarity);
        candidates.erase(candidates.begin() +
                         static_cast<std::ptrdiff_t>(guarantee));

        while (result.choices.size() < requestedChoices) {
            const auto selected = selectCandidate(
                random,
                candidates,
                remaining,
                RewardRarity::Common,
                false);
            if (selected == candidates.size()) {
                result.failure = RewardOfferPlanFailure::BudgetInsufficient;
                result.choices.clear();
                result.rarities.clear();
                result.raritySpent = 0;
                return result;
            }
            appendSelection(result, candidates[selected]);
            remaining -= RewardRarityCost(candidates[selected].rarity);
            candidates.erase(candidates.begin() +
                             static_cast<std::ptrdiff_t>(selected));
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

    static std::size_t selectCandidate(
        foundation::RandomStream& random,
        const std::vector<ModifierDefinition>& candidates,
        std::uint32_t remainingBudget,
        RewardRarity minimum,
        bool requireMinimum) {
        std::vector<std::size_t> eligible;
        std::uint64_t totalWeight = 0;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            if (RewardRarityCost(candidate.rarity) > remainingBudget) continue;
            if (requireMinimum &&
                !RewardRarityAtLeast(candidate.rarity, minimum)) {
                continue;
            }
            eligible.push_back(index);
            totalWeight += candidate.weight;
        }
        if (eligible.empty() || totalWeight == 0 ||
            totalWeight > std::numeric_limits<std::uint32_t>::max()) {
            return candidates.size();
        }
        auto roll = random.NextBounded(
            static_cast<std::uint32_t>(totalWeight));
        for (const auto index : eligible) {
            if (roll < candidates[index].weight) return index;
            roll -= candidates[index].weight;
        }
        return eligible.back();
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
