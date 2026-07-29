#pragma once

#include <RTSEngine/Rts/Simulation.h>
#include <RTSEngine/TowerDefense/WaveDirector.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/AuthoritativeStep.h>
#include <rts/sim/DeterministicCommandStream.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::roguelite {
class RunSimulation;
}

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
    WaveAffixSelected,
    EnemySpawned,
    BossSpawned,
    EnemyWaypointReached,
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
    std::uint32_t spawnSequence{};
    BossId bossId{};
    std::uint32_t waypointIndex{};
    std::uint32_t waypointCount{};
    std::int32_t healthCurrent{};
    std::int32_t healthMaximum{};
    std::int32_t armor{};
    std::int32_t weaponDamage{};
    std::int32_t cellsPerTick{};
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
    std::uint32_t plannedBosses{};
    std::uint32_t activeEnemies{};
    std::vector<WaveAffixId> waveAffixes;
    std::vector<RewardId> rewardChoices;
    RewardId selectedReward{};
    std::vector<EnemySnapshot> enemies;
};

class TowerDefenseSimulation {
public:
    class RuntimeAuthority final {
    private:
        constexpr RuntimeAuthority() noexcept = default;
        friend class ::rts::roguelite::RunSimulation;
    };

    TowerDefenseSimulation(std::int32_t width = 32,
                           std::int32_t height = 32,
                           std::uint64_t rootSeed = 1);
    ~TowerDefenseSimulation();

    TowerDefenseSimulation(const TowerDefenseSimulation&) = delete;
    TowerDefenseSimulation& operator=(const TowerDefenseSimulation&) = delete;
    TowerDefenseSimulation(TowerDefenseSimulation&&) = delete;
    TowerDefenseSimulation& operator=(TowerDefenseSimulation&&) = delete;

    bool configurationFrozen() const noexcept {
        return hasStepped_ || rts_.configurationFrozen();
    }

    void freezeConfiguration() noexcept {
        rts_.freezeConfiguration();
    }

    bool registerUnit(gameplay::UnitDefinition definition) {
        if (configurationFrozen() || !rts_.registerUnit(definition)) {
            return false;
        }
        replaceById(unitDefinitions_, std::move(definition));
        return true;
    }

    bool registerBuilding(gameplay::BuildingDefinition definition) {
        return !configurationFrozen() &&
               rts_.registerBuilding(std::move(definition));
    }

    bool registerAffix(WaveAffixDefinition affix) {
        if (configurationFrozen()) return false;
        const auto copy = affix;
        if (!director_.registerAffix(std::move(affix))) return false;
        replaceById(affixDefinitions_, copy);
        return true;
    }

    bool registerBoss(BossDefinition boss) {
        if (configurationFrozen()) return false;
        const auto copy = boss;
        if (!director_.registerBoss(std::move(boss))) return false;
        replaceById(bossDefinitions_, copy);
        return true;
    }

    bool upsertLaneNode(LaneNode node) {
        return !configurationFrozen() &&
               rts_.navigation().contains(node.point) &&
               director_.upsertLaneNode(node);
    }

    bool removeLaneNode(LaneNodeId id) {
        return !configurationFrozen() && director_.removeLaneNode(id);
    }

    bool connectLaneNodes(
        LaneNodeId from,
        LaneNodeId to,
        std::uint32_t cost = 1) {
        return !configurationFrozen() &&
               director_.connectLaneNodes(from, to, cost);
    }

    bool connectLaneNodesBidirectional(
        LaneNodeId first,
        LaneNodeId second,
        std::uint32_t cost = 1) {
        return !configurationFrozen() &&
               director_.connectLaneNodesBidirectional(first, second, cost);
    }

    bool disconnectLaneNodes(LaneNodeId from, LaneNodeId to) {
        return !configurationFrozen() &&
               director_.disconnectLaneNodes(from, to);
    }

    bool setLaneConnectionEnabled(
        LaneNodeId from,
        LaneNodeId to,
        bool enabled) {
        return !configurationFrozen() &&
               director_.setLaneConnectionEnabled(from, to, enabled);
    }

    bool registerLane(SpawnLane lane) {
        if (configurationFrozen()) return false;
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
        return !configurationFrozen() &&
               registerWaveUnchecked(std::move(wave));
    }

    bool registerRuntimeWave(RuntimeAuthority, WaveDefinition wave) {
        return registerWaveUnchecked(std::move(wave));
    }

