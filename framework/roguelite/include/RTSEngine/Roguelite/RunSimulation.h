#pragma once

#include <RTSEngine/Roguelite/GameplayStats.h>
#include <RTSEngine/Roguelite/ModifierRuntime.h>
#include <RTSEngine/TowerDefense/Simulation.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/DeterministicCommandStream.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::roguelite {

using RunId = std::uint32_t;

struct RunDefinition {
    RunId id{};
    std::vector<tower_defense::WaveId> waves;
};

enum class RunPhase : std::uint8_t {
    Idle,
    BetweenWaves,
    WaveActive,
    RewardPending,
    Complete,
    Failed
};

struct RunState {
    RunId runId{};
    RunPhase phase{RunPhase::Idle};
    std::uint32_t waveIndex{};
    std::uint32_t completedWaves{};
    tower_defense::WaveId currentWave{};
};

enum class CommandType : std::uint8_t {
    StartRun,
    ChooseModifier
};

struct TickCommand {
    std::uint64_t targetTick{};
    std::uint32_t issuer{};
    std::uint32_t sequence{};
    CommandType type{CommandType::StartRun};
    std::uint32_t objectId{};
};

using TickCommandStream =
    sim::DeterministicCommandStream<TickCommand>;

enum class RunFailure : std::uint8_t {
    None,
    UnknownRun,
    AlreadyActive,
    InvalidDefinition,
    ModifierNotOffered,
    ModifierIneligible,
    TowerDefenseRejected
};

enum class EventType : std::uint8_t {
    RunStarted,
    RunRejected,
    WaveStarted,
    WaveCompleted,
    WaveAdvanced,
    ResourceBonusGranted,
    ModifierApplied,
    ModifierRejected,
    RunCompleted,
    RunFailed
};

struct Event {
    std::uint64_t tick{};
    EventType type{};
    RunId runId{};
    tower_defense::WaveId waveId{};
    ModifierId modifierId{};
    std::int32_t value{};
    std::uint32_t reason{};
};

struct RunSnapshot {
    std::uint64_t tick{};
    std::uint64_t worldHash{};
    std::uint64_t towerDefenseWorldHash{};
    RunState state{};
    std::int32_t availableResources{};
    std::int32_t waveCompletionResourceBonus{};
    gameplay::TeamModifierProfile gameplayProfile{};
    std::vector<ModifierStack> modifiers;
};

class RunSimulation {
public:
    RunSimulation(std::int32_t width = 32,
                  std::int32_t height = 32,
                  std::uint64_t rootSeed = 1)
        : tower_(width, height, rootSeed), rootSeed_(rootSeed) {}

    void registerUnit(gameplay::UnitDefinition definition) {
        tower_.registerUnit(std::move(definition));
    }

    void registerBuilding(gameplay::BuildingDefinition definition) {
        tower_.registerBuilding(std::move(definition));
    }

    bool registerLane(tower_defense::SpawnLane lane) {
        return tower_.registerLane(std::move(lane));
    }

    bool registerWave(tower_defense::WaveDefinition wave) {
        if (wave.id == 0) return false;
        replaceById(waves_, wave);
        return tower_.registerWave(std::move(wave));
    }

    bool registerModifier(ModifierDefinition modifier) {
        const auto id = modifier.id;
        const auto weight = modifier.weight;
        if (!modifiers_.registerDefinition(std::move(modifier))) {
            return false;
        }
        return tower_.registerReward({id, weight, 0});
    }

    bool registerRun(RunDefinition run) {
        if (run.id == 0 || run.waves.empty() ||
            std::any_of(run.waves.begin(), run.waves.end(),
                        [](tower_defense::WaveId id) {
                            return id == 0;
                        })) {
            return false;
        }
        replaceById(runs_, std::move(run));
        return true;
    }

    void setResources(std::int32_t available) noexcept {
        tower_.setResources(available);
    }

    void setPlayerTeam(std::uint32_t teamId) noexcept {
        playerTeamId_ = teamId;
        tower_.setPlayerTeam(teamId);
        synchronizeGameplayModifiers();
    }

    void setRequiredRoute(gameplay::GridPoint start,
                          gameplay::GridPoint goal) noexcept {
        tower_.setRequiredRoute(start, goal);
    }

    bool setBlocked(gameplay::GridPoint point, bool blocked) {
        return tower_.setBlocked(point, blocked);
    }

