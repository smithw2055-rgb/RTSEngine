#pragma once

#include <RTSEngine/Rts/G3GameSession.h>
#include <rts/sim/DesyncDiagnostics.h>
#include <rts/sim/Lockstep.h>
#include <rts/sim/RollbackCheckpoint.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace rts::gameplay {

enum class G3NetworkCommandKind : std::uint8_t {
    Base,
    Ability
};

struct G3NetworkCommand final {
    G3NetworkCommandKind kind{G3NetworkCommandKind::Base};
    TickCommand base{};
    AbilityCommand ability{};

    static G3NetworkCommand fromBase(TickCommand value) {
        G3NetworkCommand result;
        result.kind = G3NetworkCommandKind::Base;
        result.base = std::move(value);
        return result;
    }

    static G3NetworkCommand fromAbility(AbilityCommand value) {
        G3NetworkCommand result;
        result.kind = G3NetworkCommandKind::Ability;
        result.ability = std::move(value);
        return result;
    }
};

using RtsG3LockstepFrame =
    sim::LockstepCommandFrame<G3NetworkCommand>;

struct RtsG3LockstepConfig final {
    sim::LockstepSessionId sessionId{};
    std::uint32_t inputDelayTicks{2};
    std::uint32_t maximumPredictionTicks{};
    std::uint32_t checkpointIntervalTicks{8};
    std::uint32_t checkpointCapacity{32};
    std::uint32_t hashExchangeIntervalTicks{8};
    std::uint32_t maximumCommandsPerFrame{4096};
};

struct RtsG3PeerSequence final {
    sim::LockstepPeerId peerId{};
    std::uint64_t nextFrameSequence{1};
    std::uint32_t nextCommandSequence{1};
};

enum class RtsG3CommandOutcomeKind : std::uint8_t {
    Base,
    Ability
};

struct RtsG3CommandOutcome final {
    std::uint64_t tick{};
    std::uint32_t issuer{};
    std::uint32_t sequence{};
    RtsG3CommandOutcomeKind kind{
        RtsG3CommandOutcomeKind::Base};
    SessionCommandResult baseResult{
        SessionCommandResult::Accepted};
    AbilitySubmitResult abilityResult{
        AbilitySubmitResult::Accepted};
};

struct RtsG3RollbackReport final {
    std::uint64_t requestedTick{};
    std::uint64_t restoredResumeTick{};
    std::uint64_t replayedThrough{};
    std::uint32_t replayedTicks{};
};

struct RtsG3ReconnectSnapshot final {
    RtsG3LockstepConfig config{};
    std::uint64_t nextTick{};
    std::uint64_t authoritativeHash{};
    std::vector<sim::LockstepPeer> peers;
    std::vector<RtsG3PeerSequence> sequences;
    std::vector<RtsG3LockstepFrame> futureFrames;
    std::vector<std::uint8_t> sessionArchive;
};

enum class RtsG3LockstepStartResult : std::uint8_t {
    Started,
    AlreadyStarted,
    InvalidConfiguration,
    MissingPlayers,
    ArchiveUnavailable
};

enum class RtsG3LockstepAdvanceResult : std::uint8_t {
    Advanced,
    AdvancedAfterRollback,
    WaitingForInput,
    NotStarted,
    TimelineMismatch,
    SessionStepRejected,
    RollbackUnavailable,
    RollbackRestoreFailed
};

class RtsG3LockstepSession final {
public:
    RtsG3LockstepSession(
        RtsG3GameSession& session,
        RtsG3LockstepConfig config)
        : session_(session),
          config_(sanitize(config)),
          coordinator_(coordinatorConfig(config_)),
          checkpoints_(
              config_.checkpointIntervalTicks,
              config_.checkpointCapacity),
          desync_(config_.sessionId) {}

    const RtsG3LockstepConfig& config() const noexcept {
        return config_;
    }

    const sim::LockstepCoordinator<G3NetworkCommand>&
    coordinator() const noexcept {
        return coordinator_;
    }

