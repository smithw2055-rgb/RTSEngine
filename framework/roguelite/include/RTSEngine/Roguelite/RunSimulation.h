#pragma once

#include <RTSEngine/Roguelite/GameplayStats.h>
#include <RTSEngine/Roguelite/ModifierRuntime.h>
#include <RTSEngine/Roguelite/RewardRarityPlanner.h>
#include <RTSEngine/Roguelite/RunHistory.h>
#include <RTSEngine/TowerDefense/Simulation.h>
#include <RTSEngine/TowerDefense/WaveSequence.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/AuthoritativeStep.h>
#include <rts/sim/DeterministicCommandStream.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::roguelite {

class RunSimulationArchive;

struct RunDefinition {
    RunId id{};
    std::vector<tower_defense::WaveId> waves;
    std::vector<RewardOfferRule> rewardRules;
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

using TickCommandStream = sim::DeterministicCommandStream<TickCommand>;

enum class RunFailure : std::uint8_t {
    None,
    UnknownRun,
    AlreadyActive,
    InvalidDefinition,
    ModifierNotOffered,
    ModifierIneligible,
    TowerDefenseRejected,
    RewardPolicyUnsatisfied
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
    std::uint32_t rewardPityMisses{};
    RunHistory history{};
};

class RunSimulation {
public:
    RunSimulation(std::int32_t width = 32,
                  std::int32_t height = 32,
                  std::uint64_t rootSeed = 1);
    ~RunSimulation();

    RunSimulation(const RunSimulation&) = delete;
    RunSimulation& operator=(const RunSimulation&) = delete;
    RunSimulation(RunSimulation&&) = delete;
    RunSimulation& operator=(RunSimulation&&) = delete;

    bool configurationFrozen() const noexcept {
        return hasStepped_ || tower_.configurationFrozen();
    }

    void freezeConfiguration() noexcept {
        tower_.freezeConfiguration();
    }

    bool registerUnit(gameplay::UnitDefinition definition) {
        return !configurationFrozen() &&
               tower_.registerUnit(std::move(definition));
    }

    bool registerBuilding(gameplay::BuildingDefinition definition) {
        return !configurationFrozen() &&
               tower_.registerBuilding(std::move(definition));
    }

    bool registerAffix(tower_defense::WaveAffixDefinition affix) {
        return !configurationFrozen() &&
               tower_.registerAffix(std::move(affix));
    }

    bool registerBoss(tower_defense::BossDefinition boss) {
        return !configurationFrozen() &&
               tower_.registerBoss(std::move(boss));
    }

    bool registerLane(tower_defense::SpawnLane lane) {
        return !configurationFrozen() &&
               tower_.registerLane(std::move(lane));
    }

    bool registerWave(tower_defense::WaveDefinition wave) {
        if (configurationFrozen() || wave.id == 0) return false;
        const auto copy = wave;
        if (!tower_.registerWave(std::move(wave))) return false;
        replaceById(waves_, copy);
        return true;
    }

    bool registerModifier(ModifierDefinition modifier) {
        if (configurationFrozen()) return false;
        const auto id = modifier.id;
        const auto weight = modifier.weight;
        if (!modifiers_.registerDefinition(std::move(modifier))) return false;
        return tower_.registerReward({id, weight, 0});
    }

    bool registerRun(RunDefinition run) {
        if (configurationFrozen() || !validRunDefinition(run)) return false;
        const auto sequenceDefinition = makeSequenceDefinition(run);
        if (!sequence_.registerSequence(sequenceDefinition)) return false;
        replaceById(runs_, std::move(run));
        return true;
    }

    void setResources(std::int32_t available) noexcept {
        tower_.setResources(available);
    }

    bool setPlayerTeam(std::uint32_t teamId) noexcept {
        if (configurationFrozen() || !tower_.setPlayerTeam(teamId)) {
            return false;
        }
        playerTeamId_ = teamId;
        synchronizeGameplayModifiers();
        return true;
    }

    bool setRequiredRoute(gameplay::GridPoint start,
                          gameplay::GridPoint goal) noexcept {
        return !configurationFrozen() &&
               tower_.setRequiredRoute(start, goal);
    }

