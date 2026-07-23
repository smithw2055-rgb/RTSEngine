#pragma once

#include <RTSEngine/TowerDefense/Simulation.h>
#include <RTSEngine/TowerDefense/WaveSequence.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/DeterministicCommandStream.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::tower_defense {

class WaveLoopSimulationArchive;

enum class WaveLoopCommandType : std::uint8_t {
    StartSequence,
    StartNextWave,
    ChooseReward
};

struct WaveLoopCommand final {
    std::uint64_t targetTick{};
    std::uint32_t issuer{};
    std::uint32_t sequence{};
    WaveLoopCommandType type{WaveLoopCommandType::StartSequence};
    std::uint32_t objectId{};
};

using WaveLoopCommandStream =
    sim::DeterministicCommandStream<WaveLoopCommand>;

enum class WaveLoopEventType : std::uint8_t {
    SequenceStarted,
    SequenceRejected,
    PreparationStarted,
    PreparationSkipped,
    WaveStarted,
    WaveRejected,
    WaveCompleted,
    RewardOffered,
    RewardChosen,
    RewardRejected,
    BaseCoreDestroyed,
    SequenceCompleted,
    SequenceFailed
};

struct WaveLoopEvent final {
    std::uint64_t tick{};
    WaveLoopEventType type{};
    WaveSequenceId sequenceId{};
    WaveId waveId{};
    std::uint32_t objectId{};
    std::int32_t value{};
    std::uint32_t reason{};
};

struct WaveLoopSnapshot final {
    std::uint64_t tick{};
    std::uint64_t worldHash{};
    std::uint64_t towerDefenseWorldHash{};
    WaveSequenceState sequence{};
    std::uint32_t preparationTicksRemaining{};
    bool canStartEarly{};
};

class WaveLoopSimulation final {
public:
    WaveLoopSimulation(std::int32_t width = 32,
                       std::int32_t height = 32,
                       std::uint64_t rootSeed = 1)
        : tower_(width, height, rootSeed), rootSeed_(rootSeed) {}

    void registerUnit(gameplay::UnitDefinition definition) {
        tower_.registerUnit(std::move(definition));
    }

    void registerBuilding(gameplay::BuildingDefinition definition) {
        tower_.registerBuilding(std::move(definition));
    }

    bool registerAffix(WaveAffixDefinition definition) {
        return tower_.registerAffix(std::move(definition));
    }

    bool registerBoss(BossDefinition definition) {
        return tower_.registerBoss(std::move(definition));
    }

    bool upsertLaneNode(LaneNode node) {
        return tower_.upsertLaneNode(node);
    }

    bool removeLaneNode(LaneNodeId id) {
        return tower_.removeLaneNode(id);
    }

    bool connectLaneNodes(
        LaneNodeId from,
        LaneNodeId to,
        std::uint32_t cost = 1) {
        return tower_.connectLaneNodes(from, to, cost);
    }

    bool connectLaneNodesBidirectional(
        LaneNodeId first,
        LaneNodeId second,
        std::uint32_t cost = 1) {
        return tower_.connectLaneNodesBidirectional(first, second, cost);
    }

    bool disconnectLaneNodes(LaneNodeId from, LaneNodeId to) {
        return tower_.disconnectLaneNodes(from, to);
    }

    bool setLaneConnectionEnabled(
        LaneNodeId from,
        LaneNodeId to,
        bool enabled) {
        return tower_.setLaneConnectionEnabled(from, to, enabled);
    }

    bool registerLane(SpawnLane lane) {
        return tower_.registerLane(std::move(lane));
    }

    bool registerWave(WaveDefinition wave) {
        const auto copy = wave;
        if (!tower_.registerWave(std::move(wave))) return false;
        replaceById(waveDefinitions_, copy);
        return true;
    }

    bool registerReward(RewardDefinition reward) {
        if (!tower_.registerReward(reward)) return false;
        replaceById(rewardDefinitions_, reward);
        return true;
    }

    bool registerSequence(WaveSequenceDefinition sequence) {
        return sequence_.registerSequence(std::move(sequence));
    }

    void setResources(std::int32_t available) noexcept {
        tower_.setResources(available);
    }

    void setPlayerTeam(std::uint32_t teamId) noexcept {
        tower_.setPlayerTeam(teamId);
    }

    bool setTeamModifierProfile(
        std::uint32_t teamId,
        gameplay::TeamModifierProfile profile) {
        return tower_.setTeamModifierProfile(teamId, profile);
    }

    void setRequiredRoute(
        gameplay::GridPoint start,
        gameplay::GridPoint goal) noexcept {
        tower_.setRequiredRoute(start, goal);
    }

