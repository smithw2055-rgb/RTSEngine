#pragma once

#include <RTSEngine/Rts/RtsLobby.h>
#include <RTSEngine/Rts/RtsLockstepArchive.h>
#include <RTSEngine/Rts/RtsMultiplayerRuntime.h>
#include <RTSEngine/Rts/RtsNetworkEndpoint.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct RtsMultiplayerRuntimeConfig final {
    RtsNetworkDelivery frameDelivery{RtsNetworkDelivery::Reliable};
    RtsNetworkDelivery hashDelivery{RtsNetworkDelivery::Unreliable};
};

class RtsMultiplayerHost final {
public:
    RtsMultiplayerHost(
        RtsGameSession& session,
        RtsNetworkEndpoint& network,
        RtsLobbyHostConfig lobbyConfig,
        RtsLockstepConfig lockstepConfig,
        std::string hostName,
        RtsMultiplayerRuntimeConfig runtimeConfig = {})
        : session_(session),
          network_(network),
          lobby_(std::move(lobbyConfig)),
          lockstepConfig_(lockstepConfig),
          runtimeConfig_(runtimeConfig) {
        valid_ = lockstepConfig_.sessionId == lobby_.config().sessionId &&
                 lobby_.registerHost(
                     network_.localEndpoint(), std::move(hostName), true);
    }

    bool valid() const noexcept { return valid_; }
    bool started() const noexcept { return lockstep_ != nullptr; }
    const RtsLobbyHost& lobby() const noexcept { return lobby_; }
    const RtsLockstepSession* lockstep() const noexcept {
        return lockstep_.get();
    }

    bool addRemoteEndpoint(network::NetworkEndpointId endpoint) {
        return valid_ && network_.addPeer(endpoint);
    }

    void update(std::uint64_t nowMs) {
        if (!valid_) return;
        network_.update(nowMs);
        RtsReceivedNetworkMessage message;
        while (network_.poll(message)) process(message);
    }

    bool startMatch() {
        if (!valid_ || lockstep_ || !lobby_.start()) return false;
        lockstep_ = std::make_unique<RtsLockstepSession>(
            session_, lockstepConfig_);
        for (const auto& peer : lobby_.lockstepPeers()) {
            if (!lockstep_->registerPeer(peer)) {
                lockstep_.reset();
                return false;
            }
        }
        if (lockstep_->start() != RtsLockstepStartResult::Started) {
            lockstep_.reset();
            return false;
        }

        RtsStartNotice notice{lockstepConfig_, lobby_.lockstepPeers()};
        broadcast(
            {RtsNetworkMessageKind::Start,
             lobby_.config().sessionId,
             nextRequestId_++,
             EncodeRtsStartNotice(notice)},
            RtsNetworkDelivery::Reliable);

        RtsReconnectSnapshot snapshot;
        if (!lockstep_->makeReconnectSnapshot(snapshot)) return false;
        broadcast(
            {RtsNetworkMessageKind::ReconnectSnapshot,
             lobby_.config().sessionId,
             nextRequestId_++,
             EncodeRtsReconnectSnapshot(snapshot)},
            RtsNetworkDelivery::Reliable);
        broadcastLobby();
        return true;
    }

    bool submitLocal(std::vector<TickCommand> commands) {
        if (!lockstep_) return false;
        const auto* host = memberByEndpoint(network_.localEndpoint());
        if (!host) return false;
        RtsLockstepFrame frame;
        if (!lockstep_->buildLocalFrame(
                host->peer.peerId, std::move(commands), frame)) {
            return false;
        }
        const auto result = lockstep_->receiveFrame(frame);
        if (result != sim::LockstepFrameSubmitResult::Accepted &&
            result != sim::LockstepFrameSubmitResult::Duplicate) {
            return false;
        }
        broadcastFrame(frame, network_.localEndpoint());
        return true;
    }