    ecs::Entity createBaseCore(gameplay::Position position,
                               std::uint32_t teamId,
                               gameplay::CombatStats combat) {
        return tower_.createBaseCore(position, teamId, combat);
    }

    ecs::Entity createDefender(gameplay::Position position,
                               gameplay::MoveSpeed speed,
                               std::uint32_t teamId,
                               gameplay::CombatStats combat) {
        return tower_.createDefender(position, speed, teamId, combat);
    }

    bool submit(TickCommand command) {
        return commands_.submit(command);
    }

    bool submitRts(gameplay::TickCommand command) {
        return tower_.submitRts(command);
    }

    bool step(std::uint64_t tick) {
        if (hasStepped_ && tick <= lastTick_) return false;
        hasStepped_ = true;
        lastTick_ = tick;
        events_.clear();

        for (const auto& command : commands_.consume(tick)) {
            processCommand(tick, command);
        }

        if (state_.phase == RunPhase::BetweenWaves &&
            tick >= nextWaveTick_) {
            beginCurrentWave(tick);
        }

        if (!tower_.step(tick)) return false;
        reconcileTowerEvents(tick);
        buildSnapshot(tick);
        return true;
    }

    const RunSnapshot& snapshot() const noexcept { return snapshot_; }
    const RunState& state() const noexcept { return state_; }
    const std::vector<Event>& events() const noexcept { return events_; }
    const ModifierRuntime& modifiers() const noexcept { return modifiers_; }
    const tower_defense::TowerDefenseSimulation& tower() const noexcept {
        return tower_;
    }

private:
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

    template<class T>
    static const T* findById(const std::vector<T>& values,
                             std::uint32_t id) noexcept {
        const auto iterator = std::lower_bound(
            values.begin(), values.end(), id,
            [](const T& current, std::uint32_t key) {
                return current.id < key;
            });
        return iterator != values.end() && iterator->id == id
            ? &*iterator
            : nullptr;
    }

    void processCommand(std::uint64_t tick,
                        const TickCommand& command) {
        if (command.type == CommandType::StartRun) {
            startRun(tick, command.objectId);
        } else {
            chooseModifier(tick, command.objectId);
        }
    }

    void startRun(std::uint64_t tick, RunId id) {
        if (state_.phase != RunPhase::Idle) {
            reject(tick, id, RunFailure::AlreadyActive);
            return;
        }
        const auto* run = findById(runs_, id);
        if (!run) {
            reject(tick, id, RunFailure::UnknownRun);
            return;
        }
        for (const auto waveId : run->waves) {
            if (!findById(waves_, waveId)) {
                reject(tick, id, RunFailure::InvalidDefinition);
                return;
            }
        }

        state_ = {id, RunPhase::BetweenWaves, 0, 0,
                  run->waves.front()};
        nextWaveTick_ = tick;
        events_.push_back(
            {tick, EventType::RunStarted, id,
             state_.currentWave, 0, 0, 0});
    }

    void chooseModifier(std::uint64_t tick, ModifierId id) {
        if (state_.phase != RunPhase::RewardPending ||
            std::find(tower_.director().offer().choices.begin(),
                      tower_.director().offer().choices.end(), id) ==
                tower_.director().offer().choices.end()) {
            events_.push_back(
                {tick, EventType::ModifierRejected, state_.runId,
                 state_.currentWave, id, 0,
                 static_cast<std::uint32_t>(
                     RunFailure::ModifierNotOffered)});
            return;
        }
        const auto failure = modifiers_.canApply(id);
        if (failure != ApplyFailure::None) {
            events_.push_back(
                {tick, EventType::ModifierRejected, state_.runId,
                 state_.currentWave, id, 0,
                 static_cast<std::uint32_t>(failure)});
            return;
        }

        tower_defense::TickCommand command;
        command.targetTick = tick;
        command.issuer = internalIssuer(state_.runId);
        command.sequence = nextInternalSequence_++;
        command.type = tower_defense::CommandType::ChooseReward;
        command.objectId = id;
        if (!tower_.submit(command)) {
            events_.push_back(
                {tick, EventType::ModifierRejected, state_.runId,
                 state_.currentWave, id, 0,
                 static_cast<std::uint32_t>(
                     RunFailure::TowerDefenseRejected)});
        }
    }