    bool setBlocked(gameplay::GridPoint point, bool blocked) {
        return tower_.setBlocked(point, blocked);
    }

    ecs::Entity createBaseCore(
        gameplay::Position position,
        std::uint32_t teamId,
        gameplay::CombatStats combat) {
        return tower_.createBaseCore(position, teamId, combat);
    }

    ecs::Entity createDefender(
        gameplay::Position position,
        gameplay::MoveSpeed speed,
        std::uint32_t teamId,
        gameplay::CombatStats combat) {
        return tower_.createDefender(position, speed, teamId, combat);
    }

    bool submit(WaveLoopCommand command) {
        return commands_.submit(std::move(command));
    }

    bool submitRts(gameplay::TickCommand command) {
        return tower_.submitRts(std::move(command));
    }

    bool step(std::uint64_t tick) {
        if (hasStepped_ && tick <= lastTick_) return false;
        hasStepped_ = true;
        lastTick_ = tick;
        events_.clear();

        for (const auto& command : commands_.consume(tick)) {
            processCommand(tick, command);
        }
        if (sequence_.due(tick)) {
            queuePreparedWave(tick, false);
        }
        if (!tower_.step(tick)) return false;
        reconcileTowerEvents(tick);
        buildSnapshot(tick);
        return true;
    }

    const WaveLoopSnapshot& snapshot() const noexcept { return snapshot_; }
    const std::vector<WaveLoopEvent>& events() const noexcept { return events_; }
    const WaveSequenceDirector& sequence() const noexcept { return sequence_; }
    const TowerDefenseSimulation& tower() const noexcept { return tower_; }
    const gameplay::ResourceLedger& resources() const noexcept {
        return tower_.resources();
    }
    WaveLoopCommandStream::State commandStreamState() const {
        return commands_.snapshot();
    }
    std::uint64_t rootSeed() const noexcept { return rootSeed_; }
    std::uint64_t lastTick() const noexcept { return lastTick_; }

private:
    friend class WaveLoopSimulationArchive;

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
    static const T* findById(
        const std::vector<T>& values,
        std::uint32_t id) noexcept {
        const auto iterator = std::lower_bound(
            values.begin(), values.end(), id,
            [](const T& current, std::uint32_t key) {
                return current.id < key;
            });
        return iterator != values.end() && iterator->id == id
            ? &*iterator : nullptr;
    }

    void processCommand(
        std::uint64_t tick,
        const WaveLoopCommand& command) {
        switch (command.type) {
        case WaveLoopCommandType::StartSequence:
            startSequence(tick, command.objectId);
            break;
        case WaveLoopCommandType::StartNextWave:
            startNextWave(tick, command.objectId);
            break;
        case WaveLoopCommandType::ChooseReward:
            chooseReward(tick, command.objectId);
            break;
        }
    }

    void startSequence(std::uint64_t tick, WaveSequenceId id) {
        if (tower_.director().state().phase == WavePhase::Spawning ||
            tower_.director().state().phase == WavePhase::Active ||
            tower_.director().state().phase == WavePhase::RewardPending) {
            rejectSequence(tick, id, WaveSequenceFailure::AlreadyActive);
            return;
        }
        const auto* definition = sequence_.definition(id);
        if (!definition) {
            rejectSequence(tick, id, WaveSequenceFailure::UnknownSequence);
            return;
        }
        for (const auto waveId : definition->waves) {
            if (!findById(waveDefinitions_, waveId)) {
                rejectSequence(tick, id, WaveSequenceFailure::UnknownWave,
                               waveId);
                return;
            }
        }

        const auto result = sequence_.start(id, tick);
        if (!result.accepted) {
            rejectSequence(tick, id, result.failure, result.waveId);
            return;
        }
        events_.push_back(
            {tick, WaveLoopEventType::SequenceStarted, id,
             result.waveId, id, 0, 0});
        publishPreparationStarted(tick);
    }

    void startNextWave(std::uint64_t tick, WaveSequenceId id) {
        const auto before = sequence_.state().scheduledStartTick;
        const auto result = sequence_.requestEarlyStart(id, tick);
        if (!result.accepted) {
            rejectSequence(tick, id, result.failure, result.waveId);
            return;
        }
        const auto remaining = before > tick &&
                               before != WaveSequenceDirector::noScheduledTick()
            ? std::min<std::uint64_t>(
                  before - tick,
                  static_cast<std::uint64_t>(
                      std::numeric_limits<std::int32_t>::max()))
            : 0u;
        events_.push_back(
            {tick, WaveLoopEventType::PreparationSkipped, id,
             result.waveId, id, static_cast<std::int32_t>(remaining), 0});
        queuePreparedWave(tick, true);
    }

