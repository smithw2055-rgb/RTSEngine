#pragma once

#include <cstdint>
#include <limits>

namespace rts::roguelite {

enum class RewardRarity : std::uint8_t {
    Common,
    Uncommon,
    Rare,
    Epic,
    Legendary
};

constexpr bool ValidRewardRarity(RewardRarity value) noexcept {
    return value >= RewardRarity::Common &&
           value <= RewardRarity::Legendary;
}

constexpr std::uint32_t RewardRarityCost(RewardRarity value) noexcept {
    switch (value) {
    case RewardRarity::Common: return 1u;
    case RewardRarity::Uncommon: return 2u;
    case RewardRarity::Rare: return 4u;
    case RewardRarity::Epic: return 7u;
    case RewardRarity::Legendary: return 11u;
    }
    return std::numeric_limits<std::uint32_t>::max();
}

constexpr RewardRarity MaxRewardRarity(
    RewardRarity first,
    RewardRarity second) noexcept {
    return static_cast<std::uint8_t>(first) >=
                   static_cast<std::uint8_t>(second)
        ? first : second;
}

constexpr bool RewardRarityAtLeast(
    RewardRarity value,
    RewardRarity minimum) noexcept {
    return static_cast<std::uint8_t>(value) >=
           static_cast<std::uint8_t>(minimum);
}

struct RewardOfferRule final {
    // A zero budget keeps legacy weighted reward selection enabled.
    std::uint32_t rarityBudget{};
    RewardRarity guaranteedRarity{RewardRarity::Common};
    std::uint32_t pityAfterMisses{};
    RewardRarity pityRarity{RewardRarity::Rare};

    bool enabled() const noexcept { return rarityBudget != 0; }

    bool valid() const noexcept {
        if (!ValidRewardRarity(guaranteedRarity) ||
            !ValidRewardRarity(pityRarity)) {
            return false;
        }
        if (!enabled()) {
            return guaranteedRarity == RewardRarity::Common &&
                   pityAfterMisses == 0 &&
                   pityRarity == RewardRarity::Rare;
        }
        return rarityBudget >= RewardRarityCost(guaranteedRarity) &&
               (pityAfterMisses == 0 ||
                rarityBudget >= RewardRarityCost(pityRarity));
    }

    friend bool operator==(const RewardOfferRule& a,
                           const RewardOfferRule& b) noexcept {
        return a.rarityBudget == b.rarityBudget &&
               a.guaranteedRarity == b.guaranteedRarity &&
               a.pityAfterMisses == b.pityAfterMisses &&
               a.pityRarity == b.pityRarity;
    }
};

} // namespace rts::roguelite
