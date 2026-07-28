#pragma once

#include <RTSEngine/Rts/RtsGameSessionArchive.h>
#include <rts/sim/DesyncDiagnostics.h>
#include <rts/sim/Lockstep.h>
#include <rts/sim/RollbackCheckpoint.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct RtsLockstepConfig final {
    sim::LockstepSessionId sessionId{};
    std::uint32_t inputDelayTicks{2};
    std::uint32_t maximumPredictionTicks{};
    std::uint32_t checkpointIntervalTicks{8};
    std::uint32_t checkpointCapacity{32};
    std::uint32_t hashExchangeIntervalTicks{8};
    std::uint32_t maximumCommandsPerFrame{4096};
};

using RtsLockstepFrame = sim::LockstepCommandFrame<TickCommand>;

struct RtsLockstepPeerSequence final {
    sim::LockstepPeerId peerId{};
    std::uint64_t nextFrameSequence{1};
    std::uint32_t nextCommandSequence{1};
};

struct RtsReconnectSnapshot final {
    RtsLockstepConfig config;
    std::uint64_t nextTick{};
    std::uint64_t authoritativeHash{};
    std::vector<sim::LockstepPeer> peers;
    std::vector<RtsLockstepPeerSequence> sequences;
    std::vector<RtsLockstepFrame> futureFrames;
    std::vector<std::uint8_t> sessionArchive;
};

struct RtsLockstepCommandOutcome final {
    std::uint64_t tick{};
    std::uint32_t issuer{};
    std::uint32_t sequence{};
    SessionCommandResult result{SessionCommandResult::Accepted};
};

struct RtsRollbackReport final {
    std::uint64_t requestedTick{};
    std::uint64_t restoredResumeTick{};
    std::uint64_t replayedThrough{};
    std::uint32_t replayedTicks{};
};

enum class RtsLockstepStartResult : std::uint8_t {
    Started,
    AlreadyStarted,
    InvalidConfiguration,
    MissingPlayers,
    ArchiveUnavailable
};

enum class RtsLockstepAdvanceResult : std::uint8_t {
    Advanced,
    AdvancedAfterRollback,
    WaitingForInput,
    NotStarted,
    TimelineMismatch,
    SessionStepRejected,
    RollbackUnavailable,
    RollbackRestoreFailed
};

class RtsLockstepSession final {
public:
    explicit RtsLockstepSession(
        RtsGameSession& session,
        RtsLockstepConfig config)
        : session_(session),
          config_(sanitize(config)),
          coordinator_(coordinatorConfig(config_)),
          checkpoints_(
              config_.checkpointIntervalTicks,
              config_.checkpointCapacity),
          desync_(config_.sessionId) {}

    const RtsLockstepConfig& config() const noexcept { return config_; }
    const sim::LockstepCoordinator<TickCommand>& coordinator() const noexcept {
        return coordinator_;
    }
    const sim::RollbackCheckpointRing& checkpoints() const noexcept {
        return checkpoints_;
    }
    const sim::DesyncMonitor& desync() const noexcept { return desync_; }
    const std::vector<RtsLockstepCommandOutcome>& commandOutcomes() const noexcept {
        return commandOutcomes_;
    }
    const RtsRollbackReport& lastRollback() const noexcept {
        return lastRollback_;
    }
    bool started() const noexcept { return started_; }

    bool registerPeer(sim::LockstepPeer peer) {
        return !started_ && coordinator_.registerPeer(peer);
    }

    RtsLockstepStartResult start() {
        if (started_) return RtsLockstepStartResult::AlreadyStarted;
        if (config_.sessionId == 0 ||
            config_.checkpointCapacity == 0 ||
            config_.maximumCommandsPerFrame == 0) {
            return RtsLockstepStartResult::InvalidConfiguration;
        }
        const auto firstTick = session_.simulation().nextExpectedTick();
        if (!coordinator_.start(firstTick)) {
            return RtsLockstepStartResult::MissingPlayers;
        }
        rebuildSequenceStates(coordinator_.peers());
        const auto bootstrapEnd = coordinator_.scheduledTick(firstTick);
        if (!coordinator_.bootstrapEmptyFrames(
                bootstrapEnd,
                [this](const sim::LockstepPeer& peer,
                       const RtsLockstepFrame& frame) {
                    return validateFrame(peer, frame);
                },
                &sameCommand)) {
            return RtsLockstepStartResult::InvalidConfiguration;
        }
        if (!captureCurrent(true)) {
            return RtsLockstepStartResult::ArchiveUnavailable;
        }
        started_ = true;
        return RtsLockstepStartResult::Started;
    }

