#include <RTSEngine/Network/LoopbackTransport.h>
#include <RTSEngine/Rts/RtsDedicatedServer.h>
#include <RTSEngine/Rts/RtsOnlineMultiplayerRuntime.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace rts;

void requireImpl(bool value, int line) {
    if (!value) {
        std::cerr << "online hardening assertion failed at line " << line << '\n';
        std::abort();
    }
}

#define require(value) requireImpl((value), __LINE__)

class TestSecurityProvider final : public network::INetworkSecurityProvider {
public:
    std::size_t maximumTagBytes() const noexcept override { return 8u; }

    bool seal(
        const network::NetworkSecurityRequest& request,
        std::vector<std::uint8_t>& ciphertext,
        std::vector<std::uint8_t>& tag) override {
        if (!request.associatedData || !request.input) return false;
        ciphertext = *request.input;
        transform(request.keyId, request.nonce, ciphertext);
        tag = makeTag(request, ciphertext);
        return true;
    }

    bool open(
        const network::NetworkSecurityRequest& request,
        const std::vector<std::uint8_t>& tag,
        std::vector<std::uint8_t>& plaintext) override {
        if (!request.associatedData || !request.input ||
            tag != makeTag(request, *request.input)) {
            return false;
        }
        plaintext = *request.input;
        transform(request.keyId, request.nonce, plaintext);
        return true;
    }

private:
    static void transform(
        std::uint64_t keyId,
        std::uint64_t nonce,
        std::vector<std::uint8_t>& bytes) {
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const auto key = static_cast<std::uint8_t>(
                (keyId >> ((index % 8u) * 8u)) ^
                (nonce >> (((index + 3u) % 8u) * 8u)));
            bytes[index] ^= static_cast<std::uint8_t>(key + index * 29u);
        }
    }

    static std::vector<std::uint8_t> makeTag(
        const network::NetworkSecurityRequest& request,
        const std::vector<std::uint8_t>& input) {
        foundation::CanonicalHash hash;
        hash.WriteU64(request.keyId);
        hash.WriteU64(request.epoch);
        hash.WriteU64(request.nonce);
        for (const auto byte : *request.associatedData) hash.WriteU8(byte);
        for (const auto byte : input) hash.WriteU8(byte);
        const auto value = hash.Value();
        std::vector<std::uint8_t> tag(8u);
        for (unsigned shift = 0; shift < 64; shift += 8) {
            tag[shift / 8u] = static_cast<std::uint8_t>(value >> shift);
        }
        return tag;
    }
};

std::vector<std::uint8_t> proofFor(
    const gameplay::RtsAuthenticationChallenge& challenge,
    const std::string& account) {
    foundation::CanonicalHash hash;
    hash.WriteU64(challenge.challengeId);
    for (const auto byte : challenge.nonce) hash.WriteU8(byte);
    hash.WriteString(account);
    const auto value = hash.Value();
    std::vector<std::uint8_t> proof(8u);
    for (unsigned shift = 0; shift < 64; shift += 8) {
        proof[shift / 8u] = static_cast<std::uint8_t>(value >> shift);
    }
    return proof;
}

class TestAuthenticationService final
    : public gameplay::IRtsServerAuthenticationService {
public:
    explicit TestAuthenticationService(TestSecurityProvider& security)
        : security_(&security) {}

    bool issueChallenge(
        network::NetworkEndpointId endpoint,
        std::uint64_t nowMs,
        gameplay::RtsAuthenticationChallenge& challenge) override {
        challenge.challengeId = nextChallenge_++;
        challenge.issuedAtMs = nowMs;
        challenge.expiresAtMs = nowMs + 500u;
        challenge.nonce = {
            static_cast<std::uint8_t>(endpoint),
            static_cast<std::uint8_t>(challenge.challengeId),
            0xA5u,
            0x5Au};
        challenge.providerData = {7, 7, 7};
        return true;
    }

    gameplay::RtsAuthenticationVerifyResult verify(
        network::NetworkEndpointId,
        const gameplay::RtsAuthenticationChallenge& challenge,
        const gameplay::RtsAuthenticationResponse& response,
        std::uint64_t,
        gameplay::RtsAuthenticatedPrincipal& principal) override {
        if (response.accountId != "client-account" ||
            response.credential != std::vector<std::uint8_t>({1, 3, 3, 7}) ||
            response.proof != proofFor(challenge, response.accountId)) {
            return gameplay::RtsAuthenticationVerifyResult::Rejected;
        }
        principal.challengeId = challenge.challengeId;
        principal.principalId = 9001;
        principal.accountId = response.accountId;
        principal.displayName = "Authenticated Client";
        principal.permissions = gameplay::kRtsAuthPermissionPlayer |
            gameplay::kRtsAuthPermissionSpectator |
            gameplay::kRtsAuthPermissionHostCandidate;
        principal.securityKeyId = 0xBEEFu;
        principal.securityEpoch = 3;
        principal.securityContext = {9, 8, 7, 6};
        return gameplay::RtsAuthenticationVerifyResult::Accepted;
    }

    network::INetworkSecurityProvider* packetSecurityProvider() noexcept override {
        return security_;
    }

private:
    TestSecurityProvider* security_{};
    std::uint64_t nextChallenge_{1};
};

