#pragma once

#include <RTSEngine/Rts/Combat.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

inline constexpr std::int32_t kGameplayModifierScale = 1000;

struct TeamModifierProfile {
    std::int32_t unitHealth{1000};
    std::int32_t unitDamage{1000};
    std::int32_t unitArmorAdd{};
    std::int32_t unitMoveSpeed{1000};
    std::int32_t buildingHealth{1000};
    std::int32_t buildingDamage{1000};
    std::int32_t constructionSpeed{1000};
    std::int32_t productionSpeed{1000};
    std::int32_t bountyMultiplier{1000};

    friend bool operator==(const TeamModifierProfile& a,
                           const TeamModifierProfile& b) noexcept {
        return a.unitHealth == b.unitHealth &&
               a.unitDamage == b.unitDamage &&
               a.unitArmorAdd == b.unitArmorAdd &&
               a.unitMoveSpeed == b.unitMoveSpeed &&
               a.buildingHealth == b.buildingHealth &&
               a.buildingDamage == b.buildingDamage &&
               a.constructionSpeed == b.constructionSpeed &&
               a.productionSpeed == b.productionSpeed &&
               a.bountyMultiplier == b.bountyMultiplier;
    }

    friend bool operator!=(const TeamModifierProfile& a,
                           const TeamModifierProfile& b) noexcept {
        return !(a == b);
    }
};

struct TeamModifierEntry {
    std::uint32_t teamId{};
    TeamModifierProfile profile{};

    friend bool operator==(const TeamModifierEntry& a,
                           const TeamModifierEntry& b) noexcept {
        return a.teamId == b.teamId && a.profile == b.profile;
    }
};

struct TunableStats {
    bool building{};
    std::int32_t baseMoveSpeed{};
    CombatStats baseCombat{};
};

inline TeamModifierProfile SanitizeTeamModifierProfile(
    TeamModifierProfile value) noexcept {
    constexpr std::int32_t maximumMultiplier = 1000000;
    const auto multiplier = [maximumMultiplier](std::int32_t current) {
        return std::clamp(current, 0, maximumMultiplier);
    };
    value.unitHealth = multiplier(value.unitHealth);
    value.unitDamage = multiplier(value.unitDamage);
    value.unitMoveSpeed = multiplier(value.unitMoveSpeed);
    value.buildingHealth = multiplier(value.buildingHealth);
    value.buildingDamage = multiplier(value.buildingDamage);
    value.constructionSpeed =
        std::clamp(value.constructionSpeed, 1, maximumMultiplier);
    value.productionSpeed =
        std::clamp(value.productionSpeed, 1, maximumMultiplier);
    value.bountyMultiplier = multiplier(value.bountyMultiplier);
    return value;
}

inline std::int32_t ScaleGameplayValue(std::int32_t baseValue,
                                       std::int32_t multiplier) noexcept {
    const auto scaled =
        (static_cast<std::int64_t>(baseValue) * multiplier) /
        kGameplayModifierScale;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        scaled,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

inline std::uint32_t ScaleGameplayDuration(
    std::uint32_t baseTicks,
    std::int32_t speedMultiplier) noexcept {
    const auto base = std::max<std::uint32_t>(1, baseTicks);
    const auto speed = std::max<std::int32_t>(1, speedMultiplier);
    const auto numerator =
        static_cast<std::uint64_t>(base) * kGameplayModifierScale +
        static_cast<std::uint64_t>(speed - 1);
    const auto resolved = numerator / static_cast<std::uint64_t>(speed);
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        resolved,
        std::uint64_t{1},
        static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max())));
}

class TeamModifierTable final {
public:
    bool set(std::uint32_t teamId, TeamModifierProfile profile) {
        profile = SanitizeTeamModifierProfile(profile);
        const auto iterator = std::lower_bound(
            entries_.begin(), entries_.end(), teamId,
            [](const TeamModifierEntry& current, std::uint32_t id) {
                return current.teamId < id;
            });
        if (iterator != entries_.end() && iterator->teamId == teamId) {
            if (iterator->profile == profile) return false;
            iterator->profile = profile;
            return true;
        }
        entries_.insert(iterator, TeamModifierEntry{teamId, profile});
        return true;
    }

    const TeamModifierProfile& profile(std::uint32_t teamId) const noexcept {
        const auto iterator = std::lower_bound(
            entries_.begin(), entries_.end(), teamId,
            [](const TeamModifierEntry& current, std::uint32_t id) {
                return current.teamId < id;
            });
        return iterator != entries_.end() && iterator->teamId == teamId
            ? iterator->profile
            : defaultProfile();
    }

    const std::vector<TeamModifierEntry>& entries() const noexcept {
        return entries_;
    }

    void appendHash(foundation::CanonicalHash& hash) const noexcept {
        hash.WriteU32(static_cast<std::uint32_t>(entries_.size()));
        for (const auto& entry : entries_) {
            hash.WriteU32(entry.teamId);
            hash.WriteI32(entry.profile.unitHealth);
            hash.WriteI32(entry.profile.unitDamage);
            hash.WriteI32(entry.profile.unitArmorAdd);
            hash.WriteI32(entry.profile.unitMoveSpeed);
            hash.WriteI32(entry.profile.buildingHealth);
            hash.WriteI32(entry.profile.buildingDamage);
            hash.WriteI32(entry.profile.constructionSpeed);
            hash.WriteI32(entry.profile.productionSpeed);
            hash.WriteI32(entry.profile.bountyMultiplier);
        }
    }

private:
    static const TeamModifierProfile& defaultProfile() noexcept {
        static const TeamModifierProfile value{};
        return value;
    }

    std::vector<TeamModifierEntry> entries_;
};

} // namespace rts::gameplay