    bool setBlocked(gameplay::GridPoint point, bool blocked) {
        return !configurationFrozen() && tower_.setBlocked(point, blocked);
    }

    ecs::Entity createBaseCore(gameplay::Position position,
                               std::uint32_t teamId,
                               gameplay::CombatStats combat) {
        if (configurationFrozen()) return {};
        return tower_.createBaseCore(position, teamId, combat);
    }

    ecs::Entity createDefender(gameplay::Position position,
                               gameplay::MoveSpeed speed,
                               std::uint32_t teamId,
                               gameplay::CombatStats combat) {
        if (configurationFrozen()) return {};
        return tower_.createDefender(position, speed, teamId, combat);
    }

    bool submit(TickCommand command) {
        if (isInternalIssuer(command.issuer)) return false;
        return commands_.submit(std::move(command));
    }

    bool submitRts(gameplay::TickCommand command) {
        return tower_.submitRts(std::move(command));
    }

    sim::AuthoritativeStepValidation validateStep(
        std::uint64_t tick) const noexcept {
        const auto outer = sim::ValidateAuthoritativeStep(
            hasStepped_, lastTick_, tick);
        if (!outer) return outer;
        const auto inner = tower_.validateStep(tick);
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
        if (sequence_.due(tick)) beginCurrentWave(tick);
        if (!tower_.step(tick)) return false;
        reconcileTowerEvents(tick);
        synchronizeRunState();
        buildSnapshot(tick);
        hasStepped_ = true;
        lastTick_ = tick;
        return true;
    }

    std::uint64_t nextExpectedTick() const noexcept {
        return hasStepped_ ? lastTick_ + 1u : 0u;
    }

    const RunSnapshot& snapshot() const noexcept { return snapshot_; }
    const RunState& state() const noexcept { return state_; }
    const RunHistory& history() const noexcept { return history_; }
    const std::vector<Event>& events() const noexcept { return events_; }
    const ModifierRuntime& modifiers() const noexcept { return modifiers_; }
    const tower_defense::WaveSequenceDirector& waveSequence() const noexcept {
        return sequence_;
    }
    const tower_defense::TowerDefenseSimulation& tower() const noexcept {
        return tower_;
    }
    TickCommandStream::State commandStreamState() const {
        return commands_.snapshot();
    }
    std::uint64_t rootSeed() const noexcept { return rootSeed_; }
    std::uint64_t lastTick() const noexcept { return lastTick_; }

private:
    friend class RunSimulationArchive;

    using TowerRuntimeAuthority =
        tower_defense::TowerDefenseSimulation::RuntimeAuthority;

    struct PendingWaveBaseline final {
        tower_defense::WaveId waveId{};
        std::uint32_t waveIndex{};
        std::uint64_t startedTick{};
        std::int32_t coreHealth{};
        std::int32_t coreMaximum{};
        std::int32_t resources{};
        bool valid{};
    };

    static TowerRuntimeAuthority towerRuntimeAuthority() noexcept {
        return TowerRuntimeAuthority{};
    }

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
            ? &*iterator : nullptr;
    }

    static bool validRunDefinition(const RunDefinition& run) noexcept {
        if (run.id == 0 || run.waves.empty() ||
            std::any_of(
                run.waves.begin(), run.waves.end(),
                [](tower_defense::WaveId id) { return id == 0; })) {
            return false;
        }
        if (!run.rewardRules.empty() &&
            run.rewardRules.size() != run.waves.size()) {
            return false;
        }
        return std::all_of(
            run.rewardRules.begin(), run.rewardRules.end(),
            [](const RewardOfferRule& rule) { return rule.valid(); });
    }

    static tower_defense::WaveSequenceDefinition makeSequenceDefinition(
        const RunDefinition& run) {
        return {run.id, run.waves, 0u, 1u, false};
    }

    static const RewardOfferRule* rewardRule(
        const RunDefinition& run,
        std::uint32_t waveIndex) noexcept {
        return run.rewardRules.empty() || waveIndex >= run.rewardRules.size()
            ? nullptr : &run.rewardRules[waveIndex];
    }