    void beginCurrentWave(std::uint64_t tick) {
        const auto* run = findById(runs_, state_.runId);
        if (!run || state_.waveIndex >= run->waves.size()) {
            failRun(tick, RunFailure::InvalidDefinition);
            return;
        }
        const auto waveId = run->waves[state_.waveIndex];
        const auto* baseWave = findById(waves_, waveId);
        if (!baseWave) {
            failRun(tick, RunFailure::InvalidDefinition);
            return;
        }

        auto wave = *baseWave;
        auto rewardPool = wave.rewardPool.empty()
            ? modifiers_.definitionIds()
            : wave.rewardPool;
        wave.rewardPool = modifiers_.eligible(std::move(rewardPool));
        wave.rewardChoices = static_cast<std::uint32_t>(
            std::min<std::size_t>(wave.rewardChoices,
                                  wave.rewardPool.size()));
        if (!tower_.registerWave(wave)) {
            failRun(tick, RunFailure::InvalidDefinition);
            return;
        }

        tower_defense::TickCommand command;
        command.targetTick = tick;
        command.issuer = internalIssuer(state_.runId);
        command.sequence = nextInternalSequence_++;
        command.type = tower_defense::CommandType::StartWave;
        command.objectId = waveId;
        if (!tower_.submit(command)) {
            failRun(tick, RunFailure::TowerDefenseRejected);
            return;
        }
        state_.currentWave = waveId;
        nextWaveTick_ = std::numeric_limits<std::uint64_t>::max();
    }

    void reconcileTowerEvents(std::uint64_t tick) {
        for (const auto& event : tower_.events()) {
            switch (event.type) {
            case tower_defense::EventType::WaveStarted:
                state_.phase = RunPhase::WaveActive;
                state_.currentWave = event.waveId;
                events_.push_back(
                    {tick, EventType::WaveStarted, state_.runId,
                     event.waveId, 0, 0, 0});
                break;
            case tower_defense::EventType::WaveRejected:
                failRun(tick, RunFailure::TowerDefenseRejected);
                break;
            case tower_defense::EventType::BaseCoreDestroyed:
                failRun(tick, RunFailure::TowerDefenseRejected);
                break;
            case tower_defense::EventType::WaveCompleted:
                onWaveCompleted(tick, event.waveId);
                break;
            case tower_defense::EventType::RewardChosen:
                onModifierChosen(tick, event.objectId);
                break;
            case tower_defense::EventType::RewardRejected:
                events_.push_back(
                    {tick, EventType::ModifierRejected, state_.runId,
                     state_.currentWave, event.objectId, 0,
                     event.reason});
                break;
            default:
                break;
            }
        }
    }

    void onWaveCompleted(std::uint64_t tick,
                         tower_defense::WaveId waveId) {
        const auto bonus = modifiers_.resolve(
            WaveCompletionResourceStat(), 0);
        if (bonus != 0) {
            const auto next = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(
                    tower_.resources().available) + bonus,
                0,
                std::numeric_limits<std::int32_t>::max());
            tower_.setResources(static_cast<std::int32_t>(next));
            events_.push_back(
                {tick, EventType::ResourceBonusGranted,
                 state_.runId, waveId, 0, bonus, 0});
        }