    bool registerReward(RewardDefinition reward) {
        if (configurationFrozen() || !director_.registerReward(reward)) {
            return false;
        }
        replaceById(rewardDefinitions_, reward);
        return true;
    }

    void setResources(std::int32_t available) noexcept {
        rts_.setResources(available);
    }

    bool setPlayerTeam(std::uint32_t teamId) noexcept {
        if (configurationFrozen() || !rts_.setPlayerTeam(teamId)) {
            return false;
        }
        playerTeamId_ = teamId;
        return true;
    }

    bool setTeamModifierProfile(
        std::uint32_t teamId,
        gameplay::TeamModifierProfile profile) {
        return rts_.setTeamModifierProfile(teamId, profile);
    }

    bool setRequiredRoute(gameplay::GridPoint start,
                          gameplay::GridPoint goal) noexcept {
        return !configurationFrozen() && rts_.setRequiredRoute(start, goal);
    }

    bool setBlocked(gameplay::GridPoint point, bool blocked) {
        return !configurationFrozen() && rts_.setBlocked(point, blocked);
    }

    ecs::Entity createBaseCore(gameplay::Position position,
                               std::uint32_t teamId,
                               gameplay::CombatStats combat) {
        if (configurationFrozen()) return {};
        combat.maximumHealth = std::max<std::int32_t>(1, combat.maximumHealth);
        combat.bounty = 0;
        const auto entity =
            rts_.createUnit(position, gameplay::MoveSpeed{0}, teamId, combat);
        if (!entity.valid() || !rts_.setPlayerTeam(teamId)) return {};
        core_ = entity;
        playerTeamId_ = teamId;
        coreFailureReported_ = false;
        return core_;
    }

    ecs::Entity createDefender(gameplay::Position position,
                               gameplay::MoveSpeed speed,
                               std::uint32_t teamId,
                               gameplay::CombatStats combat) {
        if (configurationFrozen()) return {};
        return rts_.createUnit(position, speed, teamId, combat);
    }

    bool submit(TickCommand command) {
        if (isRuntimeIssuer(command.issuer)) return false;
        return commands_.submit(std::move(command));
    }

    bool submitRuntime(RuntimeAuthority, TickCommand command) {
        return isRuntimeIssuer(command.issuer) &&
               commands_.submit(std::move(command));
    }

    bool submitRts(gameplay::TickCommand command) {
        return rts_.submit(std::move(command));
    }

    sim::AuthoritativeStepValidation validateStep(
        std::uint64_t tick) const noexcept {
        const auto outer = sim::ValidateAuthoritativeStep(
            hasStepped_, lastTick_, tick);
        if (!outer) return outer;
        const auto inner = rts_.validateStep(tick);
        if (!inner) {
            return {
                sim::AuthoritativeStepFailure::InnerLayerRejected,
                inner.expectedTick,
                tick};
        }
        return outer;
    }

    bool step(std::uint64_t tick) {
        if (!validateStep(tick)) return false;

        freezeConfiguration();
        events_.clear();
        for (const auto& command : commands_.consume(tick)) {
            processCommand(tick, command);
        }
        spawnDueEnemies(tick);
        if (rts_.stepDetailed(tick) != gameplay::RtsStepResult::Advanced) {
            return false;
        }
        reconcileAfterTick(tick);
        buildSnapshot(tick);
        hasStepped_ = true;
        lastTick_ = tick;
        return true;
    }

