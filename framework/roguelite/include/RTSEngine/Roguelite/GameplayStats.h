#pragma once

#include <RTSEngine/Roguelite/ModifierRuntime.h>
#include <RTSEngine/Rts/GameplayModifiers.h>

namespace rts::roguelite {

inline StatId PlayerUnitHealthStat() noexcept {
    static const StatId value = MakeStatId("player.unit.maximum-health");
    return value;
}

inline StatId PlayerUnitDamageStat() noexcept {
    static const StatId value = MakeStatId("player.unit.weapon-damage");
    return value;
}

inline StatId UnitDamageStat() noexcept {
    return PlayerUnitDamageStat();
}

inline StatId PlayerUnitArmorStat() noexcept {
    static const StatId value = MakeStatId("player.unit.armor-add");
    return value;
}

inline StatId PlayerUnitMoveSpeedStat() noexcept {
    static const StatId value = MakeStatId("player.unit.move-speed");
    return value;
}

inline StatId PlayerBuildingHealthStat() noexcept {
    static const StatId value = MakeStatId("player.building.maximum-health");
    return value;
}

inline StatId PlayerBuildingDamageStat() noexcept {
    static const StatId value = MakeStatId("player.building.weapon-damage");
    return value;
}

inline StatId PlayerConstructionSpeedStat() noexcept {
    static const StatId value = MakeStatId("player.construction.speed");
    return value;
}

inline StatId PlayerProductionSpeedStat() noexcept {
    static const StatId value = MakeStatId("player.production.speed");
    return value;
}

inline StatId PlayerBountyMultiplierStat() noexcept {
    static const StatId value = MakeStatId("player.bounty.multiplier");
    return value;
}

inline gameplay::TeamModifierProfile ResolveGameplayProfile(
    const ModifierRuntime& modifiers) noexcept {
    gameplay::TeamModifierProfile result;
    result.unitHealth = modifiers.resolve(
        PlayerUnitHealthStat(), gameplay::kGameplayModifierScale);
    result.unitDamage = modifiers.resolve(
        PlayerUnitDamageStat(), gameplay::kGameplayModifierScale);
    result.unitArmorAdd = modifiers.resolve(PlayerUnitArmorStat(), 0);
    result.unitMoveSpeed = modifiers.resolve(
        PlayerUnitMoveSpeedStat(), gameplay::kGameplayModifierScale);
    result.buildingHealth = modifiers.resolve(
        PlayerBuildingHealthStat(), gameplay::kGameplayModifierScale);
    result.buildingDamage = modifiers.resolve(
        PlayerBuildingDamageStat(), gameplay::kGameplayModifierScale);
    result.constructionSpeed = modifiers.resolve(
        PlayerConstructionSpeedStat(), gameplay::kGameplayModifierScale);
    result.productionSpeed = modifiers.resolve(
        PlayerProductionSpeedStat(), gameplay::kGameplayModifierScale);
    result.bountyMultiplier = modifiers.resolve(
        PlayerBountyMultiplierStat(), gameplay::kGameplayModifierScale);
    return gameplay::SanitizeTeamModifierProfile(result);
}

} // namespace rts::roguelite
