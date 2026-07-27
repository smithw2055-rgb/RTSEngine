#pragma once

#include <RTSEngine/Rts/RtsAuthenticationRuntime.h>
#include <RTSEngine/Rts/RtsConnectionQuality.h>
#include <RTSEngine/Rts/RtsHostMigration.h>
#include <RTSEngine/Rts/RtsLobby.h>
#include <RTSEngine/Rts/RtsLockstepArchive.h>
#include <RTSEngine/Rts/RtsNetworkEndpoint.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct RtsOnlineMultiplayerRuntimeConfig final {
    RtsNetworkDelivery frameDelivery{RtsNetworkDelivery::Unreliable};
    RtsNetworkDelivery hashDelivery{RtsNetworkDelivery::Unreliable};
    bool adaptiveDelivery{true};
    bool requirePacketSecurityAfterAuthentication{};
    std::uint32_t simulationTickMilliseconds{50};
    network::ConnectionQualityConfig quality;
};

struct RtsOnlineHostRuntimeConfig final {
    RtsOnlineMultiplayerRuntimeConfig multiplayer;
    RtsAuthenticationRuntimeConfig authentication;
    bool allowHostMigration{true};
    std::uint64_t securityEpoch{1};
};

struct RtsOnlineClientRuntimeConfig final {
    RtsOnlineMultiplayerRuntimeConfig multiplayer;
    IRtsClientAuthenticationProvider* authentication{};
};

class RtsOnlineMultiplayerHost final {
public:
    RtsOnlineMultiplayerHost(
        RtsGameSession& session,
        RtsNetworkEndpoint& network,
        RtsLobbyHostConfig lobbyConfig,
        RtsLockstepConfig lockstepConfig,
        std::string hostName,
        RtsOnlineHostRuntimeConfig config = {})
        : session_(session),
          network_(network),
          lobby_(std::move(lobbyConfig)),
          lockstepConfig_(lockstepConfig),
          config_(sanitize(config)),
          authentication_(config_.authentication),
          quality_(config_.multiplayer.quality) {
        valid_ = lockstepConfig_.sessionId == lobby_.config().sessionId &&
                 config_.securityEpoch != 0 &&
                 (!authentication_.required() || authentication_.service()) &&
                 lobby_.registerHost(
                     network_.localEndpoint(), std::move(hostName), true);
    }

    RtsOnlineMultiplayerHost(
        RtsGameSession& session,
        RtsNetworkEndpoint& network,
        RtsLobbyHostConfig lobbyConfig,
        const RtsHostMigrationPackage& migration,
        RtsOnlineHostRuntimeConfig config = {})
        : session_(session),
          network_(network),
          lobby_(std::move(lobbyConfig)),
          lockstepConfig_(migration.reconnect.config),
          config_(sanitize(config)),
          authentication_(config_.authentication),
          quality_(config_.multiplayer.quality) {
        valid_ = restoreMigration(migration);
    }

    bool valid() const noexcept { return valid_; }
    bool started() const noexcept { return lockstep_ != nullptr; }
    bool dedicated() const noexcept { return lobby_.config().dedicatedServer; }
    const RtsLobbyHost& lobby() const noexcept { return lobby_; }
    const RtsLockstepSession* lockstep() const noexcept { return lockstep_.get(); }
    const RtsServerAuthenticationRuntime& authentication() const noexcept {
        return authentication_;
    }

    bool addRemoteEndpoint(network::NetworkEndpointId endpoint) {
        return valid_ && network_.addPeer(endpoint) && quality_.addPeer(endpoint);
    }

    void update(std::uint64_t nowMs) {
        if (!valid_) return;
        authentication_.expire(nowMs);
        quality_.update(nowMs);
        network_.update(nowMs);
        RtsReceivedNetworkMessage message;
        while (network_.poll(message)) process(message, nowMs);
        sendPings(nowMs);
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
        return broadcastStartAndSnapshot();
    }

