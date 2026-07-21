#pragma once

#include <RTSEngine/Rts/Navigation.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/foundation/Random.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace rts::tower_defense {

using LaneId = std::uint32_t;
using WaveId = std::uint32_t;
using RewardId = std::uint32_t;

struct SpawnLane {
    LaneId id{};
    gameplay::GridPoint spawn{};
    gameplay::GridPoint goal{};
    std::uint32_t weight{1};
};

struct WaveEnemyEntry {
    std::uint32_t unitDefinitionId{};
    std::uint32_t budgetCost{1};
    std::uint32_t weight{1};
    std::uint32_t maxPerWave{};
};

struct RewardDefinition {
    RewardId id{};
    std::uint32_t weight{1};
    std::int32_t resourceGrant{};
};

struct WaveDefinition {
    WaveId id{};
    std::uint32_t budget{};
    std::uint32_t spawnIntervalTicks{1};
    std::uint32_t enemyTeamId{2};
    std::vector<LaneId> laneIds;
    std::vector<WaveEnemyEntry> enemies;
    std::vector<RewardId> rewardPool;
    std::uint32_t rewardChoices{3};
};

struct PlannedSpawn {
    std::uint32_t sequence{};
    std::uint64_t tickOffset{};
    LaneId laneId{};
    gameplay::GridPoint spawn{};
    gameplay::GridPoint goal{};
    std::uint32_t unitDefinitionId{};

    friend bool operator==(const PlannedSpawn& a,
                           const PlannedSpawn& b) noexcept {
        return a.sequence == b.sequence &&
               a.tickOffset == b.tickOffset &&
               a.laneId == b.laneId &&
               a.spawn == b.spawn &&
               a.goal == b.goal &&
               a.unitDefinitionId == b.unitDefinitionId;
    }
};

struct WavePlan {
    WaveId waveId{};
    std::uint32_t enemyTeamId{};
    std::uint32_t unusedBudget{};
    std::uint32_t rewardChoices{};
    std::vector<RewardDefinition> rewards;
    std::vector<PlannedSpawn> spawns;
};

enum class WavePhase : std::uint8_t {
    Idle,
    Spawning,
    Active,
    RewardPending,
    Complete,
    Failed
};

enum class WaveStartFailure : std::uint8_t {
    None,
    UnknownWave,
    WaveAlreadyActive,
    RewardPending,
    InvalidDefinition,
    MissingLane,
    NoAffordableEnemy,
    UnknownUnitDefinition,
    NoBaseCore,
    InvalidLane
};

struct WaveStartResult {
    bool accepted{};
    WaveStartFailure failure{WaveStartFailure::None};
    WaveId waveId{};
};

struct WaveState {
    WaveId waveId{};
    WavePhase phase{WavePhase::Idle};
    std::uint64_t startedTick{};
    std::uint32_t nextSpawn{};
    std::uint32_t spawned{};
    std::uint32_t resolved{};
};

struct RewardOffer {
    WaveId waveId{};
    std::vector<RewardId> choices;
    RewardId selected{};
    bool chosen{};
};

class WaveDirector {
public:
    explicit WaveDirector(std::uint64_t rootSeed = 1) noexcept
        : rootSeed_(rootSeed) {}

    bool registerLane(SpawnLane lane) {
        if (lane.id == 0 || lane.weight == 0) return false;
        replaceById(lanes_, lane);
        return true;
    }

    bool registerWave(WaveDefinition wave) {
        if (wave.id == 0) return false;
        replaceById(waves_, std::move(wave));
        return true;
    }

    bool registerReward(RewardDefinition reward) {
        if (reward.id == 0 || reward.weight == 0) return false;
        replaceById(rewards_, reward);
        return true;
    }

    const SpawnLane* lane(LaneId id) const noexcept {
        return findById(lanes_, id);
    }

    const WaveDefinition* definition(WaveId id) const noexcept {
        return findById(waves_, id);
    }

    const RewardDefinition* reward(RewardId id) const noexcept {
        const auto* planned = findById(plan_.rewards, id);
        return planned ? planned : findById(rewards_, id);
    }

    const std::vector<SpawnLane>& lanes() const noexcept {
        return lanes_;
    }

    const WavePlan& plan() const noexcept { return plan_; }
    const WaveState& state() const noexcept { return state_; }
    const RewardOffer& offer() const noexcept { return offer_; }

    std::uint32_t activeEnemies() const noexcept {
        return state_.spawned >= state_.resolved
            ? state_.spawned - state_.resolved
            : 0;
    }