    void chooseReward(std::uint64_t tick, RewardId id) {
        if (sequence_.state().phase != WaveSequencePhase::RewardPending) {
            rejectSequence(
                tick, sequence_.state().sequenceId,
                WaveSequenceFailure::RewardNotPending,
                sequence_.state().currentWave, id);
            return;
        }
        std::uint32_t sequenceValue = 0;
        if (!reserveInternalSequence(sequenceValue)) {
            failSequence(tick, WaveSequenceFailure::TowerCommandRejected);
            return;
        }
        TickCommand command;
        command.targetTick = tick;
        command.issuer = internalIssuer(sequence_.state().sequenceId);
        command.sequence = sequenceValue;
        command.type = CommandType::ChooseReward;
        command.objectId = id;
        if (!tower_.submit(command)) {
            failSequence(tick, WaveSequenceFailure::TowerCommandRejected);
        }
    }

    bool queuePreparedWave(std::uint64_t tick, bool) {
        if (sequence_.state().phase != WaveSequencePhase::Preparing) {
            return false;
        }
        const auto sequenceId = sequence_.state().sequenceId;
        const auto waveId = sequence_.state().currentWave;
        std::uint32_t sequenceValue = 0;
        if (!reserveInternalSequence(sequenceValue) ||
            !sequence_.markWaveStartQueued()) {
            failSequence(tick, WaveSequenceFailure::TowerCommandRejected);
            return false;
        }

        TickCommand command;
        command.targetTick = tick;
        command.issuer = internalIssuer(sequenceId);
        command.sequence = sequenceValue;
        command.type = CommandType::StartWave;
        command.objectId = waveId;
        if (!tower_.submit(command)) {
            failSequence(tick, WaveSequenceFailure::TowerCommandRejected);
            return false;
        }
        return true;
    }

