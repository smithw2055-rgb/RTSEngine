#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace rts::sim {

using LockstepSessionId = std::uint64_t;
using LockstepPeerId = std::uint32_t;
using LockstepPlayerSlot = std::uint32_t;

inline constexpr std::uint64_t kNoRollbackTick =
    std::numeric_limits<std::uint64_t>::max();

enum class LockstepPeerRole : std::uint8_t {
    Player,
    Spectator
};

struct LockstepPeer final {
    LockstepPeerId peerId{};
    LockstepPlayerSlot playerSlot{};
    std::uint32_t issuer{};
    LockstepPeerRole role{LockstepPeerRole::Player};
    bool active{true};
};

struct LockstepCoordinatorConfig final {
    LockstepSessionId sessionId{};
    std::uint32_t inputDelayTicks{2};
    std::uint32_t maximumPredictionTicks{};
};

template<class Command>
struct LockstepCommandFrame final {
    LockstepSessionId sessionId{};
    std::uint64_t tick{};
    LockstepPeerId peerId{};
    std::uint64_t frameSequence{};
    std::vector<Command> commands;
};

enum class LockstepFrameSubmitResult : std::uint8_t {
    Accepted,
    Duplicate,
    Conflict,
    NotStarted,
    WrongSession,
    UnknownPeer,
    SpectatorCannotSubmit,
    InactivePeer,
    InvalidTick,
    InvalidFrame
};

template<class Command>
class LockstepCoordinator final {
public:
    using Frame = LockstepCommandFrame<Command>;

    explicit LockstepCoordinator(
        LockstepCoordinatorConfig config = {}) noexcept
        : config_(config) {}

    const LockstepCoordinatorConfig& config() const noexcept {
        return config_;
    }

    bool registerPeer(LockstepPeer peer) {
        if (started_ || peer.peerId == 0 ||
            (peer.role == LockstepPeerRole::Player &&
             (peer.playerSlot == 0 || peer.issuer == 0)) ||
            (peer.role == LockstepPeerRole::Spectator &&
             peer.issuer != 0)) {
            return false;
        }
        for (const auto& existing : peers_) {
            if (existing.peerId == peer.peerId ||
                (peer.role == LockstepPeerRole::Player &&
                 existing.role == LockstepPeerRole::Player &&
                 (existing.playerSlot == peer.playerSlot ||
                  existing.issuer == peer.issuer))) {
                return false;
            }
        }
        peers_.push_back(peer);
        return true;
    }

    bool start(std::uint64_t firstTick) {
        if (started_ || config_.sessionId == 0) return false;
        const auto activePlayers = std::count_if(
            peers_.begin(), peers_.end(),
            [](const LockstepPeer& peer) {
                return peer.active &&
                       peer.role == LockstepPeerRole::Player;
            });
        if (activePlayers == 0) return false;
        std::sort(
            peers_.begin(), peers_.end(),
            [](const LockstepPeer& first, const LockstepPeer& second) {
                if (first.role != second.role) {
                    return first.role == LockstepPeerRole::Player;
                }
                if (first.playerSlot != second.playerSlot) {
                    return first.playerSlot < second.playerSlot;
                }
                return first.peerId < second.peerId;
            });
        firstTick_ = firstTick;
        simulatedThrough_ = firstTick;
        confirmedThrough_ = firstTick;
        rollbackFrom_ = kNoRollbackTick;
        started_ = true;
        return true;
    }

    bool started() const noexcept { return started_; }
    std::uint64_t firstTick() const noexcept { return firstTick_; }
    std::uint64_t simulatedThrough() const noexcept {
        return simulatedThrough_;
    }
    std::uint64_t confirmedThrough() const noexcept {
        return confirmedThrough_;
    }
    std::uint64_t predictionDepth() const noexcept {
        return simulatedThrough_ > confirmedThrough_
            ? simulatedThrough_ - confirmedThrough_
            : 0;
    }

    std::uint64_t scheduledTick(std::uint64_t currentTick) const noexcept {
        const auto remaining =
            std::numeric_limits<std::uint64_t>::max() - currentTick;
        return currentTick + std::min<std::uint64_t>(
            config_.inputDelayTicks, remaining);
    }

    const std::vector<LockstepPeer>& peers() const noexcept {
        return peers_;
    }