    WaveStartResult begin(WaveId id, std::uint64_t tick) {
        if (state_.phase == WavePhase::Spawning ||
            state_.phase == WavePhase::Active) {
            return {false, WaveStartFailure::WaveAlreadyActive, id};
        }
        if (state_.phase == WavePhase::RewardPending) {
            return {false, WaveStartFailure::RewardPending, id};
        }

        const auto* wave = definition(id);
        if (!wave) {
            return {false, WaveStartFailure::UnknownWave, id};
        }
        if (wave->budget == 0 || wave->enemies.empty()) {
            return {false, WaveStartFailure::InvalidDefinition, id};
        }

        std::vector<SpawnLane> activeLanes;
        if (wave->laneIds.empty()) {
            activeLanes = lanes_;
        } else {
            for (const auto laneId : wave->laneIds) {
                const auto* value = lane(laneId);
                if (!value) {
                    return {false, WaveStartFailure::MissingLane, id};
                }
                activeLanes.push_back(*value);
            }
            std::sort(activeLanes.begin(), activeLanes.end(),
                      idLess<SpawnLane>);
            activeLanes.erase(
                std::unique(activeLanes.begin(), activeLanes.end(),
                            [](const SpawnLane& a, const SpawnLane& b) {
                                return a.id == b.id;
                            }),
                activeLanes.end());
        }
        if (activeLanes.empty()) {
            return {false, WaveStartFailure::MissingLane, id};
        }

        std::uint64_t laneWeight = 0;
        for (const auto& value : activeLanes) {
            laneWeight += value.weight;
        }
        if (laneWeight == 0 ||
            laneWeight > std::numeric_limits<std::uint32_t>::max()) {
            return {false, WaveStartFailure::InvalidDefinition, id};
        }

        std::vector<WaveEnemyEntry> pool = wave->enemies;
        std::sort(pool.begin(), pool.end(),
                  [](const WaveEnemyEntry& a,
                     const WaveEnemyEntry& b) {
                      return a.unitDefinitionId < b.unitDefinitionId;
                  });
        for (const auto& entry : pool) {
            if (entry.unitDefinitionId == 0 ||
                entry.budgetCost == 0 || entry.weight == 0) {
                return {false, WaveStartFailure::InvalidDefinition, id};
            }
        }

        WavePlan nextPlan;
        nextPlan.waveId = id;
        nextPlan.enemyTeamId = wave->enemyTeamId;
        nextPlan.rewardChoices = wave->rewardChoices;
        nextPlan.rewards = resolveRewards(*wave);

        std::uint64_t rewardWeight = 0;
        for (const auto& reward : nextPlan.rewards) {
            rewardWeight += reward.weight;
        }
        if (rewardWeight >
            std::numeric_limits<std::uint32_t>::max()) {
            return {false, WaveStartFailure::InvalidDefinition, id};
        }

        foundation::RandomStream random(
            rootSeed_,
            scopedStreamId("tower-defense.wave-plan", id));
        std::vector<std::uint32_t> counts(pool.size(), 0);
        std::uint32_t remaining = wave->budget;
        std::uint32_t sequence = 0;
        const auto interval =
            std::max<std::uint32_t>(1, wave->spawnIntervalTicks);

        for (;;) {
            std::vector<std::size_t> affordable;
            std::uint64_t totalWeight = 0;
            for (std::size_t index = 0;
                 index < pool.size(); ++index) {
                const auto& entry = pool[index];
                const bool belowMaximum =
                    entry.maxPerWave == 0 ||
                    counts[index] < entry.maxPerWave;
                if (belowMaximum &&
                    entry.budgetCost <= remaining) {
                    affordable.push_back(index);
                    totalWeight += entry.weight;
                }
            }
            if (affordable.empty()) break;
            if (totalWeight == 0 ||
                totalWeight >
                    std::numeric_limits<std::uint32_t>::max()) {
                return {false,
                        WaveStartFailure::InvalidDefinition,
                        id};
            }

            const auto selectedIndex = selectWeighted(
                random,
                affordable,
                pool,
                [](const WaveEnemyEntry& entry) {
                    return entry.weight;
                });
            const auto laneIndex =
                selectWeightedLane(random, activeLanes);
            const auto& entry = pool[selectedIndex];
            const auto& selectedLane = activeLanes[laneIndex];

            nextPlan.spawns.push_back(
                {sequence,
                 static_cast<std::uint64_t>(sequence) * interval,
                 selectedLane.id,
                 selectedLane.spawn,
                 selectedLane.goal,
                 entry.unitDefinitionId});
            remaining -= entry.budgetCost;
            ++counts[selectedIndex];
            ++sequence;
        }

        if (nextPlan.spawns.empty()) {
            return {false, WaveStartFailure::NoAffordableEnemy, id};
        }

        nextPlan.unusedBudget = remaining;
        plan_ = std::move(nextPlan);
        state_ = {id, WavePhase::Spawning, tick, 0, 0, 0};
        offer_ = {};
        return {true, WaveStartFailure::None, id};
    }