    bool submitLocal(std::vector<TickCommand> commands) {
        if (!lockstep_ || dedicated()) return false;
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
            const auto completedTick =
                lockstep_->coordinator().simulatedThrough() - 1u;
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
                    config_.multiplayer.hashDelivery);
            }
        }
        return result;
    }

    const network::ConnectionQualitySnapshot* connectionQuality(
        network::NetworkEndpointId endpoint) const noexcept {
        return quality_.snapshot(endpoint);
    }

    const RtsAuthenticatedPrincipal* principal(
        network::NetworkEndpointId endpoint) const noexcept {
        const auto found = lowerPrincipal(endpoint);
        return found != principals_.end() && found->endpoint == endpoint
            ? &found->principal
            : nullptr;
    }

    bool setHostCandidate(
        network::NetworkEndpointId endpoint,
        bool active = true) {
        if (!lockstep_) return false;
        const auto* member = memberByEndpoint(endpoint);
        const auto* authenticated = principal(endpoint);
        if (!member || member->peer.role != sim::LockstepPeerRole::Player) {
            return false;
        }
        const auto* snapshot = quality_.snapshot(endpoint);
        RtsHostCandidate candidate;
        candidate.peerId = member->peer.peerId;
        candidate.endpoint = endpoint;
        candidate.principalId = authenticated ? authenticated->principalId : 0;
        candidate.confirmedThrough = lockstep_->coordinator().confirmedThrough();
        if (snapshot) candidate.quality = *snapshot;
        candidate.active = active;
        candidate.authorizedToHost = authenticated
            ? authenticated->hostCandidate()
            : !authentication_.required();
        return migrationElection_.setCandidate(candidate);
    }

    bool makeHostMigrationPackage(RtsHostMigrationPackage& package) {
        if (!config_.allowHostMigration || !lockstep_) return false;
        RtsHostMigrationDecision decision;
        if (!migrationElection_.elect(decision)) return false;
        RtsReconnectSnapshot reconnect;
        if (!lockstep_->makeReconnectSnapshot(reconnect)) return false;
        package.sessionId = lobby_.config().sessionId;
        package.migrationEpoch = decision.migrationEpoch;
        package.previousHostEndpoint = network_.localEndpoint();
        package.newHostEndpoint = decision.endpoint;
        package.newHostPeerId = decision.peerId;
        package.newHostPrincipalId = decision.principalId;
        package.securityEpoch = config_.securityEpoch + decision.migrationEpoch;
        package.identity = lobby_.config().identity;
        package.lobby = lobby_.snapshot();
        package.reconnect = std::move(reconnect);
        return !EncodeRtsHostMigrationPackage(package).empty();
    }