    const LockstepPeer* peer(LockstepPeerId peerId) const noexcept {
        const auto found = std::find_if(
            peers_.begin(), peers_.end(),
            [peerId](const LockstepPeer& value) {
                return value.peerId == peerId;
            });
        return found == peers_.end() ? nullptr : &*found;
    }

    const std::vector<Frame>& frames() const noexcept { return frames_; }

    template<class ValidateFrame, class EqualCommand>
    LockstepFrameSubmitResult submitFrame(
        Frame frame,
        ValidateFrame&& validateFrame,
        EqualCommand&& equalCommand) {
        if (!started_) return LockstepFrameSubmitResult::NotStarted;
        if (frame.sessionId != config_.sessionId) {
            return LockstepFrameSubmitResult::WrongSession;
        }
        const auto* owner = peer(frame.peerId);
        if (!owner) return LockstepFrameSubmitResult::UnknownPeer;
        if (!owner->active) return LockstepFrameSubmitResult::InactivePeer;
        if (owner->role != LockstepPeerRole::Player) {
            return LockstepFrameSubmitResult::SpectatorCannotSubmit;
        }
        if (frame.tick < firstTick_) {
            return LockstepFrameSubmitResult::InvalidTick;
        }
        if (!validateFrame(*owner, frame)) {
            return LockstepFrameSubmitResult::InvalidFrame;
        }

        auto&& equals = equalCommand;
        const auto found = lowerFrame(frame.tick, frame.peerId);
        if (found != frames_.end() &&
            found->tick == frame.tick && found->peerId == frame.peerId) {
            return sameFrame(*found, frame, equals)
                ? LockstepFrameSubmitResult::Duplicate
                : LockstepFrameSubmitResult::Conflict;
        }

        const auto receivedTick = frame.tick;
        const bool lateNonEmpty =
            receivedTick < simulatedThrough_ && !frame.commands.empty();
        frames_.insert(found, std::move(frame));
        if (lateNonEmpty) {
            rollbackFrom_ = std::min(rollbackFrom_, receivedTick);
        }
        advanceConfirmedThrough();
        return LockstepFrameSubmitResult::Accepted;
    }

    template<class ValidateFrame, class EqualCommand>
    bool bootstrapEmptyFrames(
        std::uint64_t endExclusive,
        ValidateFrame&& validateFrame,
        EqualCommand&& equalCommand) {
        if (!started_ || endExclusive < firstTick_) return false;
        for (std::uint64_t tick = firstTick_; tick < endExclusive; ++tick) {
            for (const auto& value : peers_) {
                if (!value.active ||
                    value.role != LockstepPeerRole::Player) {
                    continue;
                }
                Frame frame;
                frame.sessionId = config_.sessionId;
                frame.tick = tick;
                frame.peerId = value.peerId;
                const auto result = submitFrame(
                    std::move(frame), validateFrame, equalCommand);
                if (result != LockstepFrameSubmitResult::Accepted &&
                    result != LockstepFrameSubmitResult::Duplicate) {
                    return false;
                }
            }
        }
        return true;
    }

    bool allActual(std::uint64_t tick) const noexcept {
        if (!started_) return false;
        for (const auto& value : peers_) {
            if (!value.active ||
                value.role != LockstepPeerRole::Player) {
                continue;
            }
            if (!findFrame(tick, value.peerId)) return false;
        }
        return true;
    }

    bool canAdvance(std::uint64_t tick) const noexcept {
        if (!started_ || tick != simulatedThrough_) return false;
        if (allActual(tick)) return true;
        if (config_.maximumPredictionTicks == 0 ||
            tick < confirmedThrough_) {
            return false;
        }
        return tick - confirmedThrough_ <
               config_.maximumPredictionTicks;
    }

    bool collectCommands(
        std::uint64_t tick,
        std::vector<Command>& output) const {
        if (!canAdvance(tick)) return false;
        collectCommandsUnchecked(tick, output);
        return true;
    }

    void collectCommandsForReplay(
        std::uint64_t tick,
        std::vector<Command>& output) const {
        collectCommandsUnchecked(tick, output);
    }