        events_.push_back(
            {tick, EventType::WaveCompleted, state_.runId,
             waveId, 0, 0, 0});
        if (tower_.director().state().phase ==
            tower_defense::WavePhase::RewardPending) {
            state_.phase = RunPhase::RewardPending;
        } else {
            completeCurrentWave(tick);
        }
    }

    void onModifierChosen(std::uint64_t tick, ModifierId id) {
        const auto result = modifiers_.apply(id);
        if (result.accepted) {
            synchronizeGameplayModifiers();
            events_.push_back(
                {tick, EventType::ModifierApplied, state_.runId,
                 state_.currentWave, id,
                 static_cast<std::int32_t>(result.stacks), 0});
        } else {
            events_.push_back(
                {tick, EventType::ModifierRejected, state_.runId,
                 state_.currentWave, id, 0,
                 static_cast<std::uint32_t>(result.failure)});
        }
        completeCurrentWave(tick);
    }

    void completeCurrentWave(std::uint64_t tick) {
        const auto* run = findById(runs_, state_.runId);
        if (!run) {
            failRun(tick, RunFailure::UnknownRun);
            return;
        }

        ++state_.completedWaves;
        ++state_.waveIndex;
        if (state_.waveIndex >= run->waves.size()) {
            state_.phase = RunPhase::Complete;
            state_.currentWave = 0;
            events_.push_back(
                {tick, EventType::RunCompleted, state_.runId,
                 0, 0, 0, 0});
            return;
        }

        state_.phase = RunPhase::BetweenWaves;
        state_.currentWave = run->waves[state_.waveIndex];
        nextWaveTick_ = tick + 1;
        events_.push_back(
            {tick, EventType::WaveAdvanced, state_.runId,
             state_.currentWave, 0, 0, 0});
    }

    void reject(std::uint64_t tick, RunId id,
                RunFailure failure) {
        events_.push_back(
            {tick, EventType::RunRejected, id, 0, 0, 0,
             static_cast<std::uint32_t>(failure)});
    }

    void failRun(std::uint64_t tick, RunFailure failure) {
        if (state_.phase == RunPhase::Failed ||
            state_.phase == RunPhase::Complete) {
            return;
        }
        state_.phase = RunPhase::Failed;
        events_.push_back(
            {tick, EventType::RunFailed, state_.runId,
             state_.currentWave, 0, 0,
             static_cast<std::uint32_t>(failure)});
    }

    void synchronizeGameplayModifiers() {
        tower_.setTeamModifierProfile(
            playerTeamId_, ResolveGameplayProfile(modifiers_));
    }

    void buildSnapshot(std::uint64_t tick) {
        snapshot_.tick = tick;
        snapshot_.towerDefenseWorldHash =
            tower_.snapshot().worldHash;
        snapshot_.state = state_;
        snapshot_.availableResources =
            tower_.resources().available;
        snapshot_.waveCompletionResourceBonus =
            modifiers_.resolve(WaveCompletionResourceStat(), 0);
        snapshot_.gameplayProfile =
            ResolveGameplayProfile(modifiers_);
        snapshot_.modifiers = modifiers_.stacks();

        foundation::CanonicalHash hash;
        hash.WriteU64(tick);
        hash.WriteU64(rootSeed_);
        hash.WriteU64(snapshot_.towerDefenseWorldHash);
        hash.WriteU32(state_.runId);
        hash.WriteU8(static_cast<std::uint8_t>(state_.phase));
        hash.WriteU32(state_.waveIndex);
        hash.WriteU32(state_.completedWaves);
        hash.WriteU32(state_.currentWave);
        hash.WriteU64(nextWaveTick_);
        hash.WriteU32(nextInternalSequence_);
        hash.WriteI32(snapshot_.availableResources);
        hash.WriteI32(snapshot_.waveCompletionResourceBonus);
        hash.WriteU32(playerTeamId_);
        hash.WriteI32(snapshot_.gameplayProfile.unitHealth);
        hash.WriteI32(snapshot_.gameplayProfile.unitDamage);
        hash.WriteI32(snapshot_.gameplayProfile.unitArmorAdd);
        hash.WriteI32(snapshot_.gameplayProfile.unitMoveSpeed);
        hash.WriteI32(snapshot_.gameplayProfile.buildingHealth);
        hash.WriteI32(snapshot_.gameplayProfile.buildingDamage);
        hash.WriteI32(snapshot_.gameplayProfile.constructionSpeed);
        hash.WriteI32(snapshot_.gameplayProfile.productionSpeed);
        hash.WriteI32(snapshot_.gameplayProfile.bountyMultiplier);

        const auto* run = findById(runs_, state_.runId);
        hash.WriteBool(run != nullptr);
        if (run) {
            hash.WriteU32(
                static_cast<std::uint32_t>(run->waves.size()));
            for (const auto waveId : run->waves) {
                hash.WriteU32(waveId);
            }
        }
        modifiers_.appendHash(hash);
        snapshot_.worldHash = hash.Value();
    }

    static std::uint32_t internalIssuer(RunId runId) noexcept {
        return 0xc0000000u | (runId & 0x3fffffffu);
    }

    tower_defense::TowerDefenseSimulation tower_;
    ModifierRuntime modifiers_;
    TickCommandStream commands_;
    std::vector<RunDefinition> runs_;
    std::vector<tower_defense::WaveDefinition> waves_;
    std::vector<Event> events_;
    RunSnapshot snapshot_;
    RunState state_;
    std::uint64_t rootSeed_{1};
    std::uint64_t nextWaveTick_{};
    std::uint64_t lastTick_{};
    std::uint32_t nextInternalSequence_{1};
    std::uint32_t playerTeamId_{1};
    bool hasStepped_{};
};

} // namespace rts::roguelite