    static RunState projectSequenceState(
        const tower_defense::WaveSequenceState& sequence) noexcept {
        RunState result;
        result.runId = sequence.sequenceId;
        result.waveIndex = sequence.waveIndex;
        result.completedWaves = sequence.completedWaves;
        result.currentWave = sequence.currentWave;
        switch (sequence.phase) {
        case tower_defense::WaveSequencePhase::Idle:
            result = {};
            break;
        case tower_defense::WaveSequencePhase::Preparing:
        case tower_defense::WaveSequencePhase::StartingWave:
            result.phase = RunPhase::BetweenWaves;
            break;
        case tower_defense::WaveSequencePhase::WaveActive:
            result.phase = RunPhase::WaveActive;
            break;
        case tower_defense::WaveSequencePhase::RewardPending:
            result.phase = RunPhase::RewardPending;
            break;
        case tower_defense::WaveSequencePhase::Complete:
            result.phase = RunPhase::Complete;
            break;
        case tower_defense::WaveSequencePhase::Failed:
            result.phase = RunPhase::Failed;
            break;
        }
        return result;
    }

    void synchronizeRunState() noexcept {
        state_ = projectSequenceState(sequence_.state());
    }

    std::uint64_t legacyNextWaveTick() const noexcept {
        const auto& value = sequence_.state();
        switch (value.phase) {
        case tower_defense::WaveSequencePhase::Idle:
            return 0;
        case tower_defense::WaveSequencePhase::Preparing:
            return value.scheduledStartTick;
        case tower_defense::WaveSequencePhase::Failed:
            if (tower_.director().state().phase ==
                    tower_defense::WavePhase::Failed ||
                value.preparationStartedTick ==
                    tower_defense::WaveSequenceDirector::noScheduledTick()) {
                return tower_defense::WaveSequenceDirector::noScheduledTick();
            }
            if (value.waveIndex == 0) return value.preparationStartedTick;
            if (value.preparationStartedTick ==
                std::numeric_limits<std::uint64_t>::max()) {
                return value.preparationStartedTick;
            }
            return value.preparationStartedTick + 1u;
        default:
            return tower_defense::WaveSequenceDirector::noScheduledTick();
        }
    }

    void processCommand(std::uint64_t tick, const TickCommand& command) {
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

        const auto result = sequence_.start(id, tick);
        if (!result.accepted) {
            reject(tick, id, RunFailure::InvalidDefinition);
            return;
        }
        history_ = {};
        history_.runId = id;
        history_.startedTick = tick;
        history_.phase = RunHistoryPhase::Active;
        pendingWave_ = {};
        pendingRewardOffer_ = {};
        synchronizeRunState();
        events_.push_back(
            {tick, EventType::RunStarted, id, state_.currentWave, 0, 0, 0});
    }