class TestClientAuthentication final
    : public gameplay::IRtsClientAuthenticationProvider {
public:
    explicit TestClientAuthentication(TestSecurityProvider& security)
        : security_(&security) {}

    bool makeResponse(
        const gameplay::RtsAuthenticationChallenge& challenge,
        gameplay::RtsAuthenticationResponse& response) override {
        response.challengeId = challenge.challengeId;
        response.accountId = "client-account";
        response.credential = {1, 3, 3, 7};
        response.proof = proofFor(challenge, response.accountId);
        return true;
    }

    bool acceptPrincipal(
        const gameplay::RtsAuthenticatedPrincipal& principal) override {
        accepted_ = principal.principalId == 9001 &&
            principal.accountId == "client-account" &&
            principal.securityKeyId == 0xBEEFu;
        return accepted_;
    }

    network::INetworkSecurityProvider* packetSecurityProvider() noexcept override {
        return security_;
    }

    bool accepted() const noexcept { return accepted_; }

private:
    TestSecurityProvider* security_{};
    bool accepted_{};
};

struct UnitPair final {
    ecs::Entity first{};
    ecs::Entity second{};
};

UnitPair configureSession(gameplay::RtsGameSession& session) {
    gameplay::CombatStats combat;
    combat.maximumHealth = 30;
    combat.weaponDamage = 1;
    combat.weaponRange = 2;
    UnitPair result;
    for (std::int32_t index = 0; index < 12; ++index) {
        const auto first = session.createUnit(
            {2 + index % 4, 2 + index / 4}, {1}, 1, combat, 8);
        const auto second = session.createUnit(
            {12 + index % 4, 2 + index / 4}, {1}, 2, combat, 8);
        require(first.valid() && second.valid());
        if (index == 0) result = {first, second};
    }
    return result;
}

gameplay::RtsNetworkEndpointConfig endpointConfig() {
    gameplay::RtsNetworkEndpointConfig config;
    config.fragmentPayloadBytes = 420u;
    config.maximumMessageBytes = 8u * 1024u * 1024u;
    config.reliability.resendIntervalMs = 5u;
    config.reliability.maximumAttempts = 100u;
    config.reliability.maximumInFlight = 64u;
    config.reliability.maximumMessageBytes = 600u;
    config.inboundTraffic.packets = {2000u, 2000u, 1000u};
    config.inboundTraffic.bytes = {4u * 1024u * 1024u,
                                   4u * 1024u * 1024u,
                                   1000u};
    config.outboundTraffic = config.inboundTraffic;
    return config;
}

gameplay::RtsLockstepConfig lockstepConfig(std::uint64_t sessionId) {
    gameplay::RtsLockstepConfig config;
    config.sessionId = sessionId;
    config.inputDelayTicks = 2;
    config.maximumPredictionTicks = 0;
    config.checkpointIntervalTicks = 2;
    config.checkpointCapacity = 32;
    config.hashExchangeIntervalTicks = 2;
    config.maximumCommandsPerFrame = 256;
    return config;
}

gameplay::RtsOnlineMultiplayerRuntimeConfig multiplayerConfig() {
    gameplay::RtsOnlineMultiplayerRuntimeConfig config;
    config.frameDelivery = gameplay::RtsNetworkDelivery::Unreliable;
    config.hashDelivery = gameplay::RtsNetworkDelivery::Unreliable;
    config.adaptiveDelivery = true;
    config.requirePacketSecurityAfterAuthentication = true;
    config.simulationTickMilliseconds = 20;
    config.quality.pingIntervalMs = 5;
    config.quality.pingTimeoutMs = 80;
    config.quality.poorRttMs = 30;
    config.quality.criticalRttMs = 80;
    return config;
}

