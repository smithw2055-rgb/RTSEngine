#pragma once

#include <RTSEngine/Rts/Combat.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace rts::tower_defense {

using WaveAffixId = std::uint32_t;
using BossId = std::uint32_t;

struct EnemyStatModifier final {
    std::int32_t healthPermille{1000};
    std::int32_t armorAdd{};
    std::int32_t damagePermille{1000};
    std::int32_t speedPermille{1000};
    std::int32_t bountyPermille{1000};

    bool valid() const noexcept {
        return healthPermille > 0 && damagePermille > 0 &&
               speedPermille > 0 && bountyPermille > 0;
    }

    friend bool operator==(const EnemyStatModifier& a,
                           const EnemyStatModifier& b) noexcept {
        return a.healthPermille == b.healthPermille &&
               a.armorAdd == b.armorAdd &&
               a.damagePermille == b.damagePermille &&
               a.speedPermille == b.speedPermille &&
               a.bountyPermille == b.bountyPermille;
    }
};

struct WaveAffixDefinition final {
    WaveAffixId id{};
    std::uint32_t weight{1};
    EnemyStatModifier modifier{};

    friend bool operator==(const WaveAffixDefinition& a,
                           const WaveAffixDefinition& b) noexcept {
        return a.id == b.id && a.weight == b.weight &&
               a.modifier == b.modifier;
    }
};

struct BossDefinition final {
    BossId id{};
    std::uint32_t unitDefinitionId{};
    std::uint32_t budgetCost{1};
    std::uint32_t weight{1};
    EnemyStatModifier modifier{};

    friend bool operator==(const BossDefinition& a,
                           const BossDefinition& b) noexcept {
        return a.id == b.id &&
               a.unitDefinitionId == b.unitDefinitionId &&
               a.budgetCost == b.budgetCost &&
               a.weight == b.weight &&
               a.modifier == b.modifier;
    }
};

inline std::int32_t ClampI32(std::int64_t value) noexcept {
    return static_cast<std::int32_t>(std::max<std::int64_t>(
        std::numeric_limits<std::int32_t>::min(),
        std::min<std::int64_t>(
            std::numeric_limits<std::int32_t>::max(), value)));
}

inline std::int32_t ScaleI32(
    std::int32_t value,
    std::int32_t permille) noexcept {
    return ClampI32(
        static_cast<std::int64_t>(value) * permille / 1000);
}

inline EnemyStatModifier ComposeEnemyStatModifiers(
    EnemyStatModifier current,
    const EnemyStatModifier& next) noexcept {
    current.healthPermille = std::max<std::int32_t>(
        1, ScaleI32(current.healthPermille, next.healthPermille));
    current.armorAdd = ClampI32(
        static_cast<std::int64_t>(current.armorAdd) + next.armorAdd);
    current.damagePermille = std::max<std::int32_t>(
        1, ScaleI32(current.damagePermille, next.damagePermille));
    current.speedPermille = std::max<std::int32_t>(
        1, ScaleI32(current.speedPermille, next.speedPermille));
    current.bountyPermille = std::max<std::int32_t>(
        1, ScaleI32(current.bountyPermille, next.bountyPermille));
    return current;
}

inline gameplay::CombatStats ApplyEnemyStatModifier(
    gameplay::CombatStats value,
    const EnemyStatModifier& modifier) noexcept {
    value.maximumHealth = std::max<std::int32_t>(
        1, ScaleI32(value.maximumHealth, modifier.healthPermille));
    value.armor = ClampI32(
        static_cast<std::int64_t>(value.armor) + modifier.armorAdd);
    value.weaponDamage = std::max<std::int32_t>(
        1, ScaleI32(value.weaponDamage, modifier.damagePermille));
    value.bounty = std::max<std::int32_t>(
        0, ScaleI32(value.bounty, modifier.bountyPermille));
    return value;
}

inline std::int32_t ApplySpeedModifier(
    std::int32_t cellsPerTick,
    const EnemyStatModifier& modifier) noexcept {
    if (cellsPerTick <= 0) return 0;
    return std::max<std::int32_t>(
        1, ScaleI32(cellsPerTick, modifier.speedPermille));
}

} // namespace rts::tower_defense