    void chooseModifier(std::uint64_t tick, ModifierId id) {
        if (state_.phase != RunPhase::RewardPending ||
            std::find(tower_.director().offer().choices.begin(),
                      tower_.director().offer().choices.end(), id) ==
                tower_.director().offer().choices.end()) {
            events_.push_back(
                {tick, EventType::ModifierRejected, state_.runId,
                 state_.currentWave, id, 0,
                 static_cast<std::uint32_t>(RunFailure::ModifierNotOffered)});
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
        if (!tower_.submitRuntime(towerRuntimeAuthority(), command)) {
            events_.push_back(
                {tick, EventType::ModifierRejected, state_.runId,
                 state_.currentWave, id, 0,
                 static_cast<std::uint32_t>(RunFailure::TowerDefenseRejected)});
        }
    }

    std::vector<ModifierDefinition> rewardCandidates(
        const std::vector<ModifierId>& ids) const {
        std::vector<ModifierDefinition> result;
        result.reserve(ids.size());
        for (const auto id : ids) {
            const auto* value = modifiers_.definition(id);
            if (value) result.push_back(*value);
        }
        return result;
    }

    void beginCurrentWave(std::uint64_t tick) {
        const auto& sequenceState = sequence_.state();
        const auto* run = findById(runs_, sequenceState.sequenceId);
        if (!run || sequenceState.waveIndex >= run->waves.size() ||
            sequenceState.currentWave != run->waves[sequenceState.waveIndex]) {
            failRun(tick, RunFailure::InvalidDefinition,
                    tower_defense::WaveSequenceFailure::InvalidDefinition);
            return;
        }
        const auto waveId = sequenceState.currentWave;
        const auto* baseWave = findById(waves_, waveId);
        if (!baseWave) {
            failRun(tick, RunFailure::InvalidDefinition,
                    tower_defense::WaveSequenceFailure::UnknownWave);
            return;
        }

        auto wave = *baseWave;
        auto rewardPool = wave.rewardPool.empty()
            ? modifiers_.definitionIds() : wave.rewardPool;
        wave.rewardPool = modifiers_.eligible(std::move(rewardPool));
        wave.rewardChoices = static_cast<std::uint32_t>(
            std::min<std::size_t>(wave.rewardChoices, wave.rewardPool.size()));

        pendingRewardOffer_ = {};
        const auto* rule = rewardRule(*run, sequenceState.waveIndex);
        if (rule && rule->enabled()) {
            pendingRewardOffer_ = RewardRarityPlanner::plan(
                rootSeed_, run->id, sequenceState.waveIndex, waveId,
                *rule, history_.rewardPityMisses,
                rewardCandidates(wave.rewardPool), wave.rewardChoices);
            if (!pendingRewardOffer_.accepted) {
                failRun(tick, RunFailure::RewardPolicyUnsatisfied,
                        tower_defense::WaveSequenceFailure::InvalidDefinition);
                return;
            }
            wave.rewardPool = pendingRewardOffer_.choices;
            wave.rewardChoices = static_cast<std::uint32_t>(
                pendingRewardOffer_.choices.size());
        }

        if (!tower_.registerRuntimeWave(
                towerRuntimeAuthority(), std::move(wave)) ||
            !sequence_.markWaveStartQueued()) {
            pendingRewardOffer_ = {};
            failRun(tick, RunFailure::InvalidDefinition,
                    tower_defense::WaveSequenceFailure::InvalidDefinition);
            return;
        }

        tower_defense::TickCommand command;
        command.targetTick = tick;
        command.issuer = internalIssuer(sequenceState.sequenceId);
        command.sequence = nextInternalSequence_++;
        command.type = tower_defense::CommandType::StartWave;
        command.objectId = waveId;
        if (!tower_.submitRuntime(towerRuntimeAuthority(), command)) {
            pendingRewardOffer_ = {};
            failRun(tick, RunFailure::TowerDefenseRejected,
                    tower_defense::WaveSequenceFailure::TowerCommandRejected);
            return;
        }

        pendingWave_ = {};
        pendingWave_.waveId = waveId;
        pendingWave_.waveIndex = sequenceState.waveIndex;
        pendingWave_.startedTick = tick;
        pendingWave_.coreHealth = tower_.snapshot().coreHealthCurrent;
        pendingWave_.coreMaximum = tower_.snapshot().coreHealthMaximum;
        pendingWave_.resources = tower_.resources().available;
        pendingWave_.valid = true;
        synchronizeRunState();
    }

    void reconcileTowerEvents(std::uint64_t tick) {
        for (const auto& event : tower_.events()) {
            switch (event.type) {
            case tower_defense::EventType::WaveStarted:
                if (!sequence_.markWaveStarted(event.waveId)) {
                    failRun(tick, RunFailure::TowerDefenseRejected,
                            tower_defense::WaveSequenceFailure::WaveRejected);
                    break;
                }
                beginWaveHistory(tick, event.waveId);
                synchronizeRunState();
                events_.push_back(
                    {tick, EventType::WaveStarted, state_.runId,
                     event.waveId, 0, 0, 0});
                break;
            case tower_defense::EventType::EnemyDefeated:
                recordEnemyDefeated(event.waveId, event.reason != 0);
                break;
            case tower_defense::EventType::WaveRejected:
                pendingWave_ = {};
                pendingRewardOffer_ = {};
                failRun(tick, RunFailure::TowerDefenseRejected,
                        tower_defense::WaveSequenceFailure::WaveRejected);
                break;
            case tower_defense::EventType::BaseCoreDestroyed:
                failRun(tick, RunFailure::TowerDefenseRejected,
                        tower_defense::WaveSequenceFailure::BaseCoreDestroyed);
                break;
            case tower_defense::EventType::WaveCompleted:
                onWaveCompleted(tick, event.waveId);
                break;
            case tower_defense::EventType::RewardOffered:
                recordRewardOffered(event.waveId, event.objectId);
                break;
            case tower_defense::EventType::RewardChosen:
                onModifierChosen(tick, event.objectId);
                break;
            case tower_defense::EventType::RewardRejected:
                events_.push_back(
                    {tick, EventType::ModifierRejected, state_.runId,
                     state_.currentWave, event.objectId, 0, event.reason});
                break;
            default:
                break;
            }
        }
    }

    void beginWaveHistory(std::uint64_t tick,
                          tower_defense::WaveId waveId) {
        WaveResult result;
        result.waveId = waveId;
        result.waveIndex = sequence_.state().waveIndex;
        result.startedTick = tick;
        result.resourcesStart = tower_.resources().available;
        result.resourcesEnd = result.resourcesStart;
        result.coreHealthStart = tower_.snapshot().coreHealthCurrent;
        result.coreHealthEnd = result.coreHealthStart;
        result.coreHealthMaximum = tower_.snapshot().coreHealthMaximum;
        if (pendingWave_.valid && pendingWave_.waveId == waveId &&
            pendingWave_.waveIndex == result.waveIndex) {
            result.startedTick = pendingWave_.startedTick;
            result.resourcesStart = pendingWave_.resources;
            result.resourcesEnd = pendingWave_.resources;
            if (pendingWave_.coreMaximum > 0) {
                result.coreHealthStart = pendingWave_.coreHealth;
                result.coreHealthEnd = pendingWave_.coreHealth;
                result.coreHealthMaximum = pendingWave_.coreMaximum;
            }
        }

        const auto& plan = tower_.director().plan();
        result.plannedEnemies = static_cast<std::uint32_t>(plan.spawns.size());
        for (const auto& affix : plan.affixes) {
            result.affixes.push_back(affix.id);
        }
        for (const auto& spawn : plan.spawns) {
            if (spawn.bossId != 0) result.bosses.push_back(spawn.bossId);
        }
        result.plannedBosses = static_cast<std::uint32_t>(result.bosses.size());

        if (pendingRewardOffer_.accepted) {
            result.rewardRarityBudget = pendingRewardOffer_.rarityBudget;
            result.rewardRaritySpent = pendingRewardOffer_.raritySpent;
            result.guaranteedRarity = pendingRewardOffer_.guaranteedRarity;
            result.effectiveGuaranteedRarity =
                pendingRewardOffer_.effectiveGuaranteedRarity;
            result.pityBefore = pendingRewardOffer_.pityBefore;
            result.pityAfter = pendingRewardOffer_.pityAfter;
            result.pityTriggered = pendingRewardOffer_.pityTriggered;
            result.rewardChoices = pendingRewardOffer_.choices;
            result.rewardRarities = pendingRewardOffer_.rarities;
        }

        history_.waves.push_back(std::move(result));
        pendingWave_ = {};
        pendingRewardOffer_ = {};
    }

    WaveResult* currentWaveResult(
        tower_defense::WaveId waveId = 0) noexcept {
        if (history_.waves.empty()) return nullptr;
        auto& result = history_.waves.back();
        return waveId == 0 || result.waveId == waveId ? &result : nullptr;
    }

    void recordEnemyDefeated(tower_defense::WaveId waveId, bool boss) {
        auto* result = currentWaveResult(waveId);
        if (!result) return;
        if (result->enemiesDefeated < result->plannedEnemies) {
            ++result->enemiesDefeated;
        }
        if (boss && result->bossesDefeated < result->plannedBosses) {
            ++result->bossesDefeated;
        }
    }

    void recordRewardOffered(tower_defense::WaveId waveId, ModifierId id) {
        auto* result = currentWaveResult(waveId);
        if (!result || id == 0) return;
        if (std::find(result->rewardChoices.begin(),
                      result->rewardChoices.end(), id) !=
            result->rewardChoices.end()) {
            return;
        }
        result->rewardChoices.push_back(id);
        const auto* definition = modifiers_.definition(id);
        result->rewardRarities.push_back(
            definition ? definition->rarity : RewardRarity::Common);
    }

    void finalizeWaveResult(std::uint64_t tick,
                            WaveResultPhase phase,
                            std::int32_t resourceBonus) {
        auto* result = currentWaveResult();
        if (!result) return;
        if (result->phase == WaveResultPhase::Failed &&
            phase != WaveResultPhase::Failed) {
            return;
        }
        result->completedTick = tick;
        result->phase = phase;
        result->coreHealthEnd = tower_.snapshot().coreHealthCurrent;
        result->coreHealthMaximum = tower_.snapshot().coreHealthMaximum;
        result->resourcesEnd = tower_.resources().available;
        result->resourceDelta = static_cast<std::int32_t>(
            std::clamp<std::int64_t>(
                static_cast<std::int64_t>(result->resourcesEnd) -
                    result->resourcesStart,
                std::numeric_limits<std::int32_t>::min(),
                std::numeric_limits<std::int32_t>::max()));
        result->resourceBonus = resourceBonus;
    }

    void onWaveCompleted(std::uint64_t tick,
                         tower_defense::WaveId waveId) {
        const auto bonus = modifiers_.resolve(WaveCompletionResourceStat(), 0);
        if (bonus != 0) {
            const auto next = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(tower_.resources().available) + bonus,
                0, std::numeric_limits<std::int32_t>::max());
            tower_.setResources(static_cast<std::int32_t>(next));
            events_.push_back(
                {tick, EventType::ResourceBonusGranted, state_.runId,
                 waveId, 0, bonus, 0});
        }
        events_.push_back(
            {tick, EventType::WaveCompleted, state_.runId,
             waveId, 0, 0, 0});

        const bool rewardPending = tower_.director().state().phase ==
            tower_defense::WavePhase::RewardPending;
        finalizeWaveResult(
            tick,
            rewardPending ? WaveResultPhase::RewardPending
                          : WaveResultPhase::Complete,
            bonus);
        auto* waveResult = currentWaveResult(waveId);
        if (rewardPending && waveResult &&
            waveResult->rewardRarityBudget != 0) {
            history_.rewardPityMisses = waveResult->pityAfter;
        }

        const auto result = sequence_.markWaveCompleted(
            waveId, tick, rewardPending);
        if (!result.accepted) {
            failRun(tick, RunFailure::TowerDefenseRejected,
                    tower_defense::WaveSequenceFailure::WaveRejected);
            return;
        }
        synchronizeRunState();
        if (!rewardPending) publishAdvance(tick);
    }

