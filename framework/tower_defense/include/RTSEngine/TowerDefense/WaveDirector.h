#pragma once

#include <RTSEngine/TowerDefense/LaneGraph.h>
#include <RTSEngine/TowerDefense/WaveModifiers.h>
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
    LaneNodeId startNodeId{};
    LaneNodeId goalNodeId{};

    bool usesGraph() const noexcept {
        return startNodeId != 0 || goalNodeId != 0;
    }
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
    std::vector<BossId> bossPool;
    std::uint32_t bossCount{};
    std::vector<WaveAffixId> affixPool;
    std::uint32_t affixChoices{};
    std::vector<RewardId> rewardPool;
    std::uint32_t rewardChoices{3};
};

struct PlannedLaneRoute final {
    LaneId id{};
    std::uint32_t totalCost{};
    std::vector<LaneNodeId> nodeIds;
    std::vector<gameplay::GridPoint> points;

    friend bool operator==(const PlannedLaneRoute& a,
                           const PlannedLaneRoute& b) noexcept {
        return a.id == b.id &&
               a.totalCost == b.totalCost &&
               a.nodeIds == b.nodeIds &&
               a.points == b.points;
    }
};

struct PlannedSpawn {
    std::uint32_t sequence{};
    std::uint64_t tickOffset{};
    LaneId laneId{};
    gameplay::GridPoint spawn{};
    gameplay::GridPoint goal{};
    std::uint32_t unitDefinitionId{};
    BossId bossId{};
    EnemyStatModifier modifier{};

    friend bool operator==(const PlannedSpawn& a,
                           const PlannedSpawn& b) noexcept {
        return a.sequence == b.sequence &&
               a.tickOffset == b.tickOffset &&
               a.laneId == b.laneId &&
               a.spawn == b.spawn &&
               a.goal == b.goal &&
               a.unitDefinitionId == b.unitDefinitionId &&
               a.bossId == b.bossId &&
               a.modifier == b.modifier;
    }
};

struct WavePlan {
    WaveId waveId{};
    std::uint32_t enemyTeamId{};
    std::uint32_t unusedBudget{};
    std::uint32_t rewardChoices{};
    std::vector<RewardDefinition> rewards;
    std::vector<WaveAffixDefinition> affixes;
    std::vector<PlannedLaneRoute> routes;
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
    InvalidLane,
    InvalidLaneRoute,
    UnknownBoss,
    UnknownAffix,
    NoAffordableBoss
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

    bool upsertLaneNode(LaneNode node) {
        return laneGraph_.upsertNode(node);
    }

    bool removeLaneNode(LaneNodeId id) {
        return laneGraph_.removeNode(id);
    }

    bool connectLaneNodes(
        LaneNodeId from,
        LaneNodeId to,
        std::uint32_t cost = 1) {
        return laneGraph_.connect(from, to, cost);
    }

    bool connectLaneNodesBidirectional(
        LaneNodeId first,
        LaneNodeId second,
        std::uint32_t cost = 1) {
        return laneGraph_.connectBidirectional(first, second, cost);
    }

    bool disconnectLaneNodes(LaneNodeId from, LaneNodeId to) {
        return laneGraph_.disconnect(from, to);
    }

    bool setLaneConnectionEnabled(
        LaneNodeId from,
        LaneNodeId to,
        bool enabled) {
        return laneGraph_.setConnectionEnabled(from, to, enabled);
    }

    void replaceLaneGraph(LaneGraph graph) {
        laneGraph_ = std::move(graph);
    }

    const LaneGraph& laneGraph() const noexcept { return laneGraph_; }