    const sim::RollbackCheckpointRing&
    checkpoints() const noexcept {
        return checkpoints_;
    }

    const sim::DesyncMonitor& desync() const noexcept {
        return desync_;
    }

    const std::vector<RtsG3CommandOutcome>&
    commandOutcomes() const noexcept {
        return outcomes_;
    }

    const RtsG3RollbackReport&
    lastRollback() const noexcept {
        return lastRollback_;
    }

    bool started() const noexcept { return started_; }

    bool registerPeer(sim::LockstepPeer peer) {
        return !started_ &&
               coordinator_.registerPeer(peer);
    }

    RtsG3LockstepStartResult start() {
        if (started_) {
            return RtsG3LockstepStartResult::
                AlreadyStarted;
        }
        if (config_.sessionId == 0 ||
            config_.checkpointCapacity == 0 ||
            config_.maximumCommandsPerFrame == 0) {
            return RtsG3LockstepStartResult::
                InvalidConfiguration;
        }
        const auto firstTick =
            session_.base().simulation().
                nextExpectedTick();
        if (!coordinator_.start(firstTick)) {
            return RtsG3LockstepStartResult::
                MissingPlayers;
        }
        rebuildSequences();
        const auto bootstrapEnd =
            coordinator_.scheduledTick(firstTick);
        if (!coordinator_.bootstrapEmptyFrames(
                bootstrapEnd,
                [this](
                    const sim::LockstepPeer& peer,
                    const RtsG3LockstepFrame& frame) {
                    return validateFrame(peer, frame);
                },
                &sameCommand)) {
            return RtsG3LockstepStartResult::
                InvalidConfiguration;
        }
        if (!captureCurrent(true)) {
            return RtsG3LockstepStartResult::
                ArchiveUnavailable;
        }
        started_ = true;
        return RtsG3LockstepStartResult::Started;
    }