    bool reserveInternalSequence(std::uint32_t& value) noexcept {
        value = 0;
        if (nextInternalSequence_ >
            std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        value = static_cast<std::uint32_t>(nextInternalSequence_);
        ++nextInternalSequence_;
        return true;
    }

    void reconcileTowerEvents(std::uint64_t tick) {
        for (const auto& event : tower_.events()) {
            switch (event.type) {
            case EventType::WaveStarted:
                if (sequence_.markWaveStarted(event.waveId)) {
                    events_.push_back(
                        {tick, WaveLoopEventType::WaveStarted,
                         sequence_.state().sequenceId, event.waveId,
                         event.waveId, 0, 0});
                }
                break;
            case EventType::WaveRejected:
                events_.push_back(
                    {tick, WaveLoopEventType::WaveRejected,
                     sequence_.state().sequenceId, event.waveId,
                     event.objectId, event.value, event.reason});
                failSequence(tick, WaveSequenceFailure::WaveRejected);
                break;
            case EventType::WaveCompleted:
                events_.push_back(
                    {tick, WaveLoopEventType::WaveCompleted,
                     sequence_.state().sequenceId, event.waveId,
                     0, 0, 0});
                handleWaveCompleted(tick, event.waveId);
                break;
            case EventType::RewardOffered:
                events_.push_back(
                    {tick, WaveLoopEventType::RewardOffered,
                     sequence_.state().sequenceId, event.waveId,
                     event.objectId, event.value, event.reason});
                break;
            case EventType::RewardChosen:
                events_.push_back(
                    {tick, WaveLoopEventType::RewardChosen,
                     sequence_.state().sequenceId, event.waveId,
                     event.objectId, event.value, event.reason});
                handleRewardChosen(tick);
                break;
            case EventType::RewardRejected:
                events_.push_back(
                    {tick, WaveLoopEventType::RewardRejected,
                     sequence_.state().sequenceId, event.waveId,
                     event.objectId, event.value, event.reason});
                break;
            case EventType::BaseCoreDestroyed:
                events_.push_back(
                    {tick, WaveLoopEventType::BaseCoreDestroyed,
                     sequence_.state().sequenceId, event.waveId,
                     0, 0, 0});
                failSequence(tick, WaveSequenceFailure::BaseCoreDestroyed);
                break;
            default:
                break;
            }
        }
    }

    void handleWaveCompleted(std::uint64_t tick, WaveId waveId) {
        const auto result = sequence_.markWaveCompleted(
            waveId, tick,
            tower_.director().state().phase == WavePhase::RewardPending);
        if (!result.accepted) {
            failSequence(tick, result.failure);
            return;
        }
        publishPostWaveTransition(tick);
    }

    void handleRewardChosen(std::uint64_t tick) {
        const auto result = sequence_.markRewardChosen(tick);
        if (!result.accepted) {
            failSequence(tick, result.failure);
            return;
        }
        publishPostWaveTransition(tick);
    }

    void publishPostWaveTransition(std::uint64_t tick) {
        if (sequence_.state().phase == WaveSequencePhase::Preparing) {
            publishPreparationStarted(tick);
        } else if (sequence_.state().phase == WaveSequencePhase::Complete) {
            events_.push_back(
                {tick, WaveLoopEventType::SequenceCompleted,
                 sequence_.state().sequenceId, 0,
                 sequence_.state().sequenceId, 0, 0});
        }
    }

    void publishPreparationStarted(std::uint64_t tick) {
        const auto& state = sequence_.state();
        const auto duration = state.scheduledStartTick >= tick &&
                              state.scheduledStartTick !=
                                  WaveSequenceDirector::noScheduledTick()
            ? std::min<std::uint64_t>(
                  state.scheduledStartTick - tick,
                  static_cast<std::uint64_t>(
                      std::numeric_limits<std::int32_t>::max()))
            : 0u;
        events_.push_back(
            {tick, WaveLoopEventType::PreparationStarted,
             state.sequenceId, state.currentWave, state.sequenceId,
             static_cast<std::int32_t>(duration), 0});
    }

    void rejectSequence(
        std::uint64_t tick,
        WaveSequenceId id,
        WaveSequenceFailure failure,
        WaveId waveId = 0,
        std::uint32_t objectId = 0) {
        events_.push_back(
            {tick, WaveLoopEventType::SequenceRejected, id, waveId,
             objectId == 0 ? id : objectId, 0,
             static_cast<std::uint32_t>(failure)});
    }

    void failSequence(
        std::uint64_t tick,
        WaveSequenceFailure failure) {
        if (!sequence_.fail(failure)) return;
        events_.push_back(
            {tick, WaveLoopEventType::SequenceFailed,
             sequence_.state().sequenceId,
             sequence_.state().currentWave,
             sequence_.state().sequenceId, 0,
             static_cast<std::uint32_t>(failure)});
    }

    static void hashCommand(
        foundation::CanonicalHash& hash,
        const WaveLoopCommand& command) {
        hash.WriteU64(command.targetTick);
        hash.WriteU32(command.issuer);
        hash.WriteU32(command.sequence);
        hash.WriteU8(static_cast<std::uint8_t>(command.type));
        hash.WriteU32(command.objectId);
    }

    void buildSnapshot(std::uint64_t tick) {
        snapshot_.tick = tick;
        snapshot_.towerDefenseWorldHash = tower_.snapshot().worldHash;
        snapshot_.sequence = sequence_.state();
        snapshot_.canStartEarly = sequence_.canStartEarly();
        snapshot_.preparationTicksRemaining = 0;
        if (snapshot_.sequence.phase == WaveSequencePhase::Preparing &&
            snapshot_.sequence.scheduledStartTick > tick) {
            snapshot_.preparationTicksRemaining =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    snapshot_.sequence.scheduledStartTick - tick,
                    std::numeric_limits<std::uint32_t>::max()));
        }

        foundation::CanonicalHash hash;
        hash.WriteU64(tick);
        hash.WriteU64(rootSeed_);
        hash.WriteU64(snapshot_.towerDefenseWorldHash);
        hash.WriteU64(nextInternalSequence_);
        sequence_.appendHash(hash);
        const auto commandState = commands_.snapshot();
        hash.WriteU64(commandState.committedThrough);
        hash.WriteU32(static_cast<std::uint32_t>(commandState.pending.size()));
        for (const auto& command : commandState.pending) {
            hashCommand(hash, command);
        }
        snapshot_.worldHash = hash.Value();
    }

    static std::uint32_t internalIssuer(WaveSequenceId id) noexcept {
        return 0xa0000000u | (id & 0x1fffffffu);
    }

    TowerDefenseSimulation tower_;
    WaveSequenceDirector sequence_;
    WaveLoopCommandStream commands_;
    std::vector<WaveDefinition> waveDefinitions_;
    std::vector<RewardDefinition> rewardDefinitions_;
    std::vector<WaveLoopEvent> events_;
    WaveLoopSnapshot snapshot_;
    std::uint64_t rootSeed_{1};
    std::uint64_t nextInternalSequence_{1};
    std::uint64_t lastTick_{};
    bool hasStepped_{};
};

} // namespace rts::tower_defense
