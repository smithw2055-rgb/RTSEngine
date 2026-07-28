#pragma once

#include <RTSEngine/Rts/RtsOnlineMultiplayerRuntime.h>

#include <cstdint>
#include <string>
#include <utility>

namespace rts::gameplay {

struct RtsDedicatedServerHealth final {
    bool valid{};
    bool started{};
    std::uint64_t startedAtMs{};
    std::uint64_t lastUpdateAtMs{};
    std::uint64_t simulationTicks{};
    std::uint32_t players{};
    std::uint32_t spectators{};
    std::uint32_t pendingAuthentication{};
};

class RtsDedicatedServerRuntime final {
public:
    RtsDedicatedServerRuntime(
        RtsGameSession& session,
        RtsNetworkEndpoint& network,
        RtsLobbyHostConfig lobbyConfig,
        RtsLockstepConfig lockstepConfig,
        RtsOnlineHostRuntimeConfig config = {})
        : host_(
            session,
            network,
            makeDedicated(std::move(lobbyConfig)),
            lockstepConfig,
            "DedicatedServer",
            std::move(config)) {
        health_.valid = host_.valid();
    }

    bool addRemoteEndpoint(network::NetworkEndpointId endpoint) {
        return host_.addRemoteEndpoint(endpoint);
    }

    void update(std::uint64_t nowMs) {
        if (health_.startedAtMs == 0) health_.startedAtMs = nowMs;
        health_.lastUpdateAtMs = nowMs;
        host_.update(nowMs);
        refreshHealth();
    }

    bool startMatch() {
        const bool started = host_.startMatch();
        refreshHealth();
        return started;
    }

    RtsLockstepAdvanceResult advanceOne() {
        const auto result = host_.advanceOne();
        if (result == RtsLockstepAdvanceResult::Advanced ||
            result == RtsLockstepAdvanceResult::AdvancedAfterRollback) {
            ++health_.simulationTicks;
        }
        refreshHealth();
        return result;
    }

    const RtsOnlineMultiplayerHost& host() const noexcept { return host_; }
    RtsOnlineMultiplayerHost& host() noexcept { return host_; }
    const RtsDedicatedServerHealth& health() const noexcept { return health_; }

private:
    static RtsLobbyHostConfig makeDedicated(
        RtsLobbyHostConfig config) noexcept {
        config.dedicatedServer = true;
        return config;
    }

    void refreshHealth() noexcept {
        health_.valid = host_.valid();
        health_.started = host_.started();
        health_.players = 0;
        health_.spectators = 0;
        for (const auto& member : host_.lobby().members()) {
            if (!member.peer.active) continue;
            if (member.peer.role == sim::LockstepPeerRole::Player) {
                ++health_.players;
            } else {
                ++health_.spectators;
            }
        }
        health_.pendingAuthentication = static_cast<std::uint32_t>(
            host_.authentication().pendingCount());
    }

    RtsOnlineMultiplayerHost host_;
    RtsDedicatedServerHealth health_{};
};

} // namespace rts::gameplay