    bool buildLocalFrame(
        sim::LockstepPeerId peerId,
        std::vector<G3NetworkCommand> commands,
        RtsG3LockstepFrame& output) {
        if (!started_ ||
            commands.size() >
                config_.maximumCommandsPerFrame) {
            return false;
        }
        const auto* owner =
            coordinator_.peer(peerId);
        auto* sequence = findSequence(peerId);
        if (!owner || !sequence || !owner->active ||
            owner->role !=
                sim::LockstepPeerRole::Player ||
            sequence->nextFrameSequence ==
                std::numeric_limits<
                    std::uint64_t>::max()) {
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
        output.frameSequence =
            sequence->nextFrameSequence++;
        output.commands = std::move(commands);
        for (auto& command : output.commands) {
            assignIdentity(
                command,
                output.tick,
                owner->issuer,
                sequence->nextCommandSequence++);
        }
        return normalizeFrame(output);
    }

    sim::LockstepFrameSubmitResult receiveFrame(
        RtsG3LockstepFrame frame) {
        if (!normalizeFrame(frame)) {
            return sim::LockstepFrameSubmitResult::
                InvalidFrame;
        }
        return coordinator_.submitFrame(
            std::move(frame),
            [this](
                const sim::LockstepPeer& peer,
                const RtsG3LockstepFrame& value) {
                return validateFrame(peer, value);
            },
            &sameCommand);
    }

    RtsG3LockstepAdvanceResult advanceOne() {
        if (!started_) {
            return RtsG3LockstepAdvanceResult::
                NotStarted;
        }
        bool rolledBack = false;
        if (coordinator_.rollbackRequired()) {
            const auto rollback = rollbackAndReplay();
            if (rollback !=
                RtsG3LockstepAdvanceResult::
                    AdvancedAfterRollback) {
                return rollback;
            }
            rolledBack = true;
        }

        const auto tick =
            coordinator_.simulatedThrough();
        if (session_.base().simulation().
                nextExpectedTick() != tick) {
            return RtsG3LockstepAdvanceResult::
                TimelineMismatch;
        }
        if (!coordinator_.collectCommands(
                tick, commandScratch_)) {
            return RtsG3LockstepAdvanceResult::
                WaitingForInput;
        }

        outcomes_.clear();
        submitCommands(commandScratch_);
        if (session_.stepDetailed(tick) !=
                RtsStepResult::Advanced ||
            !coordinator_.markSimulated(tick)) {
            return RtsG3LockstepAdvanceResult::
                SessionStepRejected;
        }
        recordCompletedTick(tick);
        return rolledBack
            ? RtsG3LockstepAdvanceResult::
                  AdvancedAfterRollback
            : RtsG3LockstepAdvanceResult::Advanced;
    }

    bool seekNextTick(std::uint64_t nextTick) {
        if (!started_ ||
            nextTick >
                coordinator_.simulatedThrough()) {
            return false;
        }
        const auto* selected =
            checkpoints_.latestAtOrBefore(nextTick);
        if (!selected) return false;
        const auto checkpoint = *selected;
        if (!restoreCheckpoint(checkpoint)) return false;
        if (!coordinator_.rewindSimulatedThrough(
                checkpoint.resumeTick)) {
            return false;
        }
        checkpoints_.discardAfter(
            checkpoint.resumeTick);
        desync_.rewindFrom(checkpoint.resumeTick);
        if (!replayRange(
                checkpoint.resumeTick, nextTick)) {
            return false;
        }
        coordinator_.clearRollback();
        return true;
    }

    bool seekCompletedTick(std::uint64_t tick) {
        return tick !=
                   std::numeric_limits<
                       std::uint64_t>::max() &&
               seekNextTick(tick + 1u);
    }

    bool hashReportDue(
        std::uint64_t tick) const noexcept {
        return config_.hashExchangeIntervalTicks != 0 &&
               tick %
                   config_.hashExchangeIntervalTicks ==
                   0;
    }

    bool makeHashReport(
        sim::LockstepPeerId peerId,
        std::uint64_t tick,
        sim::StateHashReport& output) const noexcept {
        if (!started_ ||
            !coordinator_.peer(peerId)) {
            return false;
        }
        const auto* local =
            desync_.localHash(tick);
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
            return sim::HashReportSubmitResult::
                InvalidPeer;
        }
        return desync_.submitRemote(report);
    }

    bool makeReconnectSnapshot(
        RtsG3ReconnectSnapshot& output) const {
        if (!started_ ||
            coordinator_.rollbackRequired() ||
            coordinator_.confirmedThrough() <
                coordinator_.simulatedThrough()) {
            return false;
        }
        output = {};
        output.config = config_;
        output.nextTick =
            coordinator_.simulatedThrough();
        output.authoritativeHash =
            session_.authoritativeHash();
        output.peers = coordinator_.peers();
        output.sequences = sequences_;
        for (const auto& frame :
             coordinator_.frames()) {
            if (frame.tick >= output.nextTick) {
                output.futureFrames.push_back(frame);
            }
        }
        output.sessionArchive = session_.encode();
        return !output.sessionArchive.empty();
    }