    bool registerLane(SpawnLane lane) {
        if (lane.id == 0 || lane.weight == 0 ||
            ((lane.startNodeId == 0) != (lane.goalNodeId == 0))) {
            return false;
        }
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

    bool registerAffix(WaveAffixDefinition affix) {
        if (affix.id == 0 || affix.weight == 0 || !affix.modifier.valid()) {
            return false;
        }
        replaceById(affixes_, std::move(affix));
        return true;
    }

    bool registerBoss(BossDefinition boss) {
        if (boss.id == 0 || boss.unitDefinitionId == 0 ||
            boss.budgetCost == 0 || boss.weight == 0 ||
            !boss.modifier.valid()) {
            return false;
        }
        replaceById(bosses_, std::move(boss));
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

    const WaveAffixDefinition* affix(WaveAffixId id) const noexcept {
        return findById(affixes_, id);
    }

    const BossDefinition* boss(BossId id) const noexcept {
        return findById(bosses_, id);
    }

    const PlannedLaneRoute* plannedRoute(LaneId id) const noexcept {
        return findById(plan_.routes, id);
    }

    bool resolveLaneRoute(
        LaneId id,
        PlannedLaneRoute& output) const {
        const auto* value = lane(id);
        return value && buildLaneRoute(*value, output);
    }

    const std::vector<SpawnLane>& lanes() const noexcept { return lanes_; }
    const std::vector<WaveAffixDefinition>& affixes() const noexcept {
        return affixes_;
    }
    const std::vector<BossDefinition>& bosses() const noexcept {
        return bosses_;
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
        if (!wave) return {false, WaveStartFailure::UnknownWave, id};
        if (wave->budget == 0 ||
            (wave->enemies.empty() && wave->bossCount == 0)) {
            return {false, WaveStartFailure::InvalidDefinition, id};
        }

        std::vector<SpawnLane> activeLanes;
        if (!resolveActiveLanes(*wave, activeLanes)) {
            return {false, WaveStartFailure::MissingLane, id};
        }
        std::uint64_t laneWeight = 0;
        for (const auto& value : activeLanes) laneWeight += value.weight;
        if (laneWeight == 0 ||
            laneWeight > std::numeric_limits<std::uint32_t>::max()) {
            return {false, WaveStartFailure::InvalidDefinition, id};
        }

        std::vector<WaveEnemyEntry> enemyPool = wave->enemies;
        std::sort(enemyPool.begin(), enemyPool.end(),
                  [](const WaveEnemyEntry& a, const WaveEnemyEntry& b) {
                      return a.unitDefinitionId < b.unitDefinitionId;
                  });
        for (const auto& entry : enemyPool) {
            if (entry.unitDefinitionId == 0 || entry.budgetCost == 0 ||
                entry.weight == 0) {
                return {false, WaveStartFailure::InvalidDefinition, id};
            }
        }

        WavePlan nextPlan;
        nextPlan.waveId = id;
        nextPlan.enemyTeamId = wave->enemyTeamId;
        nextPlan.rewardChoices = wave->rewardChoices;
        nextPlan.rewards = resolveRewards(*wave);
        nextPlan.routes.reserve(activeLanes.size());
        for (const auto& laneDefinition : activeLanes) {
            PlannedLaneRoute route;
            if (!buildLaneRoute(laneDefinition, route)) {
                return {false, WaveStartFailure::InvalidLaneRoute, id};
            }
            nextPlan.routes.push_back(std::move(route));
        }

        if (!selectAffixes(*wave, nextPlan.affixes)) {
            return {false, WaveStartFailure::UnknownAffix, id};
        }
        EnemyStatModifier waveModifier;
        for (const auto& selected : nextPlan.affixes) {
            waveModifier = ComposeEnemyStatModifiers(
                waveModifier, selected.modifier);
        }

        std::uint32_t remaining = wave->budget;
        std::uint32_t sequence = 0;
        const auto interval =
            std::max<std::uint32_t>(1, wave->spawnIntervalTicks);

        std::vector<BossDefinition> selectedBosses;
        const auto bossResult = selectBosses(*wave, remaining, selectedBosses);
        if (bossResult != WaveStartFailure::None) {
            return {false, bossResult, id};
        }

        foundation::RandomStream bossLaneRandom(
            rootSeed_, scopedStreamId("tower-defense.boss-lane", id));
        for (const auto& selected : selectedBosses) {
            const auto laneIndex = selectWeightedLane(
                bossLaneRandom, activeLanes);
            const auto& route = nextPlan.routes[laneIndex];
            nextPlan.spawns.push_back(
                {sequence,
                 static_cast<std::uint64_t>(sequence) * interval,
                 activeLanes[laneIndex].id,
                 route.points.front(),
                 route.points.back(),
                 selected.unitDefinitionId,
                 selected.id,
                 ComposeEnemyStatModifiers(waveModifier, selected.modifier)});
            ++sequence;
        }

        foundation::RandomStream enemyRandom(
            rootSeed_, scopedStreamId("tower-defense.wave-plan", id));
        std::vector<std::uint32_t> counts(enemyPool.size(), 0);
        for (;;) {
            std::vector<std::size_t> affordable;
            std::uint64_t totalWeight = 0;
            for (std::size_t index = 0; index < enemyPool.size(); ++index) {
                const auto& entry = enemyPool[index];
                const bool belowMaximum = entry.maxPerWave == 0 ||
                    counts[index] < entry.maxPerWave;
                if (belowMaximum && entry.budgetCost <= remaining) {
                    affordable.push_back(index);
                    totalWeight += entry.weight;
                }
            }
            if (affordable.empty()) break;
            if (totalWeight == 0 ||
                totalWeight > std::numeric_limits<std::uint32_t>::max()) {
                return {false, WaveStartFailure::InvalidDefinition, id};
            }

            const auto selectedIndex = selectWeighted(
                enemyRandom, affordable, enemyPool,
                [](const WaveEnemyEntry& entry) { return entry.weight; });
            const auto laneIndex = selectWeightedLane(enemyRandom, activeLanes);
            const auto& entry = enemyPool[selectedIndex];
            const auto& route = nextPlan.routes[laneIndex];
            nextPlan.spawns.push_back(
                {sequence,
                 static_cast<std::uint64_t>(sequence) * interval,
                 activeLanes[laneIndex].id,
                 route.points.front(),
                 route.points.back(),
                 entry.unitDefinitionId,
                 0,
                 waveModifier});
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
            if (state_.startedTick + spawn.tickOffset > tick) break;
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
        if (state_.phase != WavePhase::RewardPending || offer_.chosen) {
            return nullptr;
        }
        if (std::find(offer_.choices.begin(), offer_.choices.end(), id) ==
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

    void appendHash(foundation::CanonicalHash& hash) const noexcept {
        hash.WriteU64(rootSeed_);
        hash.WriteU32(state_.waveId);
        hash.WriteU8(static_cast<std::uint8_t>(state_.phase));
        hash.WriteU64(state_.startedTick);
        hash.WriteU32(state_.nextSpawn);
        hash.WriteU32(state_.spawned);
        hash.WriteU32(state_.resolved);

        hash.WriteU32(plan_.waveId);
        hash.WriteU32(plan_.enemyTeamId);
        hash.WriteU32(plan_.unusedBudget);
        hash.WriteU32(plan_.rewardChoices);
        hash.WriteU32(static_cast<std::uint32_t>(plan_.affixes.size()));
        for (const auto& value : plan_.affixes) hashAffix(hash, value);
        hash.WriteU32(static_cast<std::uint32_t>(plan_.routes.size()));
        for (const auto& route : plan_.routes) {
            hash.WriteU32(route.id);
            hash.WriteU32(route.totalCost);
            hash.WriteU32(static_cast<std::uint32_t>(route.nodeIds.size()));
            for (const auto nodeId : route.nodeIds) hash.WriteU32(nodeId);
            hash.WriteU32(static_cast<std::uint32_t>(route.points.size()));
            for (const auto point : route.points) {
                hash.WriteI32(point.x);
                hash.WriteI32(point.y);
            }
        }
        hash.WriteU32(static_cast<std::uint32_t>(plan_.spawns.size()));
        for (const auto& spawn : plan_.spawns) {
            hash.WriteU32(spawn.sequence);
            hash.WriteU64(spawn.tickOffset);
            hash.WriteU32(spawn.laneId);
            hash.WriteI32(spawn.spawn.x);
            hash.WriteI32(spawn.spawn.y);
            hash.WriteI32(spawn.goal.x);
            hash.WriteI32(spawn.goal.y);
            hash.WriteU32(spawn.unitDefinitionId);
            hash.WriteU32(spawn.bossId);
            hashModifier(hash, spawn.modifier);
        }
        hash.WriteU32(static_cast<std::uint32_t>(plan_.rewards.size()));
        for (const auto& value : plan_.rewards) {
            hash.WriteU32(value.id);
            hash.WriteU32(value.weight);
            hash.WriteI32(value.resourceGrant);
        }

        hash.WriteU32(offer_.waveId);
        hash.WriteBool(offer_.chosen);
        hash.WriteU32(offer_.selected);
        hash.WriteU32(static_cast<std::uint32_t>(offer_.choices.size()));
        for (const auto choice : offer_.choices) hash.WriteU32(choice);
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
            ? &*iterator : nullptr;
    }

    template<class T>
    static void replaceById(std::vector<T>& values, T value) {
        const auto iterator = std::lower_bound(
            values.begin(), values.end(), value.id,
            [](const T& current, std::uint32_t key) {
                return current.id < key;
            });
        if (iterator != values.end() && iterator->id == value.id) {
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

    static void hashModifier(
        foundation::CanonicalHash& hash,
        const EnemyStatModifier& value) noexcept {
        hash.WriteI32(value.healthPermille);
        hash.WriteI32(value.armorAdd);
        hash.WriteI32(value.damagePermille);
        hash.WriteI32(value.speedPermille);
        hash.WriteI32(value.bountyPermille);
    }

    static void hashAffix(
        foundation::CanonicalHash& hash,
        const WaveAffixDefinition& value) noexcept {
        hash.WriteU32(value.id);
        hash.WriteU32(value.weight);
        hashModifier(hash, value.modifier);
    }

    bool resolveActiveLanes(
        const WaveDefinition& wave,
        std::vector<SpawnLane>& output) const {
        output.clear();
        if (wave.laneIds.empty()) {
            output = lanes_;
        } else {
            for (const auto laneId : wave.laneIds) {
                const auto* value = lane(laneId);
                if (!value) return false;
                output.push_back(*value);
            }
            std::sort(output.begin(), output.end(), idLess<SpawnLane>);
            output.erase(
                std::unique(output.begin(), output.end(),
                            [](const SpawnLane& a, const SpawnLane& b) {
                                return a.id == b.id;
                            }),
                output.end());
        }
        return !output.empty();
    }

    static std::uint64_t coordinateDistance(
        std::int32_t first,
        std::int32_t second) noexcept {
        const auto left = static_cast<std::int64_t>(first);
        const auto right = static_cast<std::int64_t>(second);
        return static_cast<std::uint64_t>(
            left >= right ? left - right : right - left);
    }

    bool buildLaneRoute(
        const SpawnLane& lane,
        PlannedLaneRoute& output) const {
        output = {};
        output.id = lane.id;
        if (lane.usesGraph()) {
            if (lane.startNodeId == 0 || lane.goalNodeId == 0) return false;
            const auto route = laneGraph_.findRoute(
                lane.startNodeId, lane.goalNodeId);
            if (!route.found || route.points.empty()) return false;
            output.totalCost = route.totalCost;
            output.nodeIds = route.nodeIds;
            output.points = route.points;
            return true;
        }
        const auto total = coordinateDistance(lane.spawn.x, lane.goal.x) +
                           coordinateDistance(lane.spawn.y, lane.goal.y);
        if (total > std::numeric_limits<std::uint32_t>::max()) return false;
        output.totalCost = static_cast<std::uint32_t>(total);
        output.points.push_back(lane.spawn);
        if (lane.goal != lane.spawn) output.points.push_back(lane.goal);
        return true;
    }

    template<class Entry, class Weight>
    static std::size_t selectWeighted(
        foundation::RandomStream& random,
        const std::vector<std::size_t>& indices,
        const std::vector<Entry>& entries,
        Weight weight) {
        std::uint32_t total = 0;
        for (const auto index : indices) total += weight(entries[index]);
        auto roll = random.NextBounded(total);
        for (const auto index : indices) {
            const auto current = weight(entries[index]);
            if (roll < current) return index;
            roll -= current;
        }
        return indices.back();
    }

    template<class T>
    static bool selectDefinitions(
        foundation::RandomStream& random,
        std::vector<T> candidates,
        std::uint32_t count,
        std::vector<T>& output) {
        output.clear();
        if (count > candidates.size()) return false;
        for (std::uint32_t selection = 0; selection < count; ++selection) {
            std::uint64_t total = 0;
            for (const auto& candidate : candidates) total += candidate.weight;
            if (total == 0 ||
                total > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            auto roll = random.NextBounded(static_cast<std::uint32_t>(total));
            std::size_t selected = 0;
            for (; selected < candidates.size(); ++selected) {
                if (roll < candidates[selected].weight) break;
                roll -= candidates[selected].weight;
            }
            if (selected == candidates.size()) selected = candidates.size() - 1;
            output.push_back(candidates[selected]);
            candidates.erase(candidates.begin() +
                static_cast<std::ptrdiff_t>(selected));
        }
        return true;
    }

    static std::size_t selectWeightedLane(
        foundation::RandomStream& random,
        const std::vector<SpawnLane>& lanes) {
        std::uint32_t total = 0;
        for (const auto& lane : lanes) total += lane.weight;
        auto roll = random.NextBounded(total);
        for (std::size_t index = 0; index < lanes.size(); ++index) {
            if (roll < lanes[index].weight) return index;
            roll -= lanes[index].weight;
        }
        return lanes.size() - 1;
    }

    bool selectAffixes(
        const WaveDefinition& wave,
        std::vector<WaveAffixDefinition>& output) const {
        std::vector<WaveAffixDefinition> candidates;
        if (wave.affixPool.empty()) {
            candidates = affixes_;
        } else {
            for (const auto id : wave.affixPool) {
                const auto* value = affix(id);
                if (!value) return false;
                candidates.push_back(*value);
            }
            std::sort(candidates.begin(), candidates.end(),
                      idLess<WaveAffixDefinition>);
            candidates.erase(
                std::unique(candidates.begin(), candidates.end(),
                            [](const auto& a, const auto& b) {
                                return a.id == b.id;
                            }),
                candidates.end());
        }
        foundation::RandomStream random(
            rootSeed_, scopedStreamId("tower-defense.wave-affix", wave.id));
        return selectDefinitions(random, std::move(candidates),
                                 wave.affixChoices, output);
    }

    WaveStartFailure selectBosses(
        const WaveDefinition& wave,
        std::uint32_t& remaining,
        std::vector<BossDefinition>& output) const {
        output.clear();
        if (wave.bossCount == 0) return WaveStartFailure::None;
        std::vector<BossDefinition> candidates;
        if (wave.bossPool.empty()) {
            candidates = bosses_;
        } else {
            for (const auto id : wave.bossPool) {
                const auto* value = boss(id);
                if (!value) return WaveStartFailure::UnknownBoss;
                candidates.push_back(*value);
            }
            std::sort(candidates.begin(), candidates.end(),
                      idLess<BossDefinition>);
            candidates.erase(
                std::unique(candidates.begin(), candidates.end(),
                            [](const auto& a, const auto& b) {
                                return a.id == b.id;
                            }),
                candidates.end());
        }
        if (wave.bossCount > candidates.size()) {
            return WaveStartFailure::NoAffordableBoss;
        }

        foundation::RandomStream random(
            rootSeed_, scopedStreamId("tower-defense.wave-boss", wave.id));
        for (std::uint32_t index = 0; index < wave.bossCount; ++index) {
            std::vector<std::size_t> affordable;
            std::uint64_t total = 0;
            for (std::size_t candidate = 0;
                 candidate < candidates.size(); ++candidate) {
                if (candidates[candidate].budgetCost <= remaining) {
                    affordable.push_back(candidate);
                    total += candidates[candidate].weight;
                }
            }
            if (affordable.empty()) return WaveStartFailure::NoAffordableBoss;
            if (total == 0 ||
                total > std::numeric_limits<std::uint32_t>::max()) {
                return WaveStartFailure::InvalidDefinition;
            }
            const auto selected = selectWeighted(
                random, affordable, candidates,
                [](const BossDefinition& value) { return value.weight; });
            remaining -= candidates[selected].budgetCost;
            output.push_back(candidates[selected]);
            candidates.erase(candidates.begin() +
                static_cast<std::ptrdiff_t>(selected));
        }
        return WaveStartFailure::None;
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
            std::sort(result.begin(), result.end(), idLess<RewardDefinition>);
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
            state_.resolved != state_.spawned || state_.spawned == 0) {
            return false;
        }
        buildRewardOffer();
        state_.phase = offer_.choices.empty()
            ? WavePhase::Complete : WavePhase::RewardPending;
        return true;
    }

    void buildRewardOffer() {
        offer_ = {};
        offer_.waveId = state_.waveId;
        if (plan_.rewardChoices == 0 || plan_.rewards.empty()) return;
        std::vector<RewardDefinition> candidates = plan_.rewards;
        foundation::RandomStream random(
            rootSeed_, scopedStreamId("tower-defense.wave-reward",
                                     state_.waveId));
        const auto count = std::min<std::size_t>(
            plan_.rewardChoices, candidates.size());
        for (std::size_t choice = 0; choice < count; ++choice) {
            std::uint32_t total = 0;
            for (const auto& candidate : candidates) total += candidate.weight;
            auto roll = random.NextBounded(total);
            std::size_t selected = 0;
            for (; selected < candidates.size(); ++selected) {
                if (roll < candidates[selected].weight) break;
                roll -= candidates[selected].weight;
            }
            if (selected == candidates.size()) selected = candidates.size() - 1;
            offer_.choices.push_back(candidates[selected].id);
            candidates.erase(candidates.begin() +
                static_cast<std::ptrdiff_t>(selected));
        }
    }

    std::uint64_t rootSeed_{1};
    LaneGraph laneGraph_;
    std::vector<SpawnLane> lanes_;
    std::vector<WaveDefinition> waves_;
    std::vector<RewardDefinition> rewards_;
    std::vector<WaveAffixDefinition> affixes_;
    std::vector<BossDefinition> bosses_;
    WavePlan plan_;
    WaveState state_;
    RewardOffer offer_;
};

} // namespace rts::tower_defense
