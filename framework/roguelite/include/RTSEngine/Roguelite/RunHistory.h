#pragma once

#include <RTSEngine/Roguelite/ModifierRuntime.h>
#include <RTSEngine/Roguelite/RewardRarity.h>
#include <RTSEngine/TowerDefense/WaveDirector.h>

#include <cstdint>
#include <vector>

namespace rts::roguelite {

using RunId = std::uint32_t;

enum class RunHistoryPhase : std::uint8_t {
    Idle,
    Active,
    Complete,
    Failed
};

enum class WaveResultPhase : std::uint8_t {
    Active,
    RewardPending,
    Complete,
    Failed
};

struct WaveResult final {
    tower_defense::WaveId waveId{};
    std::uint32_t waveIndex{};
    std::uint64_t startedTick{};
    std::uint64_t completedTick{};
    WaveResultPhase phase{WaveResultPhase::Active};
    std::uint32_t plannedEnemies{};
    std::uint32_t plannedBosses{};
    std::uint32_t enemiesDefeated{};
    std::uint32_t bossesDefeated{};
    std::int32_t coreHealthStart{};
    std::int32_t coreHealthEnd{};
    std::int32_t coreHealthMaximum{};
    std::int32_t resourcesStart{};
    std::int32_t resourcesEnd{};
    std::int32_t resourceDelta{};
    std::int32_t resourceBonus{};
    std::vector<tower_defense::WaveAffixId> affixes;
    std::vector<tower_defense::BossId> bosses;
    std::vector<ModifierId> rewardChoices;
    ModifierId selectedModifier{};
    bool modifierApplied{};

    std::uint32_t rewardRarityBudget{};
    std::uint32_t rewardRaritySpent{};
    RewardRarity guaranteedRarity{RewardRarity::Common};
    RewardRarity effectiveGuaranteedRarity{RewardRarity::Common};
    std::uint32_t pityBefore{};
    std::uint32_t pityAfter{};
    bool pityTriggered{};
    std::vector<RewardRarity> rewardRarities;

    friend bool operator==(const WaveResult& a,
                           const WaveResult& b) noexcept {
        return a.waveId == b.waveId &&
               a.waveIndex == b.waveIndex &&
               a.startedTick == b.startedTick &&
               a.completedTick == b.completedTick &&
               a.phase == b.phase &&
               a.plannedEnemies == b.plannedEnemies &&
               a.plannedBosses == b.plannedBosses &&
               a.enemiesDefeated == b.enemiesDefeated &&
               a.bossesDefeated == b.bossesDefeated &&
               a.coreHealthStart == b.coreHealthStart &&
               a.coreHealthEnd == b.coreHealthEnd &&
               a.coreHealthMaximum == b.coreHealthMaximum &&
               a.resourcesStart == b.resourcesStart &&
               a.resourcesEnd == b.resourcesEnd &&
               a.resourceDelta == b.resourceDelta &&
               a.resourceBonus == b.resourceBonus &&
               a.affixes == b.affixes &&
               a.bosses == b.bosses &&
               a.rewardChoices == b.rewardChoices &&
               a.selectedModifier == b.selectedModifier &&
               a.modifierApplied == b.modifierApplied &&
               a.rewardRarityBudget == b.rewardRarityBudget &&
               a.rewardRaritySpent == b.rewardRaritySpent &&
               a.guaranteedRarity == b.guaranteedRarity &&
               a.effectiveGuaranteedRarity ==
                   b.effectiveGuaranteedRarity &&
               a.pityBefore == b.pityBefore &&
               a.pityAfter == b.pityAfter &&
               a.pityTriggered == b.pityTriggered &&
               a.rewardRarities == b.rewardRarities;
    }
};

struct RunHistory final {
    RunId runId{};
    std::uint64_t startedTick{};
    std::uint64_t finishedTick{};
    RunHistoryPhase phase{RunHistoryPhase::Idle};
    bool legacyImported{};
    std::uint32_t rewardPityMisses{};
    std::vector<WaveResult> waves;

    friend bool operator==(const RunHistory& a,
                           const RunHistory& b) noexcept {
        return a.runId == b.runId &&
               a.startedTick == b.startedTick &&
               a.finishedTick == b.finishedTick &&
               a.phase == b.phase &&
               a.legacyImported == b.legacyImported &&
               a.rewardPityMisses == b.rewardPityMisses &&
               a.waves == b.waves;
    }
};

} // namespace rts::roguelite