    void onModifierChosen(std::uint64_t tick, ModifierId id) {
        const auto result = modifiers_.apply(id);
        auto* waveResult = currentWaveResult();
        if (waveResult) {
            waveResult->selectedModifier = id;
            waveResult->modifierApplied = result.accepted;
            waveResult->phase = WaveResultPhase::Complete;
        }
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

        const auto advance = sequence_.markRewardChosen(tick);
        if (!advance.accepted) {
            failRun(tick, RunFailure::TowerDefenseRejected,
                    tower_defense::WaveSequenceFailure::RewardNotPending);
            return;
        }
        synchronizeRunState();
        publishAdvance(tick);
    }

    void publishAdvance(std::uint64_t tick) {
        if (state_.phase == RunPhase::Complete) {
            history_.phase = RunHistoryPhase::Complete;
            history_.finishedTick = tick;
            events_.push_back(
                {tick, EventType::RunCompleted, state_.runId, 0, 0, 0, 0});
        } else if (state_.phase == RunPhase::BetweenWaves) {
            events_.push_back(
                {tick, EventType::WaveAdvanced, state_.runId,
                 state_.currentWave, 0, 0, 0});
        }
    }

    void reject(std::uint64_t tick, RunId id, RunFailure failure) {
        events_.push_back(
            {tick, EventType::RunRejected, id, 0, 0, 0,
             static_cast<std::uint32_t>(failure)});
    }