    RtsLockstepAdvanceResult advanceOne() {
        if (!lockstep_) return RtsLockstepAdvanceResult::NotStarted;
        const auto result = lockstep_->advanceOne();
        if (result == RtsLockstepAdvanceResult::Advanced ||
            result == RtsLockstepAdvanceResult::AdvancedAfterRollback) {
            const auto completedTick = lockstep_->coordinator().simulatedThrough() - 1u;
            const auto* host = memberByEndpoint(network_.localEndpoint());
            sim::StateHashReport report;
            if (host && lockstep_->hashReportDue(completedTick) &&
                lockstep_->makeHashReport(
                    host->peer.peerId, completedTick, report)) {
                broadcast(
                    {RtsNetworkMessageKind::HashReport,
                     lobby_.config().sessionId,
                     nextRequestId_++,
                     EncodeRtsStateHashReport(report)},
                    runtimeConfig_.hashDelivery);
            }
        }
        return result;
    }

private:
    void process(const RtsReceivedNetworkMessage& message) {
        switch (message.envelope.kind) {
        case RtsNetworkMessageKind::Hello:
            processHello(message);
            break;
        case RtsNetworkMessageKind::Ready:
            processReady(message);
            break;
        case RtsNetworkMessageKind::LockstepFrame:
            processFrame(message);
            break;
        case RtsNetworkMessageKind::HashReport:
            processHash(message);
            break;
        case RtsNetworkMessageKind::ReconnectRequest:
            sendReconnect(message.source, message.envelope.requestId);
            break;
        case RtsNetworkMessageKind::Ping:
            sendTo(
                message.source,
                {RtsNetworkMessageKind::Pong,
                 lobby_.config().sessionId,
                 message.envelope.requestId,
                 message.envelope.payload},
                RtsNetworkDelivery::Unreliable);
            break;
        case RtsNetworkMessageKind::Disconnect:
            if (!started() && lobby_.remove(message.source)) broadcastLobby();
            break;
        default:
            break;
        }
    }

    void processHello(const RtsReceivedNetworkMessage& message) {
        RtsNetworkHello hello;
        if (!DecodeRtsNetworkHello(message.envelope.payload, hello)) return;
        RtsNetworkWelcome welcome;
        RtsNetworkReject reject;
        if (lobby_.join(message.source, hello, welcome, reject) ==
            RtsLobbyJoinResult::Accepted) {
            sendTo(
                message.source,
                {RtsNetworkMessageKind::Welcome,
                 lobby_.config().sessionId,
                 message.envelope.requestId,
                 EncodeRtsNetworkWelcome(welcome)},
                RtsNetworkDelivery::Reliable);
            broadcastLobby();
        } else {
            sendTo(
                message.source,
                {RtsNetworkMessageKind::Reject,
                 lobby_.config().sessionId,
                 message.envelope.requestId,
                 EncodeRtsNetworkReject(reject)},
                RtsNetworkDelivery::Reliable);
        }
    }

    void processReady(const RtsReceivedNetworkMessage& message) {
        RtsReadyRequest ready;
        if (DecodeRtsReadyRequest(message.envelope.payload, ready) &&
            lobby_.setReady(message.source, ready.peerId, ready.ready)) {
            broadcastLobby();
        }
    }

    void processFrame(const RtsReceivedNetworkMessage& message) {
        if (!lockstep_) return;
        RtsLockstepFrame frame;
        if (!DecodeRtsLockstepFrame(
                message.envelope.payload,
                frame,
                lockstepConfig_.maximumCommandsPerFrame)) {
            return;
        }
        const auto* member = memberByEndpoint(message.source);
        if (!member || frame.peerId != member->peer.peerId) return;
        const auto result = lockstep_->receiveFrame(frame);
        if (result == sim::LockstepFrameSubmitResult::Accepted ||
            result == sim::LockstepFrameSubmitResult::Duplicate) {
            broadcastFrame(frame, message.source);
        }
    }

    void processHash(const RtsReceivedNetworkMessage& message) {
        if (!lockstep_) return;
        sim::StateHashReport report;
        if (!DecodeRtsStateHashReport(message.envelope.payload, report)) return;
        const auto* member = memberByEndpoint(message.source);
        if (member && report.peerId == member->peer.peerId) {
            (void)lockstep_->receiveHashReport(report);
        }
    }

    void sendReconnect(
        network::NetworkEndpointId destination,
        std::uint64_t requestId) {
        if (!lockstep_) return;
        RtsReconnectSnapshot snapshot;
        if (!lockstep_->makeReconnectSnapshot(snapshot)) return;
        sendTo(
            destination,
            {RtsNetworkMessageKind::ReconnectSnapshot,
             lobby_.config().sessionId,
             requestId,
             EncodeRtsReconnectSnapshot(snapshot)},
            RtsNetworkDelivery::Reliable);
    }

    void broadcastLobby() {
        broadcast(
            {RtsNetworkMessageKind::LobbySnapshot,
             lobby_.config().sessionId,
             nextRequestId_++,
             EncodeRtsLobbySnapshot(lobby_.snapshot())},
            RtsNetworkDelivery::Reliable);
    }