    bool restoreReconnectSnapshot(
        RtsG3ReconnectSnapshot snapshot) {
        if (snapshot.config.sessionId !=
                config_.sessionId ||
            snapshot.sessionArchive.empty() ||
            !validateSequences(
                snapshot.peers, snapshot.sequences) ||
            !session_.decode(snapshot.sessionArchive) ||
            session_.base().simulation().
                    nextExpectedTick() !=
                snapshot.nextTick ||
            session_.authoritativeHash() !=
                snapshot.authoritativeHash) {
            return false;
        }

        config_ = sanitize(snapshot.config);
        coordinator_ =
            sim::LockstepCoordinator<
                G3NetworkCommand>(
                coordinatorConfig(config_));
        for (const auto& peer : snapshot.peers) {
            if (!coordinator_.registerPeer(peer)) {
                return false;
            }
        }
        if (!coordinator_.start(snapshot.nextTick) ||
            !coordinator_.restoreConfirmedState(
                snapshot.nextTick,
                std::move(snapshot.futureFrames),
                [this](
                    const sim::LockstepPeer& peer,
                    const RtsG3LockstepFrame& frame) {
                    return validateFrame(peer, frame);
                },
                &sameCommand)) {
            return false;
        }

        sequences_ = std::move(snapshot.sequences);
        checkpoints_ =
            sim::RollbackCheckpointRing(
                config_.checkpointIntervalTicks,
                config_.checkpointCapacity);
        desync_ =
            sim::DesyncMonitor(config_.sessionId);
        commandScratch_.clear();
        outcomes_.clear();
        lastRollback_ = {};
        started_ = captureCurrent(true);
        return started_;
    }

private:
    static RtsG3LockstepConfig sanitize(
        RtsG3LockstepConfig value) noexcept {
        value.checkpointIntervalTicks =
            std::max<std::uint32_t>(
                1, value.checkpointIntervalTicks);
        value.checkpointCapacity =
            std::max<std::uint32_t>(
                1, value.checkpointCapacity);
        value.hashExchangeIntervalTicks =
            std::max<std::uint32_t>(
                1,
                value.hashExchangeIntervalTicks);
        value.maximumCommandsPerFrame =
            std::max<std::uint32_t>(
                1, value.maximumCommandsPerFrame);
        return value;
    }

    static sim::LockstepCoordinatorConfig
    coordinatorConfig(
        const RtsG3LockstepConfig& value) noexcept {
        return {
            value.sessionId,
            value.inputDelayTicks,
            value.maximumPredictionTicks};
    }

    static void assignIdentity(
        G3NetworkCommand& command,
        std::uint64_t tick,
        std::uint32_t issuer,
        std::uint32_t sequence) {
        if (command.kind ==
            G3NetworkCommandKind::Base) {
            command.base.targetTick = tick;
            command.base.issuer = issuer;
            command.base.sequence = sequence;
        } else {
            command.ability.targetTick = tick;
            command.ability.issuer = issuer;
            command.ability.sequence = sequence;
        }
    }

    static std::uint64_t commandTick(
        const G3NetworkCommand& command) noexcept {
        return command.kind ==
                   G3NetworkCommandKind::Base
            ? command.base.targetTick
            : command.ability.targetTick;
    }

    static std::uint32_t commandIssuer(
        const G3NetworkCommand& command) noexcept {
        return command.kind ==
                   G3NetworkCommandKind::Base
            ? command.base.issuer
            : command.ability.issuer;
    }

    static std::uint32_t commandSequence(
        const G3NetworkCommand& command) noexcept {
        return command.kind ==
                   G3NetworkCommandKind::Base
            ? command.base.sequence
            : command.ability.sequence;
    }

    static bool sameCommand(
        const G3NetworkCommand& first,
        const G3NetworkCommand& second) noexcept {
        if (first.kind != second.kind) return false;
        if (first.kind ==
            G3NetworkCommandKind::Base) {
            const auto& a = first.base;
            const auto& b = second.base;
            return a.targetTick == b.targetTick &&
                   a.issuer == b.issuer &&
                   a.sequence == b.sequence &&
                   a.type == b.type &&
                   a.subject == b.subject &&
                   a.targetX == b.targetX &&
                   a.targetY == b.targetY &&
                   a.append == b.append &&
                   a.definitionId == b.definitionId &&
                   a.objectId == b.objectId &&
                   a.targetEntity == b.targetEntity;
        }
        const auto& a = first.ability;
        const auto& b = second.ability;
        return a.targetTick == b.targetTick &&
               a.issuer == b.issuer &&
               a.sequence == b.sequence &&
               a.caster == b.caster &&
               a.abilityId == b.abilityId &&
               a.targetEntity == b.targetEntity &&
               a.targetPoint == b.targetPoint;
    }