    bool markSimulated(std::uint64_t tick) noexcept {
        if (!started_ || tick != simulatedThrough_ ||
            simulatedThrough_ ==
                std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        ++simulatedThrough_;
        return true;
    }

    bool rewindSimulatedThrough(std::uint64_t nextTick) noexcept {
        if (!started_ || nextTick < firstTick_ ||
            nextTick > simulatedThrough_) {
            return false;
        }
        simulatedThrough_ = nextTick;
        return true;
    }

    bool rollbackRequired() const noexcept {
        return rollbackFrom_ != kNoRollbackTick;
    }
    std::uint64_t rollbackFrom() const noexcept { return rollbackFrom_; }
    void clearRollback() noexcept { rollbackFrom_ = kNoRollbackTick; }

    template<class ValidateFrame, class EqualCommand>
    bool restoreConfirmedState(
        std::uint64_t nextTick,
        std::vector<Frame> futureFrames,
        ValidateFrame&& validateFrame,
        EqualCommand&& equalCommand) {
        if (!started_ || nextTick < firstTick_) return false;
        frames_.clear();
        simulatedThrough_ = nextTick;
        confirmedThrough_ = nextTick;
        rollbackFrom_ = kNoRollbackTick;
        for (auto& frame : futureFrames) {
            if (frame.tick < nextTick) return false;
            const auto result = submitFrame(
                std::move(frame), validateFrame, equalCommand);
            if (result != LockstepFrameSubmitResult::Accepted) return false;
        }
        return true;
    }

private:
    using FrameIterator = typename std::vector<Frame>::iterator;
    using FrameConstIterator = typename std::vector<Frame>::const_iterator;

    static auto frameIdentity(
        std::uint64_t tick,
        LockstepPeerId peerId) noexcept {
        return std::make_tuple(tick, peerId);
    }

    static auto frameIdentity(const Frame& frame) noexcept {
        return frameIdentity(frame.tick, frame.peerId);
    }

    FrameIterator lowerFrame(
        std::uint64_t tick,
        LockstepPeerId peerId) noexcept {
        return std::lower_bound(
            frames_.begin(), frames_.end(),
            frameIdentity(tick, peerId),
            [](const Frame& frame, const auto& identity) {
                return frameIdentity(frame) < identity;
            });
    }

    FrameConstIterator lowerFrame(
        std::uint64_t tick,
        LockstepPeerId peerId) const noexcept {
        return std::lower_bound(
            frames_.begin(), frames_.end(),
            frameIdentity(tick, peerId),
            [](const Frame& frame, const auto& identity) {
                return frameIdentity(frame) < identity;
            });
    }

    const Frame* findFrame(
        std::uint64_t tick,
        LockstepPeerId peerId) const noexcept {
        const auto found = lowerFrame(tick, peerId);
        return found != frames_.end() &&
               found->tick == tick && found->peerId == peerId
            ? &*found
            : nullptr;
    }

    template<class EqualCommand>
    static bool sameFrame(
        const Frame& first,
        const Frame& second,
        EqualCommand& equalCommand) {
        if (first.frameSequence != second.frameSequence ||
            first.commands.size() != second.commands.size()) {
            return false;
        }
        for (std::size_t index = 0;
             index < first.commands.size(); ++index) {
            if (!equalCommand(
                    first.commands[index], second.commands[index])) {
                return false;
            }
        }
        return true;
    }

    void advanceConfirmedThrough() noexcept {
        while (confirmedThrough_ <
                   std::numeric_limits<std::uint64_t>::max() &&
               allActual(confirmedThrough_)) {
            ++confirmedThrough_;
        }
    }

    void collectCommandsUnchecked(
        std::uint64_t tick,
        std::vector<Command>& output) const {
        output.clear();
        for (const auto& value : peers_) {
            if (!value.active ||
                value.role != LockstepPeerRole::Player) {
                continue;
            }
            const auto* frame = findFrame(tick, value.peerId);
            if (!frame) continue;
            output.insert(
                output.end(),
                frame->commands.begin(),
                frame->commands.end());
        }
    }

    LockstepCoordinatorConfig config_;
    std::vector<LockstepPeer> peers_;
    std::vector<Frame> frames_;
    std::uint64_t firstTick_{};
    std::uint64_t simulatedThrough_{};
    std::uint64_t confirmedThrough_{};
    std::uint64_t rollbackFrom_{kNoRollbackTick};
    bool started_{};
};

} // namespace rts::sim
