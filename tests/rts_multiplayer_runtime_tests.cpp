#include <RTSEngine/Rts/RtsLobby.h>
#include <RTSEngine/RtsDesktop/LoopbackMultiplayerRuntime.h>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace rts;

void require(bool condition) {
    if (!condition) std::abort();
}

void testProtocolCompatibilityAndLobby() {
    gameplay::RtsNetworkContentIdentity identity{
        gameplay::kRtsNetworkProtocolVersion, 11, 22};
    gameplay::RtsLobbyHost lobby({99, identity, 2, 2});
    require(lobby.registerHost(1, "Host"));

    gameplay::RtsNetworkHello incompatible{
        {gameplay::kRtsNetworkProtocolVersion, 11, 23},
        sim::LockstepPeerRole::Player,
        "Wrong Content"};
    gameplay::RtsNetworkWelcome welcome;
    gameplay::RtsNetworkReject reject;
    require(lobby.join(2, incompatible, welcome, reject) ==
            gameplay::RtsLobbyJoinResult::Rejected);
    require(reject.reason == gameplay::RtsNetworkRejectReason::ContentMismatch);

    gameplay::RtsNetworkHello compatible{
        identity,
        sim::LockstepPeerRole::Player,
        "Client"};
    require(lobby.join(2, compatible, welcome, reject) ==
            gameplay::RtsLobbyJoinResult::Accepted);
    require(welcome.sessionId == 99);
    require(welcome.peer.playerSlot == 2 && welcome.peer.issuer == 2);
    require(lobby.setReady(2, welcome.peer.peerId, true));
    require(lobby.canStart());

    const auto encoded = gameplay::EncodeRtsLobbySnapshot(lobby.snapshot());
    gameplay::RtsLobbySnapshot decoded;
    require(!encoded.empty() && gameplay::DecodeRtsLobbySnapshot(encoded, decoded));
    require(decoded.members.size() == 2);

    gameplay::RtsStartNotice notice;
    notice.lockstep.sessionId = 99;
    notice.lockstep.inputDelayTicks = 2;
    notice.lockstep.checkpointIntervalTicks = 4;
    notice.lockstep.checkpointCapacity = 16;
    notice.lockstep.hashExchangeIntervalTicks = 4;
    notice.lockstep.maximumCommandsPerFrame = 256;
    notice.peers = lobby.lockstepPeers();
    const auto startBytes = gameplay::EncodeRtsStartNotice(notice);
    gameplay::RtsStartNotice restored;
    require(gameplay::DecodeRtsStartNotice(startBytes, restored));
    require(restored.peers.size() == 2 && restored.lockstep.sessionId == 99);
}

void testPlayableLoopbackMultiplayer() {
    rts_desktop::LoopbackMultiplayerConfig config;
    config.lockstep.sessionId = 12345;
    config.lockstep.inputDelayTicks = 2;
    config.lockstep.checkpointIntervalTicks = 2;
    config.lockstep.hashExchangeIntervalTicks = 2;
    config.network.baseLatencyMs = 2;
    config.network.jitterMs = 3;
    config.network.reorderDelayMs = 4;
    config.network.lossBasisPoints = 1000;
    config.network.duplicateBasisPoints = 1000;
    config.network.randomSeed = 998877;

    rts_desktop::LoopbackMultiplayerRuntime runtime(config);
    gameplay::CombatStats combat;
    combat.maximumHealth = 20;
    combat.weaponDamage = 2;
    combat.weaponRange = 2;

    ecs::Entity hostUnit{};
    ecs::Entity clientUnit{};
    for (std::int32_t index = 0; index < 20; ++index) {
        const auto firstHost = runtime.hostSession().createUnit(
            {2 + index % 5, 2 + index / 5}, {1}, 1, combat, 8);
        const auto firstClient = runtime.clientSession().createUnit(
            {2 + index % 5, 2 + index / 5}, {1}, 1, combat, 8);
        const auto secondHost = runtime.hostSession().createUnit(
            {12 + index % 5, 2 + index / 5}, {1}, 2, combat, 8);
        const auto secondClient = runtime.clientSession().createUnit(
            {12 + index % 5, 2 + index / 5}, {1}, 2, combat, 8);
        require(firstHost == firstClient && secondHost == secondClient);
        if (index == 0) {
            hostUnit = firstHost;
            clientUnit = secondHost;
        }
    }

    require(runtime.connect());
    require(runtime.readyAndStart());
    require(runtime.hashesMatch());

    for (std::uint64_t tick = 0; tick < 16; ++tick) {
        std::vector<gameplay::TickCommand> hostCommands;
        std::vector<gameplay::TickCommand> clientCommands;
        if (tick == 0) {
            gameplay::TickCommand hostMove;
            hostMove.type = gameplay::CommandType::Move;
            hostMove.subject = hostUnit;
            hostMove.targetX = 9;
            hostMove.targetY = 2;
            hostCommands.push_back(hostMove);

            gameplay::TickCommand clientMove;
            clientMove.type = gameplay::CommandType::Move;
            clientMove.subject = clientUnit;
            clientMove.targetX = 9;
            clientMove.targetY = 6;
            clientCommands.push_back(clientMove);
        }
        require(runtime.advanceTick(
            std::move(hostCommands), std::move(clientCommands)));
        require(runtime.hashesMatch());
    }

    const auto* hostPosition =
        runtime.hostSession().simulation().world().try_get<gameplay::Position>(
            hostUnit);
    const auto* clientPosition =
        runtime.clientSession().simulation().world().try_get<gameplay::Position>(
            hostUnit);
    require(hostPosition && clientPosition);
    require(hostPosition->x == clientPosition->x &&
            hostPosition->y == clientPosition->y);

    const auto* hostSecond =
        runtime.hostSession().simulation().world().try_get<gameplay::Position>(
            clientUnit);
    const auto* clientSecond =
        runtime.clientSession().simulation().world().try_get<gameplay::Position>(
            clientUnit);
    require(hostSecond && clientSecond);
    require(hostSecond->x == clientSecond->x &&
            hostSecond->y == clientSecond->y);
}

} // namespace

int main() {
    testProtocolCompatibilityAndLobby();
    testPlayableLoopbackMultiplayer();
    std::cout << "RTS multiplayer runtime tests passed\n";
    return EXIT_SUCCESS;
}