    static bool normalizeFrame(
        RtsG3LockstepFrame& frame) {
        std::stable_sort(
            frame.commands.begin(),
            frame.commands.end(),
            [](const auto& first, const auto& second) {
                const auto firstSequence =
                    commandSequence(first);
                const auto secondSequence =
                    commandSequence(second);
                if (firstSequence != secondSequence) {
                    return firstSequence <
                           secondSequence;
                }
                return static_cast<std::uint8_t>(
                           first.kind) <
                       static_cast<std::uint8_t>(
                           second.kind);
            });
        for (std::size_t index = 0;
             index < frame.commands.size();
             ++index) {
            if (commandSequence(
                    frame.commands[index]) == 0) {
                return false;
            }
            if (index != 0 &&
                commandSequence(
                    frame.commands[index - 1]) ==
                    commandSequence(
                        frame.commands[index])) {
                return false;
            }
        }
        return true;
    }

    bool validateFrame(
        const sim::LockstepPeer& peer,
        const RtsG3LockstepFrame& frame) const {
        if (frame.sessionId !=
                config_.sessionId ||
            frame.peerId != peer.peerId ||
            frame.commands.size() >
                config_.maximumCommandsPerFrame) {
            return false;
        }
        std::uint32_t previousSequence = 0;
        for (const auto& command :
             frame.commands) {
            const auto sequence =
                commandSequence(command);
            if (commandTick(command) != frame.tick ||
                commandIssuer(command) != peer.issuer ||
                sequence == 0 ||
                sequence <= previousSequence) {
                return false;
            }
            if (command.kind ==
                    G3NetworkCommandKind::Ability &&
                (!command.ability.caster.valid() ||
                 command.ability.abilityId == 0)) {
                return false;
            }
            previousSequence = sequence;
        }
        return true;
    }

    void rebuildSequences() {
        sequences_.clear();
        for (const auto& peer :
             coordinator_.peers()) {
            if (peer.role ==
                    sim::LockstepPeerRole::Player) {
                sequences_.push_back(
                    {peer.peerId, 1, 1});
            }
        }
        std::sort(
            sequences_.begin(), sequences_.end(),
            [](const auto& first, const auto& second) {
                return first.peerId <
                       second.peerId;
            });
    }

    RtsG3PeerSequence* findSequence(
        sim::LockstepPeerId peerId) noexcept {
        const auto found = std::lower_bound(
            sequences_.begin(), sequences_.end(),
            peerId,
            [](const RtsG3PeerSequence& value,
               sim::LockstepPeerId id) {
                return value.peerId < id;
            });
        return found != sequences_.end() &&
                       found->peerId == peerId
            ? &*found
            : nullptr;
    }

    static bool validateSequences(
        const std::vector<sim::LockstepPeer>& peers,
        const std::vector<RtsG3PeerSequence>& sequences) {
        std::vector<sim::LockstepPeerId> expected;
        for (const auto& peer : peers) {
            if (peer.role ==
                sim::LockstepPeerRole::Player) {
                expected.push_back(peer.peerId);
            }
        }
        std::sort(expected.begin(), expected.end());
        if (expected.size() != sequences.size()) {
            return false;
        }
        for (std::size_t index = 0;
             index < expected.size();
             ++index) {
            if (sequences[index].peerId !=
                    expected[index] ||
                sequences[index].
                        nextFrameSequence == 0 ||
                sequences[index].
                        nextCommandSequence == 0) {
                return false;
            }
        }
        return true;
    }

    void submitCommands(
        const std::vector<G3NetworkCommand>& commands) {
        for (const auto& command : commands) {
            if (command.kind ==
                G3NetworkCommandKind::Base) {
                const auto result =
                    session_.base().
                        submitDetailed(command.base);
                outcomes_.push_back({
                    command.base.targetTick,
                    command.base.issuer,
                    command.base.sequence,
                    RtsG3CommandOutcomeKind::Base,
                    result,
                    AbilitySubmitResult::Accepted});
            } else {
                const auto result =
                    session_.submitAbility(
                        command.ability);
                outcomes_.push_back({
                    command.ability.targetTick,
                    command.ability.issuer,
                    command.ability.sequence,
                    RtsG3CommandOutcomeKind::Ability,
                    SessionCommandResult::Accepted,
                    result});
            }
        }
    }