    std::uint64_t nextExpectedTick() const noexcept {
        return hasStepped_ ? lastTick_ + 1u : 0u;
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
        std::uint32_t spawnSequence{};
        BossId bossId{};
        std::uint32_t waypointIndex{};
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

    bool registerWaveUnchecked(WaveDefinition wave) {
        const auto copy = wave;
        if (!director_.registerWave(std::move(wave))) return false;
        replaceById(waveDefinitions_, copy);
        return true;
    }

    const gameplay::UnitDefinition* unitDefinition(
        std::uint32_t id) const noexcept {
        const auto iterator = std::lower_bound(
            unitDefinitions_.begin(), unitDefinitions_.end(), id,
            [](const gameplay::UnitDefinition& value, std::uint32_t key) {
                return value.id < key;
            });
        return iterator != unitDefinitions_.end() && iterator->id == id
            ? &*iterator : nullptr;
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
        for (const auto& affix : director_.plan().affixes) {
            events_.push_back(
                {tick, EventType::WaveAffixSelected, id,
                 affix.id, {}, 0, 0});
        }
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

        if (wave->bossCount > 0) {
            std::vector<const BossDefinition*> bosses;
            if (wave->bossPool.empty()) {
                for (const auto& boss : director_.bosses()) {
                    bosses.push_back(&boss);
                }
            } else {
                for (const auto bossId : wave->bossPool) {
                    const auto* boss = director_.boss(bossId);
                    if (!boss) return {false, WaveStartFailure::UnknownBoss, id};
                    bosses.push_back(boss);
                }
            }
            for (const auto* boss : bosses) {
                const auto* definition = unitDefinition(boss->unitDefinitionId);
                if (!definition || !definition->combat.attackCapable()) {
                    return {false, WaveStartFailure::UnknownUnitDefinition, id};
                }
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
                route.points.empty() ||
                route.points.size() >
                    std::numeric_limits<std::uint32_t>::max()) {
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

    bool reserveInternalSequences(
        std::size_t count,
        std::uint32_t& firstSequence) noexcept {
        firstSequence = 0;
        if (count == 0) return true;
        constexpr auto maximum = static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max());
        if (nextInternalRtsSequence_ > maximum) return false;
        const auto available = maximum - nextInternalRtsSequence_ + 1u;
        if (static_cast<std::uint64_t>(count) > available) return false;
        firstSequence = static_cast<std::uint32_t>(nextInternalRtsSequence_);
        nextInternalRtsSequence_ += static_cast<std::uint64_t>(count);
        return true;
    }

    void spawnDueEnemies(std::uint64_t tick) {
        const auto due = director_.dueSpawns(tick);
        for (const auto& spawn : due) {
            const auto* definition = unitDefinition(spawn.unitDefinitionId);
            const auto* route = director_.plannedRoute(spawn.laneId);
            if (!definition || !route || route->points.empty() ||
                !spawn.modifier.valid() ||
                route->points.front() != spawn.spawn ||
                route->points.back() != spawn.goal ||
                route->points.size() >
                    std::numeric_limits<std::uint32_t>::max()) {
                director_.fail();
                events_.push_back(
                    {tick, EventType::WaveRejected, director_.state().waveId,
                     spawn.unitDefinitionId, {}, 0,
                     static_cast<std::uint32_t>(
                         definition
                             ? WaveStartFailure::InvalidLaneRoute
                             : WaveStartFailure::UnknownUnitDefinition)});
                return;
            }

            const auto segmentCount = route->points.size() - 1u;
            std::uint32_t firstSequence = 0;
            if (!reserveInternalSequences(segmentCount, firstSequence)) {
                director_.fail();
                events_.push_back(
                    {tick, EventType::WaveRejected, director_.state().waveId,
                     spawn.unitDefinitionId, {}, 0,
                     static_cast<std::uint32_t>(
                         WaveStartFailure::InvalidDefinition)});
                return;
            }

            const auto combat = ApplyEnemyStatModifier(
                definition->combat, spawn.modifier);
            const auto speed = ApplySpeedModifier(
                definition->cellsPerTick, spawn.modifier);
            const auto entity = rts_.createUnitInternal(
                {spawn.spawn.x, spawn.spawn.y},
                {speed},
                director_.plan().enemyTeamId,
                combat,
                definition->visionRange);

            for (std::size_t waypoint = 1;
                 waypoint < route->points.size(); ++waypoint) {
                gameplay::TickCommand attackMove;
                attackMove.targetTick = tick;
                attackMove.issuer = internalIssuer(director_.state().waveId);
                attackMove.sequence = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(firstSequence) + waypoint - 1u);
                attackMove.type = gameplay::CommandType::AttackMove;
                attackMove.subject = entity;
                attackMove.targetX = route->points[waypoint].x;
                attackMove.targetY = route->points[waypoint].y;
                attackMove.append = waypoint > 1u;
                if (!rts_.submitInternal(attackMove)) {
                    director_.fail();
                    events_.push_back(
                        {tick, EventType::WaveRejected,
                         director_.state().waveId,
                         spawn.unitDefinitionId, entity, 0,
                         static_cast<std::uint32_t>(
                             WaveStartFailure::InvalidDefinition)});
                    return;
                }
            }

            const auto waypointIndex = route->points.size() > 1u
                ? 1u : static_cast<std::uint32_t>(route->points.size());
            trackedEnemies_.push_back(
                {entity, director_.state().waveId, spawn.laneId,
                 spawn.unitDefinitionId, spawn.sequence, spawn.bossId,
                 static_cast<std::uint32_t>(waypointIndex), false});
            events_.push_back(
                {tick, EventType::EnemySpawned, director_.state().waveId,
                 spawn.unitDefinitionId, entity,
                 static_cast<std::int32_t>(spawn.laneId), 0});
            if (spawn.bossId != 0) {
                events_.push_back(
                    {tick, EventType::BossSpawned, director_.state().waveId,
                     spawn.bossId, entity,
                     static_cast<std::int32_t>(spawn.unitDefinitionId), 0});
            }
        }
    }

    void reconcileWaypointProgress(std::uint64_t tick) {
        for (const auto& event : rts_.events()) {
            if (event.type != gameplay::DomainEventType::MoveCompleted) continue;
            const auto found = std::find_if(
                trackedEnemies_.begin(), trackedEnemies_.end(),
                [&](const TrackedEnemy& enemy) {
                    return !enemy.resolved && enemy.entity == event.entity;
                });
            if (found == trackedEnemies_.end()) continue;
            const auto* route = director_.plannedRoute(found->laneId);
            if (!route || found->waypointIndex >= route->points.size()) continue;
            ++found->waypointIndex;
            const auto displayIndex = static_cast<std::int32_t>(
                std::min<std::uint32_t>(
                    found->waypointIndex,
                    static_cast<std::uint32_t>(
                        std::numeric_limits<std::int32_t>::max())));
            events_.push_back(
                {tick, EventType::EnemyWaypointReached, found->waveId,
                 found->laneId, found->entity, displayIndex, 0});
        }
    }

    void reconcileAfterTick(std::uint64_t tick) {
        reconcileWaypointProgress(tick);
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
                 enemy.unitDefinitionId, enemy.entity, 0, enemy.bossId});
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
        snapshot_.plannedBosses = static_cast<std::uint32_t>(
            std::count_if(
                director_.plan().spawns.begin(),
                director_.plan().spawns.end(),
                [](const PlannedSpawn& spawn) { return spawn.bossId != 0; }));
        snapshot_.activeEnemies = director_.activeEnemies();
        snapshot_.waveAffixes.clear();
        for (const auto& affix : director_.plan().affixes) {
            snapshot_.waveAffixes.push_back(affix.id);
        }
        snapshot_.rewardChoices = director_.offer().choices;
        snapshot_.selectedReward = director_.offer().selected;
        snapshot_.enemies.clear();
        for (const auto& enemy : trackedEnemies_) {
            const auto* route = director_.plannedRoute(enemy.laneId);
            const auto waypointCount = route
                ? static_cast<std::uint32_t>(route->points.size()) : 0u;
            const auto* health =
                rts_.world().try_get<gameplay::Health>(enemy.entity);
            const auto* armor =
                rts_.world().try_get<gameplay::Armor>(enemy.entity);
            const auto* weapon =
                rts_.world().try_get<gameplay::Weapon>(enemy.entity);
            const auto* speed =
                rts_.world().try_get<gameplay::MoveSpeed>(enemy.entity);
            snapshot_.enemies.push_back(
                {enemy.entity, enemy.waveId, enemy.laneId,
                 enemy.unitDefinitionId, enemy.spawnSequence, enemy.bossId,
                 enemy.waypointIndex, waypointCount,
                 health ? health->current : 0,
                 health ? health->maximum : 0,
                 armor ? armor->value : 0,
                 weapon ? weapon->damage : 0,
                 speed ? speed->cellsPerTick : 0,
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
        hash.WriteU64(nextInternalRtsSequence_);
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
            hash.WriteU32(enemy.spawnSequence);
            hash.WriteU32(enemy.bossId);
            hash.WriteU32(enemy.waypointIndex);
            hash.WriteBool(enemy.resolved);
        }
        snapshot_.worldHash = hash.Value();
    }

    static bool isRuntimeIssuer(std::uint32_t issuer) noexcept {
        return (issuer & 0xc0000000u) == 0xc0000000u;
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
    std::vector<WaveAffixDefinition> affixDefinitions_;
    std::vector<BossDefinition> bossDefinitions_;
    std::vector<TrackedEnemy> trackedEnemies_;
    std::vector<Event> events_;
    TowerDefenseSnapshot snapshot_;
    ecs::Entity core_{};
    std::uint32_t playerTeamId_{1};
    std::uint64_t rootSeed_{1};
    std::uint64_t nextInternalRtsSequence_{1};
    std::uint64_t lastTick_{};
    bool hasStepped_{};
    bool coreFailureReported_{};
};

} // namespace rts::tower_defense