    void broadcastFrame(
        const RtsLockstepFrame& frame,
        network::NetworkEndpointId source) {
        RtsNetworkEnvelope envelope{
            RtsNetworkMessageKind::LockstepFrame,
            lobby_.config().sessionId,
            nextRequestId_++,
            EncodeRtsLockstepFrame(
                frame, lockstepConfig_.maximumCommandsPerFrame)};
        for (const auto& member : lobby_.members()) {
            if (member.endpoint != network_.localEndpoint() &&
                member.endpoint != source) {
                sendTo(member.endpoint, envelope, runtimeConfig_.frameDelivery);
            }
        }
    }

    void broadcast(
        const RtsNetworkEnvelope& envelope,
        RtsNetworkDelivery delivery) {
        for (const auto& member : lobby_.members()) {
            if (member.endpoint != network_.localEndpoint()) {
                sendTo(member.endpoint, envelope, delivery);
            }
        }
    }

    void sendTo(
        network::NetworkEndpointId destination,
        RtsNetworkEnvelope envelope,
        RtsNetworkDelivery delivery) {
        (void)network_.send(destination, std::move(envelope), delivery);
    }

    const RtsLobbyMember* memberByEndpoint(
        network::NetworkEndpointId endpoint) const noexcept {
        for (const auto& member : lobby_.members()) {
            if (member.endpoint == endpoint) return &member;
        }
        return nullptr;
    }

    RtsGameSession& session_;
    RtsNetworkEndpoint& network_;
    RtsLobbyHost lobby_;
    RtsLockstepConfig lockstepConfig_;
    RtsMultiplayerRuntimeConfig runtimeConfig_;
    std::unique_ptr<RtsLockstepSession> lockstep_;
    std::uint64_t nextRequestId_{1};
    bool valid_{};
};

class RtsMultiplayerClient final {
public:
    RtsMultiplayerClient(
        RtsGameSession& session,
        RtsNetworkEndpoint& network,
        network::NetworkEndpointId hostEndpoint,
        RtsNetworkHello hello,
        RtsMultiplayerRuntimeConfig runtimeConfig = {})
        : session_(session),
          network_(network),
          hostEndpoint_(hostEndpoint),
          lobby_(std::move(hello)),
          runtimeConfig_(runtimeConfig) {}

    const RtsLobbyClient& lobby() const noexcept { return lobby_; }
    bool started() const noexcept { return restored_ && lockstep_ != nullptr; }
    const RtsLockstepSession* lockstep() const noexcept {
        return lockstep_.get();
    }

    bool connect() {
        if (hostEndpoint_ == 0 || !network_.addPeer(hostEndpoint_)) return false;
        lobby_.markJoining();
        return network_.send(
            hostEndpoint_,
            {RtsNetworkMessageKind::Hello,
             0,
             nextRequestId_++,
             EncodeRtsNetworkHello(lobby_.hello())},
            RtsNetworkDelivery::Reliable);
    }

    void update(std::uint64_t nowMs) {
        network_.update(nowMs);
        RtsReceivedNetworkMessage message;
        while (network_.poll(message)) {
            if (message.source == hostEndpoint_) process(message);
        }
        tryRestorePendingSnapshot();
    }

    bool setReady(bool ready) {
        if (!lobby_.markReady(ready) || lobby_.welcome().peer.peerId == 0) {
            return false;
        }
        return network_.send(
            hostEndpoint_,
            {RtsNetworkMessageKind::Ready,
             lobby_.welcome().sessionId,
             nextRequestId_++,
             EncodeRtsReadyRequest(
                 {lobby_.welcome().peer.peerId, ready})},
            RtsNetworkDelivery::Reliable);
    }

    bool submitLocal(std::vector<TickCommand> commands) {
        if (!started()) return false;
        RtsLockstepFrame frame;
        if (!lockstep_->buildLocalFrame(
                lobby_.welcome().peer.peerId,
                std::move(commands),
                frame)) {
            return false;
        }
        const auto received = lockstep_->receiveFrame(frame);
        if (received != sim::LockstepFrameSubmitResult::Accepted &&
            received != sim::LockstepFrameSubmitResult::Duplicate) {
            return false;
        }
        return network_.send(
            hostEndpoint_,
            {RtsNetworkMessageKind::LockstepFrame,
             lobby_.welcome().sessionId,
             nextRequestId_++,
             EncodeRtsLockstepFrame(
                 frame, frame.commands.size() + 1u)},
            runtimeConfig_.frameDelivery);
    }