    std::vector<PlannedSpawn> dueSpawns(std::uint64_t tick) {
        std::vector<PlannedSpawn> due;
        if (state_.phase != WavePhase::Spawning &&
            state_.phase != WavePhase::Active) {
            return due;
        }

        while (state_.nextSpawn < plan_.spawns.size()) {
            const auto& spawn = plan_.spawns[state_.nextSpawn];
            if (state_.startedTick + spawn.tickOffset > tick) {
                break;
            }
            due.push_back(spawn);
            ++state_.nextSpawn;
            ++state_.spawned;
        }
        if (state_.nextSpawn == plan_.spawns.size()) {
            state_.phase = WavePhase::Active;
        }
        return due;
    }

    bool markEnemyResolved() {
        if (state_.phase != WavePhase::Spawning &&
            state_.phase != WavePhase::Active) {
            return false;
        }
        if (state_.resolved >= state_.spawned) return false;
        ++state_.resolved;
        return finishIfReady();
    }

    bool fail() noexcept {
        if (state_.phase == WavePhase::Idle ||
            state_.phase == WavePhase::Complete ||
            state_.phase == WavePhase::Failed) {
            return false;
        }
        state_.phase = WavePhase::Failed;
        offer_ = {};
        return true;
    }

    const RewardDefinition* chooseReward(RewardId id) {
        if (state_.phase != WavePhase::RewardPending ||
            offer_.chosen) {
            return nullptr;
        }
        if (std::find(offer_.choices.begin(),
                      offer_.choices.end(), id) ==
            offer_.choices.end()) {
            return nullptr;
        }
        const auto* selected = findById(plan_.rewards, id);
        if (!selected) return nullptr;
        offer_.selected = id;
        offer_.chosen = true;
        state_.phase = WavePhase::Complete;
        return selected;
    }

    void appendHash(
        foundation::CanonicalHash& hash) const noexcept {
        hash.WriteU64(rootSeed_);
        hash.WriteU32(state_.waveId);
        hash.WriteU8(
            static_cast<std::uint8_t>(state_.phase));
        hash.WriteU64(state_.startedTick);
        hash.WriteU32(state_.nextSpawn);
        hash.WriteU32(state_.spawned);
        hash.WriteU32(state_.resolved);

        hash.WriteU32(plan_.waveId);
        hash.WriteU32(plan_.enemyTeamId);
        hash.WriteU32(plan_.unusedBudget);
        hash.WriteU32(plan_.rewardChoices);
        hash.WriteU32(
            static_cast<std::uint32_t>(plan_.spawns.size()));
        for (const auto& spawn : plan_.spawns) {
            hash.WriteU32(spawn.sequence);
            hash.WriteU64(spawn.tickOffset);
            hash.WriteU32(spawn.laneId);
            hash.WriteI32(spawn.spawn.x);
            hash.WriteI32(spawn.spawn.y);
            hash.WriteI32(spawn.goal.x);
            hash.WriteI32(spawn.goal.y);
            hash.WriteU32(spawn.unitDefinitionId);
        }
        hash.WriteU32(
            static_cast<std::uint32_t>(plan_.rewards.size()));
        for (const auto& value : plan_.rewards) {
            hash.WriteU32(value.id);
            hash.WriteU32(value.weight);
            hash.WriteI32(value.resourceGrant);
        }

        hash.WriteU32(offer_.waveId);
        hash.WriteBool(offer_.chosen);
        hash.WriteU32(offer_.selected);
        hash.WriteU32(
            static_cast<std::uint32_t>(offer_.choices.size()));
        for (const auto choice : offer_.choices) {
            hash.WriteU32(choice);
        }
    }

private:
    template<class T>
    static bool idLess(const T& a, const T& b) noexcept {
        return a.id < b.id;
    }