template<class Predicate, class... Runtimes>
bool pumpUntil(
    network::LoopbackNetworkHub& hub,
    std::uint64_t& nowMs,
    Predicate&& predicate,
    Runtimes&... runtimes) {
    for (std::uint32_t step = 0; step < 20000u; ++step) {
        if (predicate()) return true;
        ++nowMs;
        hub.update(nowMs);
        (runtimes.update(nowMs), ...);
        (runtimes.update(nowMs), ...);
    }
    return predicate();
}

void testAuthenticatedEncryptedRuntimeAndMigration() {
    network::LoopbackNetworkConfig networkConfig;
    networkConfig.baseLatencyMs = 2;
    networkConfig.jitterMs = 2;
    networkConfig.reorderDelayMs = 3;
    networkConfig.lossBasisPoints = 500;
    networkConfig.duplicateBasisPoints = 500;
    networkConfig.maximumPayloadBytes = 700;
    networkConfig.randomSeed = 1234567;
    network::LoopbackNetworkHub hub(networkConfig);
    network::LoopbackTransport hostTransport(hub, 1);
    network::LoopbackTransport clientTransport(hub, 2);
    gameplay::RtsNetworkEndpoint hostEndpoint(hostTransport, endpointConfig());
    gameplay::RtsNetworkEndpoint clientEndpoint(clientTransport, endpointConfig());

    TestSecurityProvider security;
    TestAuthenticationService authService(security);
    TestClientAuthentication clientAuth(security);
    gameplay::RtsGameSession hostSession(20, 12);
    gameplay::RtsGameSession clientSession(20, 12);
    const auto hostUnits = configureSession(hostSession);
    const auto clientUnits = configureSession(clientSession);
    require(hostUnits.first == clientUnits.first &&
            hostUnits.second == clientUnits.second);

    const gameplay::RtsNetworkContentIdentity identity{
        gameplay::kRtsNetworkProtocolVersion, 101, 202};
    gameplay::RtsLobbyHostConfig lobbyConfig{777, identity, 2, 2};
    gameplay::RtsOnlineHostRuntimeConfig hostConfig;
    hostConfig.multiplayer = multiplayerConfig();
    hostConfig.authentication.service = &authService;
    hostConfig.authentication.required = true;
    hostConfig.allowHostMigration = true;
    hostConfig.securityEpoch = 3;
    gameplay::RtsOnlineClientRuntimeConfig clientConfig;
    clientConfig.multiplayer = multiplayerConfig();
    clientConfig.authentication = &clientAuth;

    gameplay::RtsOnlineMultiplayerHost host(
        hostSession,
        hostEndpoint,
        lobbyConfig,
        lockstepConfig(777),
        "Host",
        hostConfig);
    gameplay::RtsOnlineMultiplayerClient client(
        clientSession,
        clientEndpoint,
        1,
        {identity, sim::LockstepPeerRole::Player, "Untrusted Name"},
        clientConfig);
    require(host.valid());
    require(host.addRemoteEndpoint(2));
    require(client.connect());

    std::uint64_t nowMs = 1;
    require(pumpUntil(
        hub,
        nowMs,
        [&]() {
            return client.lobby().state() ==
                   gameplay::RtsLobbyClientState::Joined;
        },
        host,
        client));
    require(clientAuth.accepted());
    require(hostEndpoint.securityActive(2));
    require(clientEndpoint.securityActive(1));
    require(host.principal(2) && host.principal(2)->principalId == 9001);
    require(client.principal() && client.principal()->principalId == 9001);

    require(client.setReady(true));
    require(pumpUntil(
        hub, nowMs, [&]() { return host.lobby().canStart(); }, host, client));
    require(host.startMatch());
    require(pumpUntil(
        hub, nowMs, [&]() { return client.started(); }, host, client));
    require(gameplay::RtsGameSessionArchive::authoritativeHash(hostSession) ==
            gameplay::RtsGameSessionArchive::authoritativeHash(clientSession));

    for (std::uint64_t tick = 0; tick < 8; ++tick) {
        std::vector<gameplay::TickCommand> hostCommands;
        std::vector<gameplay::TickCommand> clientCommands;
        if (tick == 0) {
            gameplay::TickCommand first;
            first.type = gameplay::CommandType::Move;
            first.subject = hostUnits.first;
            first.targetX = 8;
            first.targetY = 2;
            hostCommands.push_back(first);

            gameplay::TickCommand second;
            second.type = gameplay::CommandType::Move;
            second.subject = hostUnits.second;
            second.targetX = 10;
            second.targetY = 7;
            clientCommands.push_back(second);
        }
        require(host.submitLocal(std::move(hostCommands)));
        require(client.submitLocal(std::move(clientCommands)));
        const auto targetNextTick = tick + 1u;
        require(pumpUntil(
            hub,
            nowMs,
            [&]() {
                auto hostTick = host.lockstep()
                    ? host.lockstep()->coordinator().simulatedThrough()
                    : 0;
                auto clientTick = client.lockstep()
                    ? client.lockstep()->coordinator().simulatedThrough()
                    : 0;
                if (hostTick < targetNextTick) (void)host.advanceOne();
                if (clientTick < targetNextTick) (void)client.advanceOne();
                hostTick = host.lockstep()
                    ? host.lockstep()->coordinator().simulatedThrough()
                    : 0;
                clientTick = client.lockstep()
                    ? client.lockstep()->coordinator().simulatedThrough()
                    : 0;
                return hostTick >= targetNextTick &&
                       clientTick >= targetNextTick;
            },
            host,
            client));
        require(gameplay::RtsGameSessionArchive::authoritativeHash(hostSession) ==
                gameplay::RtsGameSessionArchive::authoritativeHash(clientSession));
    }

    require(pumpUntil(
        hub,
        nowMs,
        [&]() {
            const auto* quality = host.connectionQuality(2);
            return quality && quality->pingsAcknowledged >= 2;
        },
        host,
        client));
    const auto* quality = host.connectionQuality(2);
    require(quality && quality->smoothedRttMs != 0);

    gameplay::RtsNetworkEndpointPeerStats peerStats;
    require(hostEndpoint.peerStats(2, peerStats));
    require(peerStats.securityActive);
    require(peerStats.security.protectedSent != 0);
    require(peerStats.security.protectedReceived != 0);
    require(peerStats.inboundTraffic.acceptedPackets != 0);

    require(host.setHostCandidate(2));
    gameplay::RtsHostMigrationPackage migration;
    require(host.makeHostMigrationPackage(migration));
    const auto migrationBytes =
        gameplay::EncodeRtsHostMigrationPackage(migration);
    gameplay::RtsHostMigrationPackage decodedMigration;
    require(!migrationBytes.empty() &&
            gameplay::DecodeRtsHostMigrationPackage(
                migrationBytes, decodedMigration));
    require(decodedMigration.newHostEndpoint == 2);
    require(decodedMigration.securityEpoch > 3);

    gameplay::RtsGameSession migratedSession(20, 12);
    gameplay::RtsOnlineMultiplayerHost migratedHost(
        migratedSession,
        clientEndpoint,
        lobbyConfig,
        decodedMigration,
        hostConfig);
    require(migratedHost.valid() && migratedHost.started());
    require(gameplay::RtsGameSessionArchive::authoritativeHash(migratedSession) ==
            gameplay::RtsGameSessionArchive::authoritativeHash(hostSession));
}