    void failRun(
        std::uint64_t tick,
        RunFailure failure,
        tower_defense::WaveSequenceFailure sequenceFailure =
            tower_defense::WaveSequenceFailure::WaveRejected) {
        if (state_.phase == RunPhase::Failed ||
            state_.phase == RunPhase::Complete) return;
        (void)sequence_.fail(sequenceFailure);
        synchronizeRunState();
        pendingWave_ = {};
        pendingRewardOffer_ = {};
        finalizeWaveResult(tick, WaveResultPhase::Failed, 0);
        if (history_.phase != RunHistoryPhase::Idle) {
            history_.phase = RunHistoryPhase::Failed;
            history_.finishedTick = tick;
        }
        events_.push_back(
            {tick, EventType::RunFailed, state_.runId,
             state_.currentWave, 0, 0,
             static_cast<std::uint32_t>(failure)});
    }

    void synchronizeGameplayModifiers() {
        tower_.setTeamModifierProfile(
            playerTeamId_, ResolveGameplayProfile(modifiers_));
    }

    static void hashCommand(foundation::CanonicalHash& hash,
                            const TickCommand& command) {
        hash.WriteU64(command.targetTick);
        hash.WriteU32(command.issuer);
        hash.WriteU32(command.sequence);
        hash.WriteU8(static_cast<std::uint8_t>(command.type));
        hash.WriteU32(command.objectId);
    }