    template<class T>
    static const T* findById(const std::vector<T>& values,
                             std::uint32_t id) noexcept {
        const auto iterator = std::lower_bound(
            values.begin(), values.end(), id,
            [](const T& value, std::uint32_t key) {
                return value.id < key;
            });
        return iterator != values.end() && iterator->id == id
            ? &*iterator
            : nullptr;
    }

    template<class T>
    static void replaceById(std::vector<T>& values, T value) {
        const auto iterator = std::lower_bound(
            values.begin(), values.end(), value.id,
            [](const T& current, std::uint32_t key) {
                return current.id < key;
            });
        if (iterator != values.end() &&
            iterator->id == value.id) {
            *iterator = std::move(value);
        } else {
            values.insert(iterator, std::move(value));
        }
    }

    static foundation::RandomStreamId scopedStreamId(
        std::string_view name,
        std::uint64_t scope) noexcept {
        foundation::CanonicalHash hash;
        hash.WriteString(name);
        hash.WriteU64(scope);
        return hash.Value();
    }

    template<class Entry, class Weight>
    static std::size_t selectWeighted(
        foundation::RandomStream& random,
        const std::vector<std::size_t>& indices,
        const std::vector<Entry>& entries,
        Weight weight) {
        std::uint32_t total = 0;
        for (const auto index : indices) {
            total += weight(entries[index]);
        }
        auto roll = random.NextBounded(total);
        for (const auto index : indices) {
            const auto current = weight(entries[index]);
            if (roll < current) return index;
            roll -= current;
        }
        return indices.back();
    }

    static std::size_t selectWeightedLane(
        foundation::RandomStream& random,
        const std::vector<SpawnLane>& lanes) {
        std::uint32_t total = 0;
        for (const auto& lane : lanes) {
            total += lane.weight;
        }
        auto roll = random.NextBounded(total);
        for (std::size_t index = 0;
             index < lanes.size(); ++index) {
            if (roll < lanes[index].weight) return index;
            roll -= lanes[index].weight;
        }
        return lanes.size() - 1;
    }

    std::vector<RewardDefinition> resolveRewards(
        const WaveDefinition& wave) const {
        std::vector<RewardDefinition> result;
        if (wave.rewardPool.empty()) {
            result = rewards_;
        } else {
            for (const auto id : wave.rewardPool) {
                const auto* value = findById(rewards_, id);
                if (value) result.push_back(*value);
            }
            std::sort(result.begin(), result.end(),
                      idLess<RewardDefinition>);
            result.erase(
                std::unique(result.begin(), result.end(),
                            [](const RewardDefinition& a,
                               const RewardDefinition& b) {
                                return a.id == b.id;
                            }),
                result.end());
        }
        return result;
    }

    bool finishIfReady() {
        if (state_.nextSpawn != plan_.spawns.size() ||
            state_.resolved != state_.spawned ||
            state_.spawned == 0) {
            return false;
        }

        buildRewardOffer();
        state_.phase = offer_.choices.empty()
            ? WavePhase::Complete
            : WavePhase::RewardPending;
        return true;
    }

    void buildRewardOffer() {
        offer_ = {};
        offer_.waveId = state_.waveId;
        if (plan_.rewardChoices == 0 ||
            plan_.rewards.empty()) {
            return;
        }

        std::vector<RewardDefinition> candidates =
            plan_.rewards;
        foundation::RandomStream random(
            rootSeed_,
            scopedStreamId("tower-defense.wave-reward",
                           state_.waveId));
        const auto count = std::min<std::size_t>(
            plan_.rewardChoices, candidates.size());
        for (std::size_t choice = 0;
             choice < count; ++choice) {
            std::uint32_t total = 0;
            for (const auto& candidate : candidates) {
                total += candidate.weight;
            }
            auto roll = random.NextBounded(total);
            std::size_t selected = 0;
            for (; selected < candidates.size(); ++selected) {
                if (roll < candidates[selected].weight) break;
                roll -= candidates[selected].weight;
            }
            if (selected == candidates.size()) {
                selected = candidates.size() - 1;
            }
            offer_.choices.push_back(candidates[selected].id);
            candidates.erase(
                candidates.begin() +
                static_cast<std::ptrdiff_t>(selected));
        }
    }

    std::uint64_t rootSeed_{1};
    std::vector<SpawnLane> lanes_;
    std::vector<WaveDefinition> waves_;
    std::vector<RewardDefinition> rewards_;
    WavePlan plan_;
    WaveState state_;
    RewardOffer offer_;
};

} // namespace rts::tower_defense