void testDedicatedServerHasNoLocalPlayer() {
    network::LoopbackNetworkConfig networkConfig;
    networkConfig.baseLatencyMs = 1;
    networkConfig.jitterMs = 1;
    networkConfig.maximumPayloadBytes = 900;
    networkConfig.randomSeed = 424242;
    network::LoopbackNetworkHub hub(networkConfig);
    network::LoopbackTransport serverTransport(hub, 10);
    network::LoopbackTransport firstTransport(hub, 11);
    network::LoopbackTransport secondTransport(hub, 12);
    gameplay::RtsNetworkEndpoint serverEndpoint(serverTransport, endpointConfig());
    gameplay::RtsNetworkEndpoint firstEndpoint(firstTransport, endpointConfig());
    gameplay::RtsNetworkEndpoint secondEndpoint(secondTransport, endpointConfig());

    gameplay::RtsGameSession serverSession(16, 10);
    gameplay::RtsGameSession firstSession(16, 10);
    gameplay::RtsGameSession secondSession(16, 10);
    const auto serverUnits = configureSession(serverSession);
    const auto firstUnits = configureSession(firstSession);
    const auto secondUnits = configureSession(secondSession);
    require(serverUnits.first == firstUnits.first &&
            serverUnits.first == secondUnits.first);

    const gameplay::RtsNetworkContentIdentity identity{
        gameplay::kRtsNetworkProtocolVersion, 303, 404};
    gameplay::RtsLobbyHostConfig lobbyConfig{888, identity, 2, 1};
    gameplay::RtsOnlineHostRuntimeConfig hostConfig;
    hostConfig.multiplayer.adaptiveDelivery = false;
    hostConfig.multiplayer.requirePacketSecurityAfterAuthentication = false;
    gameplay::RtsDedicatedServerRuntime server(
        serverSession,
        serverEndpoint,
        lobbyConfig,
        lockstepConfig(888),
        hostConfig);
    gameplay::RtsOnlineClientRuntimeConfig clientConfig;
    clientConfig.multiplayer.adaptiveDelivery = false;
    clientConfig.multiplayer.requirePacketSecurityAfterAuthentication = false;
    gameplay::RtsOnlineMultiplayerClient first(
        firstSession,
        firstEndpoint,
        10,
        {identity, sim::LockstepPeerRole::Player, "First"},
        clientConfig);
    gameplay::RtsOnlineMultiplayerClient second(
        secondSession,
        secondEndpoint,
        10,
        {identity, sim::LockstepPeerRole::Player, "Second"},
        clientConfig);

    require(server.addRemoteEndpoint(11));
    require(server.addRemoteEndpoint(12));
    require(first.connect() && second.connect());
    std::uint64_t nowMs = 1;
    require(pumpUntil(
        hub,
        nowMs,
        [&]() {
            return first.lobby().state() == gameplay::RtsLobbyClientState::Joined &&
                   second.lobby().state() == gameplay::RtsLobbyClientState::Joined;
        },
        server,
        first,
        second));
    require(server.host().lobby().members().size() == 2);
    require(server.host().lobby().managementEndpoint() == 10);
    require(first.setReady(true) && second.setReady(true));
    require(pumpUntil(
        hub,
        nowMs,
        [&]() { return server.host().lobby().canStart(); },
        server,
        first,
        second));
    require(server.startMatch());
    require(pumpUntil(
        hub,
        nowMs,
        [&]() { return first.started() && second.started(); },
        server,
        first,
        second));
    require(server.host().lockstep()->coordinator().peers().size() == 2);
    require(!server.host().submitLocal({}));

    for (std::uint64_t tick = 0; tick < 4; ++tick) {
        require(first.submitLocal({}));
        require(second.submitLocal({}));
        const auto targetNextTick = tick + 1u;
        require(pumpUntil(
            hub,
            nowMs,
            [&]() {
                auto serverTick =
                    server.host().lockstep()->coordinator().simulatedThrough();
                auto firstTick =
                    first.lockstep()->coordinator().simulatedThrough();
                auto secondTick =
                    second.lockstep()->coordinator().simulatedThrough();
                if (serverTick < targetNextTick) (void)server.advanceOne();
                if (firstTick < targetNextTick) (void)first.advanceOne();
                if (secondTick < targetNextTick) (void)second.advanceOne();
                serverTick =
                    server.host().lockstep()->coordinator().simulatedThrough();
                firstTick = first.lockstep()->coordinator().simulatedThrough();
                secondTick = second.lockstep()->coordinator().simulatedThrough();
                return serverTick >= targetNextTick &&
                       firstTick >= targetNextTick &&
                       secondTick >= targetNextTick;
            },
            server,
            first,
            second));
        const auto serverHash =
            gameplay::RtsGameSessionArchive::authoritativeHash(serverSession);
        require(serverHash ==
                gameplay::RtsGameSessionArchive::authoritativeHash(firstSession));
        require(serverHash ==
                gameplay::RtsGameSessionArchive::authoritativeHash(secondSession));
    }
    require(server.health().players == 2);
    require(server.health().simulationTicks == 4);
}

#undef require

} // namespace

int main() {
    testAuthenticatedEncryptedRuntimeAndMigration();
    testDedicatedServerHasNoLocalPlayer();
    std::cout << "RTS online hardening tests passed\n";
    return EXIT_SUCCESS;
}