    bool captureCurrent(bool force) {
        const auto archive = session_.encode();
        if (archive.empty()) return false;
        const auto resumeTick =
            session_.base().simulation().
                nextExpectedTick();
        const auto hash =
            session_.authoritativeHash();
        if (!checkpoints_.capture(
                {resumeTick, hash, archive},
                force)) {
            return false;
        }
        if (resumeTick != 0) {
            desync_.recordLocal(
                resumeTick - 1u, hash);
        }
        return true;
    }

    void recordCompletedTick(std::uint64_t tick) {
        const auto hash =
            session_.authoritativeHash();
        desync_.recordLocal(tick, hash);
        if (checkpoints_.shouldCapture(tick + 1u)) {
            const auto archive = session_.encode();
            if (!archive.empty()) {
                (void)checkpoints_.capture(
                    {tick + 1u, hash, archive});
            }
        }
    }

    bool restoreCheckpoint(
        const sim::RollbackCheckpoint& checkpoint) {
        return !checkpoint.archive.empty() &&
               session_.decode(checkpoint.archive) &&
               session_.base().simulation().
                       nextExpectedTick() ==
                   checkpoint.resumeTick &&
               session_.authoritativeHash() ==
                   checkpoint.authoritativeHash;
    }

    bool replayRange(
        std::uint64_t begin,
        std::uint64_t endExclusive) {
        for (std::uint64_t tick = begin;
             tick < endExclusive;
             ++tick) {
            coordinator_.
                collectCommandsForReplay(
                    tick, commandScratch_);
            submitCommands(commandScratch_);
            if (session_.stepDetailed(tick) !=
                    RtsStepResult::Advanced ||
                !coordinator_.markSimulated(tick)) {
                return false;
            }
            recordCompletedTick(tick);
        }
        return true;
    }

    RtsG3LockstepAdvanceResult rollbackAndReplay() {
        const auto requested =
            coordinator_.rollbackFrom();
        const auto oldThrough =
            coordinator_.simulatedThrough();
        const auto* selected =
            checkpoints_.latestAtOrBefore(
                requested);
        if (!selected) {
            return RtsG3LockstepAdvanceResult::
                RollbackUnavailable;
        }
        const auto checkpoint = *selected;
        if (!restoreCheckpoint(checkpoint) ||
            !coordinator_.rewindSimulatedThrough(
                checkpoint.resumeTick)) {
            return RtsG3LockstepAdvanceResult::
                RollbackRestoreFailed;
        }
        checkpoints_.discardAfter(
            checkpoint.resumeTick);
        desync_.rewindFrom(checkpoint.resumeTick);
        outcomes_.clear();
        if (!replayRange(
                checkpoint.resumeTick,
                oldThrough)) {
            return RtsG3LockstepAdvanceResult::
                RollbackRestoreFailed;
        }
        coordinator_.clearRollback();
        lastRollback_ = {
            requested,
            checkpoint.resumeTick,
            oldThrough == 0 ? 0 : oldThrough - 1u,
            static_cast<std::uint32_t>(
                oldThrough -
                checkpoint.resumeTick)};
        return RtsG3LockstepAdvanceResult::
            AdvancedAfterRollback;
    }

    RtsG3GameSession& session_;
    RtsG3LockstepConfig config_;
    sim::LockstepCoordinator<G3NetworkCommand>
        coordinator_;
    sim::RollbackCheckpointRing checkpoints_;
    sim::DesyncMonitor desync_;
    std::vector<RtsG3PeerSequence> sequences_;
    std::vector<G3NetworkCommand> commandScratch_;
    std::vector<RtsG3CommandOutcome> outcomes_;
    RtsG3RollbackReport lastRollback_{};
    bool started_{};
};

} // namespace rts::gameplay