    RtsLockstepAdvanceResult advanceOne() {
        if (!started()) return RtsLockstepAdvanceResult::NotStarted;
        const auto result = lockstep_->advanceOne();
        if (result == RtsLockstepAdvanceResult::Advanced ||
            result == RtsLockstepAdvanceResult::AdvancedAfterRollback) {
            const auto completedTick = lockstep_->coordinator().simulatedThrough() - 1u;
            sim::StateHashReport report;
            if (lockstep_->hashReportDue(completedTick) &&
                lockstep_->makeHashReport(
                    lobby_.welcome().peer.peerId,
                    completedTick,
                    report)) {
                (void)network_.send(
                    hostEndpoint_,
                    {RtsNetworkMessageKind::HashReport,
                     lobby_.welcome().sessionId,
                     nextRequestId_++,
                     EncodeRtsStateHashReport(report)},
                    runtimeConfig_.hashDelivery);
            }
        }
        return result;
    }

    bool requestReconnect() {
        return network_.send(
            hostEndpoint_,
            {RtsNetworkMessageKind::ReconnectRequest,
             lobby_.welcome().sessionId,
             nextRequestId_++,
             {}},
            RtsNetworkDelivery::Reliable);
    }

private:
    void process(const RtsReceivedNetworkMessage& message) {
        switch (message.envelope.kind) {
        case RtsNetworkMessageKind::Welcome: {
            RtsNetworkWelcome welcome;
            if (DecodeRtsNetworkWelcome(message.envelope.payload, welcome)) {
                (void)lobby_.acceptWelcome(welcome);
            }
            break;
        }
        case RtsNetworkMessageKind::Reject: {
            RtsNetworkReject reject;
            if (DecodeRtsNetworkReject(message.envelope.payload, reject)) {
                lobby_.reject(reject);
            }
            break;
        }
        case RtsNetworkMessageKind::LobbySnapshot: {
            RtsLobbySnapshot snapshot;
            if (DecodeRtsLobbySnapshot(message.envelope.payload, snapshot)) {
                (void)lobby_.applySnapshot(std::move(snapshot));
            }
            break;
        }
        case RtsNetworkMessageKind::Start: {
            RtsStartNotice notice;
            if (DecodeRtsStartNotice(message.envelope.payload, notice)) {
                startNotice_ = std::move(notice);
                createLockstep();
            }
            break;
        }
        case RtsNetworkMessageKind::ReconnectSnapshot:
            pendingReconnect_ = message.envelope.payload;
            break;
        case RtsNetworkMessageKind::LockstepFrame: {
            if (!lockstep_) break;
            RtsLockstepFrame frame;
            if (DecodeRtsLockstepFrame(
                    message.envelope.payload,
                    frame,
                    startNotice_.lockstep.maximumCommandsPerFrame)) {
                (void)lockstep_->receiveFrame(std::move(frame));
            }
            break;
        }
        case RtsNetworkMessageKind::HashReport: {
            if (!lockstep_) break;
            sim::StateHashReport report;
            if (DecodeRtsStateHashReport(message.envelope.payload, report)) {
                (void)lockstep_->receiveHashReport(report);
            }
            break;
        }
        case RtsNetworkMessageKind::Ping:
            (void)network_.send(
                hostEndpoint_,
                {RtsNetworkMessageKind::Pong,
                 lobby_.welcome().sessionId,
                 message.envelope.requestId,
                 message.envelope.payload},
                RtsNetworkDelivery::Unreliable);
            break;
        default:
            break;
        }
    }

    void createLockstep() {
        if (lockstep_ || startNotice_.lockstep.sessionId == 0) return;
        lockstep_ = std::make_unique<RtsLockstepSession>(
            session_, startNotice_.lockstep);
        for (const auto& peer : startNotice_.peers) {
            if (!lockstep_->registerPeer(peer)) {
                lockstep_.reset();
                return;
            }
        }
        if (lockstep_->start() != RtsLockstepStartResult::Started) {
            lockstep_.reset();
        }
    }

    void tryRestorePendingSnapshot() {
        if (!lockstep_ || pendingReconnect_.empty()) return;
        RtsReconnectSnapshot snapshot;
        if (DecodeRtsReconnectSnapshot(pendingReconnect_, snapshot) &&
            lockstep_->restoreReconnectSnapshot(std::move(snapshot))) {
            restored_ = true;
            pendingReconnect_.clear();
        }
    }

    RtsGameSession& session_;
    RtsNetworkEndpoint& network_;
    network::NetworkEndpointId hostEndpoint_{};
    RtsLobbyClient lobby_;
    RtsMultiplayerRuntimeConfig runtimeConfig_;
    RtsStartNotice startNotice_;
    std::vector<std::uint8_t> pendingReconnect_;
    std::unique_ptr<RtsLockstepSession> lockstep_;
    std::uint64_t nextRequestId_{1};
    bool restored_{};
};

} // namespace rts::gameplay