    bool buildLocalFrame(
        sim::LockstepPeerId peerId,
        std::vector<TickCommand> commands,
        RtsLockstepFrame& output) {
        if (!started_ || commands.size() > config_.maximumCommandsPerFrame) {
            return false;
        }
        const auto* owner = coordinator_.peer(peerId);
        auto* sequence = findSequence(peerId);
        if (!owner || !sequence || !owner->active ||
            owner->role != sim::LockstepPeerRole::Player ||
            sequence->nextFrameSequence ==
                std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        const auto remaining =
            std::numeric_limits<std::uint32_t>::max() -
            sequence->nextCommandSequence;
        if (commands.size() > remaining) return false;

        output = {};
        output.sessionId = config_.sessionId;
        output.tick = coordinator_.scheduledTick(
            coordinator_.simulatedThrough());
        output.peerId = peerId;
        output.frameSequence = sequence->nextFrameSequence++;
        output.commands = std::move(commands);
        for (auto& command : output.commands) {
            command.targetTick = output.tick;
            command.issuer = owner->issuer;
            command.sequence = sequence->nextCommandSequence++;
        }
        return normalizeFrame(output);
    }

    sim::LockstepFrameSubmitResult receiveFrame(RtsLockstepFrame frame) {
        if (!normalizeFrame(frame)) {
            return sim::LockstepFrameSubmitResult::InvalidFrame;
        }
        return coordinator_.submitFrame(
            std::move(frame),
            [this](const sim::LockstepPeer& peer,
                   const RtsLockstepFrame& value) {
                return validateFrame(peer, value);
            },
            &sameCommand);
    }

    RtsLockstepAdvanceResult advanceOne() {
        if (!started_) return RtsLockstepAdvanceResult::NotStarted;
        bool rolledBack = false;
        if (coordinator_.rollbackRequired()) {
            const auto rollback = rollbackAndReplay();
            if (rollback != RtsLockstepAdvanceResult::AdvancedAfterRollback) {
                return rollback;
            }
            rolledBack = true;
        }

        const auto tick = coordinator_.simulatedThrough();
        if (session_.simulation().nextExpectedTick() != tick) {
            return RtsLockstepAdvanceResult::TimelineMismatch;
        }
        if (!coordinator_.collectCommands(tick, commandScratch_)) {
            return RtsLockstepAdvanceResult::WaitingForInput;
        }

        commandOutcomes_.clear();
        submitCommands(commandScratch_, true);
        if (session_.stepDetailed(tick) != RtsStepResult::Advanced ||
            !coordinator_.markSimulated(tick)) {
            return RtsLockstepAdvanceResult::SessionStepRejected;
        }
        recordCompletedTick(tick);
        return rolledBack
            ? RtsLockstepAdvanceResult::AdvancedAfterRollback
            : RtsLockstepAdvanceResult::Advanced;
    }

    bool seekNextTick(std::uint64_t nextTick) {
        if (!started_ || nextTick > coordinator_.simulatedThrough()) {
            return false;
        }
        const auto* selected = checkpoints_.latestAtOrBefore(nextTick);
        if (!selected) return false;
        const auto checkpoint = *selected;
        if (!restoreCheckpoint(checkpoint)) return false;
        const auto oldThrough = coordinator_.simulatedThrough();
        if (!coordinator_.rewindSimulatedThrough(checkpoint.resumeTick)) {
            return false;
        }
        checkpoints_.discardAfter(checkpoint.resumeTick);
        desync_.rewindFrom(checkpoint.resumeTick);
        if (!replayRange(checkpoint.resumeTick, nextTick)) return false;
        (void)oldThrough;
        coordinator_.clearRollback();
        return true;
    }

    bool seekCompletedTick(std::uint64_t tick) {
        if (tick == std::numeric_limits<std::uint64_t>::max()) return false;
        return seekNextTick(tick + 1u);
    }

    bool hashReportDue(std::uint64_t tick) const noexcept {
        return config_.hashExchangeIntervalTicks != 0 &&
               tick % config_.hashExchangeIntervalTicks == 0;
    }

    bool makeHashReport(
        sim::LockstepPeerId peerId,
        std::uint64_t tick,
        sim::StateHashReport& output) const noexcept {
        if (!started_ || !coordinator_.peer(peerId)) return false;
        const auto* local = desync_.localHash(tick);
        if (!local) return false;
        output = {
            config_.sessionId,
            peerId,
            tick,
            local->worldHash};
        return true;
    }

    sim::HashReportSubmitResult receiveHashReport(
        sim::StateHashReport report) {
        if (!coordinator_.peer(report.peerId)) {
            return sim::HashReportSubmitResult::InvalidPeer;
        }
        return desync_.submitRemote(report);
    }

    bool makeReconnectSnapshot(RtsReconnectSnapshot& output) const {
        if (!started_ || coordinator_.rollbackRequired() ||
            coordinator_.confirmedThrough() <
                coordinator_.simulatedThrough()) {
            return false;
        }
        output = {};
        output.config = config_;
        output.nextTick = coordinator_.simulatedThrough();
        output.authoritativeHash =
            RtsGameSessionArchive::authoritativeHash(session_);
        output.peers = coordinator_.peers();
        output.sequences = sequences_;
        for (const auto& frame : coordinator_.frames()) {
            if (frame.tick >= output.nextTick) {
                output.futureFrames.push_back(frame);
            }
        }
        output.sessionArchive = RtsGameSessionArchive::encode(session_);
        return !output.sessionArchive.empty();
    }

    bool restoreReconnectSnapshot(RtsReconnectSnapshot snapshot) {
        if (snapshot.config.sessionId != config_.sessionId ||
            snapshot.sessionArchive.empty() ||
            !validateSequences(snapshot.peers, snapshot.sequences) ||
            !RtsGameSessionArchive::decode(
                snapshot.sessionArchive, session_) ||
            session_.simulation().nextExpectedTick() != snapshot.nextTick ||
            RtsGameSessionArchive::authoritativeHash(session_) !=
                snapshot.authoritativeHash) {
            return false;
        }

        config_ = sanitize(snapshot.config);
        coordinator_ = sim::LockstepCoordinator<TickCommand>(
            coordinatorConfig(config_));
        for (const auto& peer : snapshot.peers) {
            if (!coordinator_.registerPeer(peer)) return false;
        }
        if (!coordinator_.start(snapshot.nextTick) ||
            !coordinator_.restoreConfirmedState(
                snapshot.nextTick,
                std::move(snapshot.futureFrames),
                [this](const sim::LockstepPeer& peer,
                       const RtsLockstepFrame& frame) {
                    return validateFrame(peer, frame);
                },
                &sameCommand)) {
            return false;
        }
        sequences_ = std::move(snapshot.sequences);
        checkpoints_ = sim::RollbackCheckpointRing(
            config_.checkpointIntervalTicks,
            config_.checkpointCapacity);
        desync_ = sim::DesyncMonitor(config_.sessionId);
        commandScratch_.clear();
        commandOutcomes_.clear();
        lastRollback_ = {};
        started_ = captureCurrent(true);
        return started_;
    }

private:
    static RtsLockstepConfig sanitize(RtsLockstepConfig value) noexcept {
        value.checkpointIntervalTicks = std::max<std::uint32_t>(
            1, value.checkpointIntervalTicks);
        value.checkpointCapacity = std::max<std::uint32_t>(
            1, value.checkpointCapacity);
        value.hashExchangeIntervalTicks = std::max<std::uint32_t>(
            1, value.hashExchangeIntervalTicks);
        value.maximumCommandsPerFrame = std::max<std::uint32_t>(
            1, value.maximumCommandsPerFrame);
        return value;
    }

    static sim::LockstepCoordinatorConfig coordinatorConfig(
        const RtsLockstepConfig& value) noexcept {
        return {
            value.sessionId,
            value.inputDelayTicks,
            value.maximumPredictionTicks};
    }

    static bool sameCommand(
        const TickCommand& first,
        const TickCommand& second) noexcept {
        return first.targetTick == second.targetTick &&
               first.issuer == second.issuer &&
               first.sequence == second.sequence &&
               first.type == second.type &&
               first.subject == second.subject &&
               first.targetX == second.targetX &&
               first.targetY == second.targetY &&
               first.append == second.append &&
               first.definitionId == second.definitionId &&
               first.objectId == second.objectId &&
               first.targetEntity == second.targetEntity;
    }

    static bool commandLess(
        const TickCommand& first,
        const TickCommand& second) noexcept {
        return first.sequence < second.sequence;
    }

    bool normalizeFrame(RtsLockstepFrame& frame) const {
        if (frame.commands.size() > config_.maximumCommandsPerFrame) {
            return false;
        }
        std::stable_sort(
            frame.commands.begin(), frame.commands.end(), commandLess);
        auto output = frame.commands.begin();
        for (auto current = frame.commands.begin();
             current != frame.commands.end(); ++current) {
            if (output != frame.commands.begin() &&
                (output - 1)->sequence == current->sequence) {
                if (!sameCommand(*(output - 1), *current)) return false;
                continue;
            }
            if (output != current) *output = std::move(*current);
            ++output;
        }
        frame.commands.erase(output, frame.commands.end());
        return true;
    }

    bool validateFrame(
        const sim::LockstepPeer& owner,
        const RtsLockstepFrame& frame) const noexcept {
        if (frame.commands.size() > config_.maximumCommandsPerFrame) {
            return false;
        }
        std::uint32_t previousSequence = 0;
        for (const auto& command : frame.commands) {
            if (command.targetTick != frame.tick ||
                command.issuer != owner.issuer ||
                command.sequence == 0 ||
                command.sequence <= previousSequence) {
                return false;
            }
            previousSequence = command.sequence;
        }
        return true;
    }

    void rebuildSequenceStates(
        const std::vector<sim::LockstepPeer>& peers) {
        sequences_.clear();
        for (const auto& peer : peers) {
            if (peer.role == sim::LockstepPeerRole::Player) {
                sequences_.push_back({peer.peerId, 1, 1});
            }
        }
        std::sort(
            sequences_.begin(), sequences_.end(),
            [](const auto& first, const auto& second) {
                return first.peerId < second.peerId;
            });
    }

    RtsLockstepPeerSequence* findSequence(
        sim::LockstepPeerId peerId) noexcept {
        const auto found = std::lower_bound(
            sequences_.begin(), sequences_.end(), peerId,
            [](const RtsLockstepPeerSequence& value,
               sim::LockstepPeerId id) {
                return value.peerId < id;
            });
        return found != sequences_.end() && found->peerId == peerId
            ? &*found
            : nullptr;
    }

    static bool validateSequences(
        const std::vector<sim::LockstepPeer>& peers,
        const std::vector<RtsLockstepPeerSequence>& sequences) {
        std::vector<sim::LockstepPeerId> players;
        for (const auto& peer : peers) {
            if (peer.role == sim::LockstepPeerRole::Player) {
                players.push_back(peer.peerId);
            }
        }
        std::sort(players.begin(), players.end());
        if (players.size() != sequences.size()) return false;
        for (std::size_t index = 0; index < sequences.size(); ++index) {
            if (sequences[index].peerId != players[index] ||
                sequences[index].nextFrameSequence == 0 ||
                sequences[index].nextCommandSequence == 0 ||
                (index != 0 &&
                 sequences[index - 1].peerId >= sequences[index].peerId)) {
                return false;
            }
        }
        return true;
    }

    void submitCommands(
        const std::vector<TickCommand>& commands,
        bool recordOutcomes) {
        for (const auto& command : commands) {
            const auto result = session_.submitDetailed(command);
            if (recordOutcomes) {
                commandOutcomes_.push_back(
                    {command.targetTick,
                     command.issuer,
                     command.sequence,
                     result});
            }
        }
    }

    void recordCompletedTick(std::uint64_t tick) {
        const auto hash =
            RtsGameSessionArchive::authoritativeHash(session_);
        desync_.recordLocal(tick, hash);
        (void)captureCurrent(false);
    }

    bool captureCurrent(bool force) {
        sim::RollbackCheckpoint checkpoint;
        checkpoint.resumeTick =
            session_.simulation().nextExpectedTick();
        checkpoint.authoritativeHash =
            RtsGameSessionArchive::authoritativeHash(session_);
        checkpoint.archive = RtsGameSessionArchive::encode(session_);
        if (checkpoint.archive.empty()) return false;
        if (!force && !checkpoints_.shouldCapture(checkpoint.resumeTick)) {
            return true;
        }
        return checkpoints_.capture(std::move(checkpoint), force);
    }

    bool restoreCheckpoint(
        const sim::RollbackCheckpoint& checkpoint) {
        return RtsGameSessionArchive::decode(
                   checkpoint.archive, session_) &&
               session_.simulation().nextExpectedTick() ==
                   checkpoint.resumeTick &&
               RtsGameSessionArchive::authoritativeHash(session_) ==
                   checkpoint.authoritativeHash;
    }

    bool replayRange(
        std::uint64_t beginTick,
        std::uint64_t endExclusive) {
        for (std::uint64_t tick = beginTick;
             tick < endExclusive; ++tick) {
            coordinator_.collectCommandsForReplay(tick, commandScratch_);
            submitCommands(commandScratch_, false);
            if (session_.stepDetailed(tick) != RtsStepResult::Advanced ||
                !coordinator_.markSimulated(tick)) {
                return false;
            }
            recordCompletedTick(tick);
        }
        return true;
    }

    RtsLockstepAdvanceResult rollbackAndReplay() {
        const auto requested = coordinator_.rollbackFrom();
        const auto previousThrough = coordinator_.simulatedThrough();
        const auto* selected = checkpoints_.latestAtOrBefore(requested);
        if (!selected) {
            return RtsLockstepAdvanceResult::RollbackUnavailable;
        }
        const auto checkpoint = *selected;
        if (!restoreCheckpoint(checkpoint) ||
            !coordinator_.rewindSimulatedThrough(
                checkpoint.resumeTick)) {
            return RtsLockstepAdvanceResult::RollbackRestoreFailed;
        }
        checkpoints_.discardAfter(checkpoint.resumeTick);
        desync_.rewindFrom(checkpoint.resumeTick);
        if (!replayRange(checkpoint.resumeTick, previousThrough)) {
            return RtsLockstepAdvanceResult::RollbackRestoreFailed;
        }
        coordinator_.clearRollback();
        lastRollback_.requestedTick = requested;
        lastRollback_.restoredResumeTick = checkpoint.resumeTick;
        lastRollback_.replayedThrough = previousThrough;
        lastRollback_.replayedTicks = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max(),
                previousThrough - checkpoint.resumeTick));
        return RtsLockstepAdvanceResult::AdvancedAfterRollback;
    }

    RtsGameSession& session_;
    RtsLockstepConfig config_;
    sim::LockstepCoordinator<TickCommand> coordinator_;
    sim::RollbackCheckpointRing checkpoints_;
    sim::DesyncMonitor desync_;
    std::vector<RtsLockstepPeerSequence> sequences_;
    std::vector<TickCommand> commandScratch_;
    std::vector<RtsLockstepCommandOutcome> commandOutcomes_;
    RtsRollbackReport lastRollback_;
    bool started_{};
};

} // namespace rts::gameplay
