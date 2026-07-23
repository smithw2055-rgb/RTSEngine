#pragma once

#include <RTSEngine/Rts/Simulation.h>
#include <RTSEngine/TowerDefense/WaveDirector.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/DeterministicCommandStream.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::tower_defense {

class TowerDefenseSimulationArchive;

enum class CommandType : std::uint8_t {
    StartWave,
    ChooseReward
};

struct TickCommand {
    std::uint64_t targetTick{};
    std::uint32_t issuer{};
    std::uint32_t sequence{};
    CommandType type{CommandType::StartWave};
    std::uint32_t objectId{};
};

using TickCommandStream = sim::DeterministicCommandStream<TickCommand>;

enum class EventType : std::uint8_t {
    WaveStarted,
    WaveRejected,
    EnemySpawned,
    EnemyDefeated,
    BaseCoreDestroyed,
    WaveCompleted,
    RewardOffered,
    RewardChosen,
    RewardRejected
};

struct Event {
    std::uint64_t tick{};
    EventType type{};
    WaveId waveId{};
    std::uint32_t objectId{};
    ecs::Entity entity{};
    std::int32_t value{};
    std::uint32_t reason{};
};

struct EnemySnapshot {
    ecs::Entity entity{};
    WaveId waveId{};
    LaneId laneId{};
    std::uint32_t unitDefinitionId{};
    bool alive{};
};

struct TowerDefenseSnapshot {
    std::uint64_t tick{};
    std::uint64_t worldHash{};
    std::uint64_t rtsWorldHash{};
    ecs::Entity baseCore{};
    std::int32_t coreHealthCurrent{};
    std::int32_t coreHealthMaximum{};
    WaveState wave{};
    std::uint32_t plannedSpawns{};
    std::uint32_t activeEnemies{};
    std::vector<RewardId> rewardChoices;
    RewardId selectedReward{};
    std::vector<EnemySnapshot> enemies;
};

class TowerDefenseSimulation {
public:
    TowerDefenseSimulation(std::int32_t width = 32,
                           std::int32_t height = 32,
                           std::uint64_t rootSeed = 1)
        : rts_(width, height), director_(rootSeed), rootSeed_(rootSeed) {}

    void registerUnit(gameplay::UnitDefinition definition) {
        replaceById(unitDefinitions_, definition);
        rts_.registerUnit(definition);
    }

    void registerBuilding(gameplay::BuildingDefinition definition) {
        rts_.registerBuilding(std::move(definition));
    }

    bool upsertLaneNode(LaneNode node) {
        return rts_.navigation().contains(node.point) &&
               director_.upsertLaneNode(node);
    }

    bool removeLaneNode(LaneNodeId id) {
        return director_.removeLaneNode(id);
    }

    bool connectLaneNodes(
        LaneNodeId from,
        LaneNodeId to,
        std::uint32_t cost = 1) {
        return director_.connectLaneNodes(from, to, cost);
    }

    bool connectLaneNodesBidirectional(
        LaneNodeId first,
        LaneNodeId second,
        std::uint32_t cost = 1) {
        return director_.connectLaneNodesBidirectional(first, second, cost);
    }

    bool disconnectLaneNodes(LaneNodeId from, LaneNodeId to) {
        return director_.disconnectLaneNodes(from, to);
    }

    bool setLaneConnectionEnabled(
        LaneNodeId from,
        LaneNodeId to,
        bool enabled) {
        return director_.setLaneConnectionEnabled(from, to, enabled);
    }

    bool registerLane(SpawnLane lane) {
        gameplay::GridPoint spawn;
        gameplay::GridPoint goal;
        if (!resolveLaneEndpoints(lane, spawn, goal) ||
            !rts_.navigation().contains(spawn) ||
            !rts_.navigation().contains(goal)) {
            return false;
        }
        lane.spawn = spawn;
        lane.goal = goal;
        if (!director_.registerLane(lane)) return false;
        replaceById(laneDefinitions_, lane);
        return true;
    }

    bool registerWave(WaveDefinition wave) {
        const auto copy = wave;
        if (!director_.registerWave(std::move(wave))) return false;
        replaceById(waveDefinitions_, copy);
        return true;
    }

    bool registerReward(RewardDefinition reward) {
        if (!director_.registerReward(reward)) return false;
        replaceById(rewardDefinitions_, reward);
        return true;
    }

    void setResources(std::int32_t available) noexcept {
        rts_.setResources(available);
    }

    void setPlayerTeam(std::uint32_t teamId) noexcept {
        playerTeamId_ = teamId;
        rts_.setPlayerTeam(teamId);
    }

    bool setTeamModifierProfile(
        std::uint32_t teamId,
        gameplay::TeamModifierProfile profile) {
        return rts_.setTeamModifierProfile(teamId, profile);
    }