    static void appendRewardRuleHash(
        foundation::CanonicalHash& hash,
        const RewardOfferRule& rule) {
        hash.WriteU32(rule.rarityBudget);
        hash.WriteU8(static_cast<std::uint8_t>(rule.guaranteedRarity));
        hash.WriteU32(rule.pityAfterMisses);
        hash.WriteU8(static_cast<std::uint8_t>(rule.pityRarity));
    }

    static void appendHistoryHash(foundation::CanonicalHash& hash,
                                  const RunHistory& history,
                                  std::uint16_t version) {
        hash.WriteU32(history.runId);
        hash.WriteU64(history.startedTick);
        hash.WriteU64(history.finishedTick);
        hash.WriteU8(static_cast<std::uint8_t>(history.phase));
        hash.WriteBool(history.legacyImported);
        if (version >= 3u) hash.WriteU32(history.rewardPityMisses);
        hash.WriteU32(static_cast<std::uint32_t>(history.waves.size()));
        for (const auto& wave : history.waves) {
            hash.WriteU32(wave.waveId);
            hash.WriteU32(wave.waveIndex);
            hash.WriteU64(wave.startedTick);
            hash.WriteU64(wave.completedTick);
            hash.WriteU8(static_cast<std::uint8_t>(wave.phase));
            hash.WriteU32(wave.plannedEnemies);
            hash.WriteU32(wave.plannedBosses);
            hash.WriteU32(wave.enemiesDefeated);
            hash.WriteU32(wave.bossesDefeated);
            hash.WriteI32(wave.coreHealthStart);
            hash.WriteI32(wave.coreHealthEnd);
            hash.WriteI32(wave.coreHealthMaximum);
            hash.WriteI32(wave.resourcesStart);
            hash.WriteI32(wave.resourcesEnd);
            hash.WriteI32(wave.resourceDelta);
            hash.WriteI32(wave.resourceBonus);
            hash.WriteU32(static_cast<std::uint32_t>(wave.affixes.size()));
            for (const auto id : wave.affixes) hash.WriteU32(id);
            hash.WriteU32(static_cast<std::uint32_t>(wave.bosses.size()));
            for (const auto id : wave.bosses) hash.WriteU32(id);
            hash.WriteU32(static_cast<std::uint32_t>(wave.rewardChoices.size()));
            for (const auto id : wave.rewardChoices) hash.WriteU32(id);
            hash.WriteU32(wave.selectedModifier);
            hash.WriteBool(wave.modifierApplied);
            if (version >= 3u) {
                hash.WriteU32(wave.rewardRarityBudget);
                hash.WriteU32(wave.rewardRaritySpent);
                hash.WriteU8(static_cast<std::uint8_t>(
                    wave.guaranteedRarity));
                hash.WriteU8(static_cast<std::uint8_t>(
                    wave.effectiveGuaranteedRarity));
                hash.WriteU32(wave.pityBefore);
                hash.WriteU32(wave.pityAfter);
                hash.WriteBool(wave.pityTriggered);
                hash.WriteU32(static_cast<std::uint32_t>(
                    wave.rewardRarities.size()));
                for (const auto rarity : wave.rewardRarities) {
                    hash.WriteU8(static_cast<std::uint8_t>(rarity));
                }
            }
        }
    }