private:
    struct PrincipalEntry final {
        network::NetworkEndpointId endpoint{};
        RtsAuthenticatedPrincipal principal;
    };

    struct PendingMigrationRestoreTag final {};

    static RtsOnlineHostRuntimeConfig sanitize(
        RtsOnlineHostRuntimeConfig value) noexcept {
        value.securityEpoch = std::max<std::uint64_t>(1u, value.securityEpoch);
        value.multiplayer.simulationTickMilliseconds =
            std::max<std::uint32_t>(
                1u, value.multiplayer.simulationTickMilliseconds);
        return value;
    }

    bool restoreMigration(const RtsHostMigrationPackage& migration) {
        if (migration.sessionId == 0 ||
            migration.newHostEndpoint != network_.localEndpoint() ||
            migration.identity != lobby_.config().identity ||
            migration.sessionId != lobby_.config().sessionId ||
            migration.reconnect.config.sessionId != migration.sessionId ||
            !lobby_.restore(migration.lobby, network_.localEndpoint())) {
            return false;
        }
        config_.securityEpoch = migration.securityEpoch;
        for (const auto& member : lobby_.members()) {
            if (member.endpoint != network_.localEndpoint() &&
                (!network_.addPeer(member.endpoint) ||
                 !quality_.addPeer(member.endpoint))) {
                return false;
            }
        }
        lockstep_ = std::make_unique<RtsLockstepSession>(
            session_, migration.reconnect.config);
        for (const auto& peer : migration.reconnect.peers) {
            if (!lockstep_->registerPeer(peer)) {
                lockstep_.reset();
                return false;
            }
        }
        if (lockstep_->start() != RtsLockstepStartResult::Started ||
            !lockstep_->restoreReconnectSnapshot(migration.reconnect)) {
            lockstep_.reset();
            return false;
        }
        return true;
    }

    bool broadcastStartAndSnapshot() {
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

    void process(
        const RtsReceivedNetworkMessage& message,
        std::uint64_t nowMs) {
        switch (message.envelope.kind) {
        case RtsNetworkMessageKind::Hello:
            processHello(message, nowMs);
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
                 {}},
                RtsNetworkDelivery::Unreliable);
            break;
        case RtsNetworkMessageKind::Pong:
            (void)quality_.acknowledge(
                message.source, message.envelope.requestId, nowMs);
            break;
        case RtsNetworkMessageKind::Disconnect:
            if (!started() && lobby_.remove(message.source)) {
                quality_.removePeer(message.source);
                broadcastLobby();
            }
            break;
        default:
            break;
        }
    }

    void processHello(
        const RtsReceivedNetworkMessage& message,
        std::uint64_t nowMs) {
        RtsAuthenticatedHello authenticated;
        if (DecodeRtsAuthenticatedHello(
                message.envelope.payload, authenticated)) {
            processAuthenticatedHello(message, authenticated, nowMs);
            return;
        }

        RtsNetworkHello hello;
        if (!DecodeRtsNetworkHello(message.envelope.payload, hello)) return;
        RtsNetworkReject reject;
        if (lobby_.validateJoin(message.source, hello, reject) !=
            RtsLobbyJoinResult::Accepted) {
            sendLegacyReject(message.source, message.envelope.requestId, reject);
            return;
        }
        RtsAuthenticationChallengeNotice challenge;
        switch (authentication_.begin(
            message.source, hello, nowMs, challenge)) {
        case RtsAuthenticationBeginResult::Bypassed:
            joinUnauthenticated(message, hello);
            return;
        case RtsAuthenticationBeginResult::ChallengeIssued:
            sendTo(
                message.source,
                {RtsNetworkMessageKind::Reject,
                 lobby_.config().sessionId,
                 message.envelope.requestId,
                 EncodeRtsAuthenticationChallengeNotice(challenge)},
                RtsNetworkDelivery::Reliable);
            return;
        case RtsAuthenticationBeginResult::RateLimited:
            sendAuthenticationFailure(
                message.source,
                message.envelope.requestId,
                RtsAuthenticationFailureReason::RateLimited);
            return;
        case RtsAuthenticationBeginResult::ServiceUnavailable:
            sendAuthenticationFailure(
                message.source,
                message.envelope.requestId,
                RtsAuthenticationFailureReason::ServiceUnavailable);
            return;
        case RtsAuthenticationBeginResult::InvalidRequest:
            sendAuthenticationFailure(
                message.source,
                message.envelope.requestId,
                RtsAuthenticationFailureReason::InvalidResponse);
            return;
        }
    }

    void processAuthenticatedHello(
        const RtsReceivedNetworkMessage& message,
        RtsAuthenticatedHello authenticated,
        std::uint64_t nowMs) {
        RtsAuthenticatedPrincipal principalValue;
        const auto result = authentication_.verify(
            message.source, authenticated, nowMs, principalValue);
        if (result != RtsAuthenticationVerifyResult::Accepted) {
            sendAuthenticationFailure(
                message.source,
                message.envelope.requestId,
                ToAuthenticationFailureReason(result));
            return;
        }
        authenticated.hello.displayName = principalValue.displayName;
        RtsNetworkWelcome welcome;
        RtsNetworkReject reject;
        if (lobby_.join(
                message.source,
                authenticated.hello,
                welcome,
                reject) != RtsLobbyJoinResult::Accepted) {
            sendLegacyReject(message.source, message.envelope.requestId, reject);
            return;
        }

        RtsAuthenticatedWelcome accepted{welcome, principalValue};
        sendTo(
            message.source,
            {RtsNetworkMessageKind::Welcome,
             lobby_.config().sessionId,
             message.envelope.requestId,
             EncodeRtsAuthenticatedWelcome(accepted)},
            RtsNetworkDelivery::Reliable);
        storePrincipal(message.source, principalValue);

        auto* provider = authentication_.service()
            ? authentication_.service()->packetSecurityProvider()
            : nullptr;
        if (provider && principalValue.securityKeyId != 0) {
            (void)network_.enableSecurity(
                message.source,
                {provider,
                 principalValue.securityKeyId,
                 principalValue.securityEpoch,
                 true,
                 RtsGameSessionArchive::kMaximumNestedBytes + 1024u});
        } else if (config_.multiplayer.requirePacketSecurityAfterAuthentication) {
            (void)lobby_.remove(message.source);
            sendAuthenticationFailure(
                message.source,
                message.envelope.requestId,
                RtsAuthenticationFailureReason::ServiceUnavailable);
            return;
        }
        broadcastLobby();
    }

    void joinUnauthenticated(
        const RtsReceivedNetworkMessage& message,
        const RtsNetworkHello& hello) {
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
            sendLegacyReject(message.source, message.envelope.requestId, reject);
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

    void sendPings(std::uint64_t nowMs) {
        for (const auto& member : lobby_.members()) {
            if (member.endpoint == network_.localEndpoint() ||
                !quality_.pingDue(member.endpoint, nowMs)) {
                continue;
            }
            const auto pingId = quality_.beginPing(member.endpoint, nowMs);
            if (pingId != 0) {
                sendTo(
                    member.endpoint,
                    {RtsNetworkMessageKind::Ping,
                     lobby_.config().sessionId,
                     pingId,
                     {}},
                    RtsNetworkDelivery::Unreliable);
            }
        }
    }

    void sendLegacyReject(
        network::NetworkEndpointId destination,
        std::uint64_t requestId,
        const RtsNetworkReject& reject) {
        sendTo(
            destination,
            {RtsNetworkMessageKind::Reject,
             lobby_.config().sessionId,
             requestId,
             EncodeRtsNetworkReject(reject)},
            RtsNetworkDelivery::Reliable);
    }

    void sendAuthenticationFailure(
        network::NetworkEndpointId destination,
        std::uint64_t requestId,
        RtsAuthenticationFailureReason reason) {
        sendTo(
            destination,
            {RtsNetworkMessageKind::Reject,
             lobby_.config().sessionId,
             requestId,
             EncodeRtsAuthenticationFailureNotice({reason})},
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
        const RtsNetworkEnvelope envelope{
            RtsNetworkMessageKind::LockstepFrame,
            lobby_.config().sessionId,
            nextRequestId_++,
            EncodeRtsLockstepFrame(
                frame, lockstepConfig_.maximumCommandsPerFrame)};
        for (const auto& member : lobby_.members()) {
            if (member.endpoint == network_.localEndpoint() ||
                member.endpoint == source) {
                continue;
            }
            sendTo(
                member.endpoint,
                envelope,
                quality_.delivery(
                    member.endpoint,
                    config_.multiplayer.frameDelivery,
                    config_.multiplayer.adaptiveDelivery));
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
        const auto found = std::find_if(
            lobby_.members().begin(), lobby_.members().end(),
            [endpoint](const RtsLobbyMember& member) {
                return member.endpoint == endpoint;
            });
        return found == lobby_.members().end() ? nullptr : &*found;
    }

    using PrincipalIterator = std::vector<PrincipalEntry>::iterator;
    using PrincipalConstIterator = std::vector<PrincipalEntry>::const_iterator;

    PrincipalIterator lowerPrincipal(
        network::NetworkEndpointId endpoint) noexcept {
        return std::lower_bound(
            principals_.begin(), principals_.end(), endpoint,
            [](const PrincipalEntry& value, network::NetworkEndpointId id) {
                return value.endpoint < id;
            });
    }

    PrincipalConstIterator lowerPrincipal(
        network::NetworkEndpointId endpoint) const noexcept {
        return std::lower_bound(
            principals_.begin(), principals_.end(), endpoint,
            [](const PrincipalEntry& value, network::NetworkEndpointId id) {
                return value.endpoint < id;
            });
    }

    void storePrincipal(
        network::NetworkEndpointId endpoint,
        RtsAuthenticatedPrincipal principalValue) {
        const auto found = lowerPrincipal(endpoint);
        if (found != principals_.end() && found->endpoint == endpoint) {
            found->principal = std::move(principalValue);
        } else {
            principals_.insert(
                found, PrincipalEntry{endpoint, std::move(principalValue)});
        }
    }

    RtsGameSession& session_;
    RtsNetworkEndpoint& network_;
    RtsLobbyHost lobby_;
    RtsLockstepConfig lockstepConfig_;
    RtsOnlineHostRuntimeConfig config_;
    RtsServerAuthenticationRuntime authentication_;
    RtsConnectionQualityTable quality_;
    RtsHostMigrationElection migrationElection_;
    std::vector<PrincipalEntry> principals_;
    std::unique_ptr<RtsLockstepSession> lockstep_;
    std::uint64_t nextRequestId_{1};
    bool valid_{};
};

class RtsOnlineMultiplayerClient final {
public:
    RtsOnlineMultiplayerClient(
        RtsGameSession& session,
        RtsNetworkEndpoint& network,
        network::NetworkEndpointId hostEndpoint,
        RtsNetworkHello hello,
        RtsOnlineClientRuntimeConfig config = {})
        : session_(session),
          network_(network),
          hostEndpoint_(hostEndpoint),
          lobby_(std::move(hello)),
          config_(sanitize(config)),
          quality_(config_.multiplayer.quality) {}

    const RtsLobbyClient& lobby() const noexcept { return lobby_; }
    bool started() const noexcept { return restored_ && lockstep_ != nullptr; }
    const RtsLockstepSession* lockstep() const noexcept { return lockstep_.get(); }
    const RtsAuthenticatedPrincipal* principal() const noexcept {
        return authenticated_ ? &principal_ : nullptr;
    }
    RtsAuthenticationFailureReason authenticationFailure() const noexcept {
        return authenticationFailure_;
    }

    bool connect() {
        if (hostEndpoint_ == 0 || !network_.addPeer(hostEndpoint_) ||
            !quality_.addPeer(hostEndpoint_)) {
            return false;
        }
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
        quality_.update(nowMs);
        network_.update(nowMs);
        RtsReceivedNetworkMessage message;
        while (network_.poll(message)) {
            if (message.source == hostEndpoint_) process(message, nowMs);
        }
        tryRestorePendingSnapshot();
        sendPing(nowMs);
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
             EncodeRtsLockstepFrame(frame, frame.commands.size() + 1u)},
            quality_.delivery(
                hostEndpoint_,
                config_.multiplayer.frameDelivery,
                config_.multiplayer.adaptiveDelivery));
    }

    RtsLockstepAdvanceResult advanceOne() {
        if (!started()) return RtsLockstepAdvanceResult::NotStarted;
        const auto result = lockstep_->advanceOne();
        if (result == RtsLockstepAdvanceResult::Advanced ||
            result == RtsLockstepAdvanceResult::AdvancedAfterRollback) {
            const auto completedTick =
                lockstep_->coordinator().simulatedThrough() - 1u;
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
                    config_.multiplayer.hashDelivery);
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

    const network::ConnectionQualitySnapshot* connectionQuality() const noexcept {
        return quality_.snapshot(hostEndpoint_);
    }

private:
    static RtsOnlineClientRuntimeConfig sanitize(
        RtsOnlineClientRuntimeConfig value) noexcept {
        value.multiplayer.simulationTickMilliseconds =
            std::max<std::uint32_t>(
                1u, value.multiplayer.simulationTickMilliseconds);
        return value;
    }

    void process(
        const RtsReceivedNetworkMessage& message,
        std::uint64_t nowMs) {
        switch (message.envelope.kind) {
        case RtsNetworkMessageKind::Welcome:
            processWelcome(message);
            break;
        case RtsNetworkMessageKind::Reject:
            processReject(message);
            break;
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
                 {}},
                RtsNetworkDelivery::Unreliable);
            break;
        case RtsNetworkMessageKind::Pong:
            (void)quality_.acknowledge(
                hostEndpoint_, message.envelope.requestId, nowMs);
            break;
        default:
            break;
        }
    }

    void processReject(const RtsReceivedNetworkMessage& message) {
        RtsAuthenticationChallengeNotice challenge;
        if (DecodeRtsAuthenticationChallengeNotice(
                message.envelope.payload, challenge)) {
            if (!config_.authentication) {
                authenticationFailure_ =
                    RtsAuthenticationFailureReason::ServiceUnavailable;
                return;
            }
            RtsAuthenticationResponse response;
            if (!config_.authentication->makeResponse(
                    challenge.challenge, response)) {
                authenticationFailure_ =
                    RtsAuthenticationFailureReason::InvalidResponse;
                return;
            }
            (void)network_.send(
                hostEndpoint_,
                {RtsNetworkMessageKind::Hello,
                 0,
                 nextRequestId_++,
                 EncodeRtsAuthenticatedHello(
                     {lobby_.hello(), std::move(response)})},
                RtsNetworkDelivery::Reliable);
            return;
        }
        RtsAuthenticationFailureNotice failure;
        if (DecodeRtsAuthenticationFailureNotice(
                message.envelope.payload, failure)) {
            authenticationFailure_ = failure.reason;
            return;
        }
        RtsNetworkReject reject;
        if (DecodeRtsNetworkReject(message.envelope.payload, reject)) {
            lobby_.reject(reject);
        }
    }

    void processWelcome(const RtsReceivedNetworkMessage& message) {
        RtsAuthenticatedWelcome authenticated;
        if (DecodeRtsAuthenticatedWelcome(
                message.envelope.payload, authenticated)) {
            if (!config_.authentication ||
                !config_.authentication->acceptPrincipal(
                    authenticated.principal) ||
                !lobby_.acceptWelcome(authenticated.welcome)) {
                authenticationFailure_ =
                    RtsAuthenticationFailureReason::InvalidResponse;
                return;
            }
            principal_ = authenticated.principal;
            authenticated_ = true;
            auto* provider = config_.authentication->packetSecurityProvider();
            if (provider && principal_.securityKeyId != 0) {
                (void)network_.enableSecurity(
                    hostEndpoint_,
                    {provider,
                     principal_.securityKeyId,
                     principal_.securityEpoch,
                     true,
                     RtsGameSessionArchive::kMaximumNestedBytes + 1024u});
            } else if (
                config_.multiplayer.requirePacketSecurityAfterAuthentication) {
                authenticationFailure_ =
                    RtsAuthenticationFailureReason::ServiceUnavailable;
            }
            return;
        }
        RtsNetworkWelcome welcome;
        if (DecodeRtsNetworkWelcome(message.envelope.payload, welcome)) {
            (void)lobby_.acceptWelcome(welcome);
        }
    }

    void sendPing(std::uint64_t nowMs) {
        if (!quality_.pingDue(hostEndpoint_, nowMs)) return;
        const auto pingId = quality_.beginPing(hostEndpoint_, nowMs);
        if (pingId != 0) {
            (void)network_.send(
                hostEndpoint_,
                {RtsNetworkMessageKind::Ping,
                 lobby_.welcome().sessionId,
                 pingId,
                 {}},
                RtsNetworkDelivery::Unreliable);
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
    RtsOnlineClientRuntimeConfig config_;
    RtsConnectionQualityTable quality_;
    RtsAuthenticatedPrincipal principal_;
    RtsStartNotice startNotice_;
    std::vector<std::uint8_t> pendingReconnect_;
    std::unique_ptr<RtsLockstepSession> lockstep_;
    std::uint64_t nextRequestId_{1};
    RtsAuthenticationFailureReason authenticationFailure_{
        RtsAuthenticationFailureReason::Rejected};
    bool authenticated_{};
    bool restored_{};
};

} // namespace rts::gameplay