    void setRequiredRoute(gameplay::GridPoint start,
                          gameplay::GridPoint goal) noexcept {
        rts_.setRequiredRoute(start, goal);
    }

    bool setBlocked(gameplay::GridPoint point, bool blocked) {
        return rts_.setBlocked(point, blocked);
    }

    ecs::Entity createBaseCore(gameplay::Position position,
                               std::uint32_t teamId,
                               gameplay::CombatStats combat) {
        combat.maximumHealth = std::max<std::int32_t>(1, combat.maximumHealth);
        combat.bounty = 0;
        core_ = rts_.createUnit(position, gameplay::MoveSpeed{0}, teamId, combat);
        playerTeamId_ = teamId;
        rts_.setPlayerTeam(teamId);
        coreFailureReported_ = false;
        return core_;
    }

    ecs::Entity createDefender(gameplay::Position position,
                               gameplay::MoveSpeed speed,
                               std::uint32_t teamId,
                               gameplay::CombatStats combat) {
        return rts_.createUnit(position, speed, teamId, combat);
    }

    bool submit(TickCommand command) {
        return commands_.submit(std::move(command));
    }

    bool submitRts(gameplay::TickCommand command) {
        return rts_.submit(std::move(command));
    }

    bool step(std::uint64_t tick) {
        if (hasStepped_ && tick <= lastTick_) return false;
        hasStepped_ = true;
        lastTick_ = tick;
        events_.clear();

        for (const auto& command : commands_.consume(tick)) {
            processCommand(tick, command);
        }
        spawnDueEnemies(tick);
        rts_.step(tick);
        reconcileAfterTick(tick);
        buildSnapshot(tick);
        return true;
    }

    const TowerDefenseSnapshot& snapshot() const noexcept { return snapshot_; }
    const std::vector<Event>& events() const noexcept { return events_; }
    const WaveDirector& director() const noexcept { return director_; }
    const gameplay::RtsSimulation& rts() const noexcept { return rts_; }
    const gameplay::ResourceLedger& resources() const noexcept {
        return rts_.resources();
    }
    TickCommandStream::State commandStreamState() const {
        return commands_.snapshot();
    }
    std::uint64_t lastTick() const noexcept { return lastTick_; }
    std::uint64_t rootSeed() const noexcept { return rootSeed_; }

private:
    friend class TowerDefenseSimulationArchive;

    struct TrackedEnemy {
        ecs::Entity entity{};
        WaveId waveId{};
        LaneId laneId{};
        std::uint32_t unitDefinitionId{};
        bool resolved{};
    };

    template<class T>
    static void replaceById(std::vector<T>& values, T value) {
        const auto iterator = std::lower_bound(
            values.begin(), values.end(), value.id,
            [](const T& current, std::uint32_t id) {
                return current.id < id;
            });
        if (iterator != values.end() && iterator->id == value.id) {
            *iterator = std::move(value);
        } else {
            values.insert(iterator, std::move(value));
        }
    }

    const gameplay::UnitDefinition* unitDefinition(
        std::uint32_t id) const noexcept {
        const auto iterator = std::lower_bound(
            unitDefinitions_.begin(), unitDefinitions_.end(), id,
            [](const gameplay::UnitDefinition& value, std::uint32_t key) {
                return value.id < key;
            });
        return iterator != unitDefinitions_.end() && iterator->id == id
            ? &*iterator
            : nullptr;
    }

    bool resolveLaneEndpoints(
        const SpawnLane& lane,
        gameplay::GridPoint& spawn,
        gameplay::GridPoint& goal) const noexcept {
        if (!lane.usesGraph()) {
            spawn = lane.spawn;
            goal = lane.goal;
            return true;
        }
        if (lane.startNodeId == 0 || lane.goalNodeId == 0) return false;
        const auto* start = director_.laneGraph().node(lane.startNodeId);
        const auto* end = director_.laneGraph().node(lane.goalNodeId);
        if (!start || !end) return false;
        spawn = start->point;
        goal = end->point;
        return true;
    }

    void processCommand(std::uint64_t tick, const TickCommand& command) {
        if (command.type == CommandType::StartWave) {
            startWave(tick, command.objectId);
        } else {
            chooseReward(tick, command.objectId);
        }
    }

    void startWave(std::uint64_t tick, WaveId id) {
        WaveStartResult result;
        if (!core_.valid() || !rts_.world().alive(core_)) {
            result = {false, WaveStartFailure::NoBaseCore, id};
        } else if (director_.state().phase == WavePhase::Spawning ||
                   director_.state().phase == WavePhase::Active ||
                   director_.state().phase == WavePhase::RewardPending) {
            result = director_.begin(id, tick);
        } else {
            result = validateWaveContent(id);
            if (result.accepted) result = director_.begin(id, tick);
        }

        if (!result.accepted) {
            events_.push_back(
                {tick, EventType::WaveRejected, id, 0, {}, 0,
                 static_cast<std::uint32_t>(result.failure)});
            return;
        }

        trackedEnemies_.clear();
        coreFailureReported_ = false;
        events_.push_back({tick, EventType::WaveStarted, id, 0, {}, 0, 0});
    }