    void buildSnapshot(std::uint64_t tick,
                       std::uint16_t compatibilityVersion = 3u) {
        snapshot_.tick = tick;
        snapshot_.towerDefenseWorldHash = tower_.snapshot().worldHash;
        snapshot_.state = state_;
        snapshot_.availableResources = tower_.resources().available;
        snapshot_.waveCompletionResourceBonus =
            modifiers_.resolve(WaveCompletionResourceStat(), 0);
        snapshot_.gameplayProfile = ResolveGameplayProfile(modifiers_);
        snapshot_.modifiers = modifiers_.stacks();
        snapshot_.rewardPityMisses = history_.rewardPityMisses;
        snapshot_.history = history_;

        foundation::CanonicalHash hash;
        hash.WriteU64(tick);
        hash.WriteU64(rootSeed_);
        hash.WriteU64(snapshot_.towerDefenseWorldHash);
        hash.WriteU32(state_.runId);
        hash.WriteU8(static_cast<std::uint8_t>(state_.phase));
        hash.WriteU32(state_.waveIndex);
        hash.WriteU32(state_.completedWaves);
        hash.WriteU32(state_.currentWave);
        hash.WriteU64(legacyNextWaveTick());
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

        const auto commandState = commands_.snapshot();
        hash.WriteU64(commandState.committedThrough);
        hash.WriteU32(static_cast<std::uint32_t>(commandState.pending.size()));
        for (const auto& command : commandState.pending) {
            hashCommand(hash, command);
        }

        const auto* run = findById(runs_, state_.runId);
        hash.WriteBool(run != nullptr);
        if (run) {
            hash.WriteU32(static_cast<std::uint32_t>(run->waves.size()));
            for (const auto waveId : run->waves) hash.WriteU32(waveId);
            if (compatibilityVersion >= 3u) {
                hash.WriteU32(static_cast<std::uint32_t>(
                    run->rewardRules.size()));
                for (const auto& rule : run->rewardRules) {
                    appendRewardRuleHash(hash, rule);
                }
            }
        }
        modifiers_.appendHash(hash, compatibilityVersion >= 3u);
        if (compatibilityVersion >= 2u) {
            appendHistoryHash(hash, history_, compatibilityVersion);
        }
        snapshot_.worldHash = hash.Value();
    }

    static bool isInternalIssuer(std::uint32_t issuer) noexcept {
        return (issuer & 0xc0000000u) == 0xc0000000u;
    }

    static std::uint32_t internalIssuer(RunId runId) noexcept {
        return 0xc0000000u | (runId & 0x3fffffffu);
    }

    tower_defense::TowerDefenseSimulation tower_;
    tower_defense::WaveSequenceDirector sequence_;
    ModifierRuntime modifiers_;
    TickCommandStream commands_;
    std::vector<RunDefinition> runs_;
    std::vector<tower_defense::WaveDefinition> waves_;
    std::vector<Event> events_;
    RunSnapshot snapshot_;
    RunState state_;
    RunHistory history_;
    PendingWaveBaseline pendingWave_;
    PlannedModifierOffer pendingRewardOffer_;
    std::uint64_t rootSeed_{1};
    std::uint64_t lastTick_{};
    std::uint32_t nextInternalSequence_{1};
    std::uint32_t playerTeamId_{1};
    bool hasStepped_{};
};

} // namespace rts::roguelite