    WaveStartResult validateWaveContent(WaveId id) const {
        const auto* wave = director_.definition(id);
        if (!wave) return {false, WaveStartFailure::UnknownWave, id};
        for (const auto& enemy : wave->enemies) {
            const auto* definition = unitDefinition(enemy.unitDefinitionId);
            if (!definition || !definition->combat.attackCapable()) {
                return {false, WaveStartFailure::UnknownUnitDefinition, id};
            }
        }

        std::vector<const SpawnLane*> lanes;
        if (wave->laneIds.empty()) {
            for (const auto& lane : director_.lanes()) lanes.push_back(&lane);
        } else {
            for (const auto laneId : wave->laneIds) {
                const auto* lane = director_.lane(laneId);
                if (!lane) return {false, WaveStartFailure::MissingLane, id};
                lanes.push_back(lane);
            }
        }
        if (lanes.empty()) return {false, WaveStartFailure::MissingLane, id};

        for (const auto* lane : lanes) {
            PlannedLaneRoute route;
            if (!director_.resolveLaneRoute(lane->id, route) ||
                route.points.empty()) {
                return {false, WaveStartFailure::InvalidLaneRoute, id};
            }
            for (const auto point : route.points) {
                if (!rts_.navigation().contains(point) ||
                    rts_.navigation().blocked(point)) {
                    return {false, WaveStartFailure::InvalidLane, id};
                }
            }
            for (std::size_t index = 1; index < route.points.size(); ++index) {
                if (!gameplay::GridPathfinder::find(
                        rts_.navigation(),
                        route.points[index - 1],
                        route.points[index]).found) {
                    return {false, WaveStartFailure::InvalidLane, id};
                }
            }
        }
        return {true, WaveStartFailure::None, id};
    }

    void chooseReward(std::uint64_t tick, RewardId id) {
        const auto* selected = director_.chooseReward(id);
        if (!selected) {
            events_.push_back(
                {tick, EventType::RewardRejected, director_.state().waveId,
                 id, {}, 0, 0});
            return;
        }

        const auto next = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(rts_.resources().available) +
                selected->resourceGrant,
            0, std::numeric_limits<std::int32_t>::max());
        rts_.setResources(static_cast<std::int32_t>(next));
        events_.push_back(
            {tick, EventType::RewardChosen, director_.state().waveId,
             id, {}, selected->resourceGrant, 0});
    }

    void spawnDueEnemies(std::uint64_t tick) {
        const auto due = director_.dueSpawns(tick);
        for (const auto& spawn : due) {
            const auto* definition = unitDefinition(spawn.unitDefinitionId);
            if (!definition) {
                director_.fail();
                events_.push_back(
                    {tick, EventType::WaveRejected, director_.state().waveId,
                     spawn.unitDefinitionId, {}, 0,
                     static_cast<std::uint32_t>(
                         WaveStartFailure::UnknownUnitDefinition)});
                return;
            }

            const auto entity = rts_.createUnit(
                {spawn.spawn.x, spawn.spawn.y},
                {definition->cellsPerTick},
                director_.plan().enemyTeamId,
                definition->combat);

            gameplay::TickCommand attackMove;
            attackMove.targetTick = tick;
            attackMove.issuer = internalIssuer(director_.state().waveId);
            attackMove.sequence = spawn.sequence + 1;
            attackMove.type = gameplay::CommandType::AttackMove;
            attackMove.subject = entity;
            attackMove.targetX = spawn.goal.x;
            attackMove.targetY = spawn.goal.y;
            if (!rts_.submit(attackMove)) {
                director_.fail();
                events_.push_back(
                    {tick, EventType::WaveRejected, director_.state().waveId,
                     spawn.unitDefinitionId, entity, 0,
                     static_cast<std::uint32_t>(
                         WaveStartFailure::InvalidDefinition)});
                return;
            }

            trackedEnemies_.push_back(
                {entity, director_.state().waveId, spawn.laneId,
                 spawn.unitDefinitionId, false});
            events_.push_back(
                {tick, EventType::EnemySpawned, director_.state().waveId,
                 spawn.unitDefinitionId, entity,
                 static_cast<std::int32_t>(spawn.laneId), 0});
        }
    }

    void reconcileAfterTick(std::uint64_t tick) {
        if (core_.valid() && !rts_.world().alive(core_)) {
            if (!coreFailureReported_) {
                coreFailureReported_ = true;
                director_.fail();
                events_.push_back(
                    {tick, EventType::BaseCoreDestroyed,
                     director_.state().waveId, 0, core_, 0, 0});
            }
        }

        for (auto& enemy : trackedEnemies_) {
            if (enemy.resolved || rts_.world().alive(enemy.entity)) continue;
            enemy.resolved = true;
            events_.push_back(
                {tick, EventType::EnemyDefeated, enemy.waveId,
                 enemy.unitDefinitionId, enemy.entity, 0, 0});
            if (director_.markEnemyResolved()) publishWaveCompletion(tick);
        }
    }

    void publishWaveCompletion(std::uint64_t tick) {
        events_.push_back(
            {tick, EventType::WaveCompleted, director_.state().waveId,
             0, {}, 0, 0});
        for (const auto rewardId : director_.offer().choices) {
            const auto* reward = director_.reward(rewardId);
            events_.push_back(
                {tick, EventType::RewardOffered, director_.state().waveId,
                 rewardId, {}, reward ? reward->resourceGrant : 0, 0});
        }
    }

    static void hashCommand(
        foundation::CanonicalHash& hash,
        const TickCommand& command) {
        hash.WriteU64(command.targetTick);
        hash.WriteU32(command.issuer);
        hash.WriteU32(command.sequence);
        hash.WriteU8(static_cast<std::uint8_t>(command.type));
        hash.WriteU32(command.objectId);
    }

    void buildSnapshot(std::uint64_t tick) {
        snapshot_.tick = tick;
        snapshot_.rtsWorldHash = rts_.snapshot().worldHash;
        snapshot_.baseCore = core_;
        snapshot_.coreHealthCurrent = 0;
        snapshot_.coreHealthMaximum = 0;
        if (const auto* health =
                rts_.world().try_get<gameplay::Health>(core_)) {
            snapshot_.coreHealthCurrent = health->current;
            snapshot_.coreHealthMaximum = health->maximum;
        }
        snapshot_.wave = director_.state();
        snapshot_.plannedSpawns = static_cast<std::uint32_t>(
            director_.plan().spawns.size());
        snapshot_.activeEnemies = director_.activeEnemies();
        snapshot_.rewardChoices = director_.offer().choices;
        snapshot_.selectedReward = director_.offer().selected;
        snapshot_.enemies.clear();
        for (const auto& enemy : trackedEnemies_) {
            snapshot_.enemies.push_back(
                {enemy.entity, enemy.waveId, enemy.laneId,
                 enemy.unitDefinitionId,
                 rts_.world().alive(enemy.entity)});
        }
        std::sort(snapshot_.enemies.begin(), snapshot_.enemies.end(),
                  [](const EnemySnapshot& a, const EnemySnapshot& b) {
                      return a.entity < b.entity;
                  });

        foundation::CanonicalHash hash;
        hash.WriteU64(tick);
        hash.WriteU64(snapshot_.rtsWorldHash);
        hash.WriteU32(core_.index);
        hash.WriteU32(core_.generation);
        hash.WriteI32(snapshot_.coreHealthCurrent);
        hash.WriteI32(snapshot_.coreHealthMaximum);
        hash.WriteU32(playerTeamId_);
        director_.appendHash(hash);
        const auto commandState = commands_.snapshot();
        hash.WriteU64(commandState.committedThrough);
        hash.WriteU32(static_cast<std::uint32_t>(commandState.pending.size()));
        for (const auto& command : commandState.pending) {
            hashCommand(hash, command);
        }
        hash.WriteU32(static_cast<std::uint32_t>(trackedEnemies_.size()));
        for (const auto& enemy : trackedEnemies_) {
            hash.WriteU32(enemy.entity.index);
            hash.WriteU32(enemy.entity.generation);
            hash.WriteU32(enemy.waveId);
            hash.WriteU32(enemy.laneId);
            hash.WriteU32(enemy.unitDefinitionId);
            hash.WriteBool(enemy.resolved);
        }
        snapshot_.worldHash = hash.Value();
    }

    static std::uint32_t internalIssuer(WaveId waveId) noexcept {
        return 0x80000000u | (waveId & 0x7fffffffu);
    }

    gameplay::RtsSimulation rts_;
    WaveDirector director_;
    TickCommandStream commands_;
    std::vector<gameplay::UnitDefinition> unitDefinitions_;
    std::vector<SpawnLane> laneDefinitions_;
    std::vector<WaveDefinition> waveDefinitions_;
    std::vector<RewardDefinition> rewardDefinitions_;
    std::vector<TrackedEnemy> trackedEnemies_;
    std::vector<Event> events_;
    TowerDefenseSnapshot snapshot_;
    ecs::Entity core_{};
    std::uint32_t playerTeamId_{1};
    std::uint64_t rootSeed_{1};
    std::uint64_t lastTick_{};
    bool hasStepped_{};
    bool coreFailureReported_{};
};

} // namespace rts::tower_defense
