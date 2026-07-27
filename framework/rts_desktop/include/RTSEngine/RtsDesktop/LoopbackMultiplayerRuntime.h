#pragma once

#include <RTSEngine/Network/LoopbackTransport.h>
#include <RTSEngine/Rts/RtsMultiplayerRuntime.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace rts::rts_desktop {

struct LoopbackMultiplayerConfig final {
    std::int32_t mapWidth{32};
    std::int32_t mapHeight{18};
    network::NetworkEndpointId hostEndpoint{1};
    network::NetworkEndpointId clientEndpoint{2};
    gameplay::RtsNetworkContentIdentity contentIdentity{
        gameplay::kRtsNetworkProtocolVersion,
        0x525453454E47494Eull,
        1};
    gameplay::RtsLockstepConfig lockstep{
        1,
        2,
        0,
        4,
        32,
        4,
        4096};
    network::LoopbackNetworkConfig network;
};

class LoopbackMultiplayerRuntime final {
public:
    explicit LoopbackMultiplayerRuntime(
        LoopbackMultiplayerConfig config = {})
        : config_(sanitize(config)),
          hub_(config_.network),
          hostTransport_(hub_, config_.hostEndpoint),
          clientTransport_(hub_, config_.clientEndpoint),
          hostEndpoint_(hostTransport_, endpointConfig()),
          clientEndpoint_(clientTransport_, endpointConfig()),
          hostSession_(config_.mapWidth, config_.mapHeight),
          clientSession_(config_.mapWidth, config_.mapHeight),
          host_(
              hostSession_,
              hostEndpoint_,
              {config_.lockstep.sessionId,
               config_.contentIdentity,
               2,
               4},
              config_.lockstep,
              "Host"),
          client_(
              clientSession_,
              clientEndpoint_,
              config_.hostEndpoint,
              {config_.contentIdentity,
               sim::LockstepPeerRole::Player,
               "Client"}) {}

    gameplay::RtsGameSession& hostSession() noexcept { return hostSession_; }
    gameplay::RtsGameSession& clientSession() noexcept { return clientSession_; }
    const gameplay::RtsMultiplayerHost& host() const noexcept { return host_; }
    const gameplay::RtsMultiplayerClient& client() const noexcept { return client_; }

    bool connect() {
        if (connected_ || !host_.valid() || !hostTransport_.open() ||
            !clientTransport_.open() ||
            !host_.addRemoteEndpoint(config_.clientEndpoint) ||
            !client_.connect()) {
            return false;
        }
        if (!pumpUntil(
                [this]() {
                    return client_.lobby().state() ==
                           gameplay::RtsLobbyClientState::Joined;
                },
                2000u)) {
            return false;
        }
        connected_ = true;
        return true;
    }

    bool readyAndStart() {
        if (!connected_ || started_ || !client_.setReady(true) ||
            !pumpUntil([this]() { return host_.lobby().canStart(); }, 2000u) ||
            !host_.startMatch() ||
            !pumpUntil([this]() { return client_.started(); }, 10000u)) {
            return false;
        }
        started_ = true;
        return hashesMatch();
    }

    bool advanceTick(
        std::vector<gameplay::TickCommand> hostCommands = {},
        std::vector<gameplay::TickCommand> clientCommands = {}) {
        if (!started_ ||
            !host_.submitLocal(std::move(hostCommands)) ||
            !client_.submitLocal(std::move(clientCommands))) {
            return false;
        }

        for (std::uint32_t attempt = 0; attempt < 5000u; ++attempt) {
            pump(1u);
            const auto hostResult = host_.advanceOne();
            const auto clientResult = client_.advanceOne();
            const bool hostAdvanced =
                hostResult == gameplay::RtsLockstepAdvanceResult::Advanced ||
                hostResult ==
                    gameplay::RtsLockstepAdvanceResult::AdvancedAfterRollback;
            const bool clientAdvanced =
                clientResult == gameplay::RtsLockstepAdvanceResult::Advanced ||
                clientResult ==
                    gameplay::RtsLockstepAdvanceResult::AdvancedAfterRollback;
            if (hostAdvanced && clientAdvanced) {
                pump(1u);
                return hashesMatch();
            }
            if ((hostResult != gameplay::RtsLockstepAdvanceResult::WaitingForInput &&
                 !hostAdvanced) ||
                (clientResult != gameplay::RtsLockstepAdvanceResult::WaitingForInput &&
                 !clientAdvanced) ||
                hostAdvanced != clientAdvanced) {
                return false;
            }
        }
        return false;
    }

    void pump(std::uint64_t elapsedMs) {
        nowMs_ += elapsedMs;
        host_.update(nowMs_);
        client_.update(nowMs_);
        host_.update(nowMs_);
        client_.update(nowMs_);
    }

    bool hashesMatch() const {
        return gameplay::RtsGameSessionArchive::authoritativeHash(hostSession_) ==
               gameplay::RtsGameSessionArchive::authoritativeHash(clientSession_);
    }

    bool connected() const noexcept { return connected_; }
    bool started() const noexcept { return started_; }
    std::uint64_t nowMs() const noexcept { return nowMs_; }

private:
    static LoopbackMultiplayerConfig sanitize(
        LoopbackMultiplayerConfig value) noexcept {
        value.mapWidth = std::max<std::int32_t>(1, value.mapWidth);
        value.mapHeight = std::max<std::int32_t>(1, value.mapHeight);
        if (value.lockstep.sessionId == 0) value.lockstep.sessionId = 1;
        value.network.maximumPayloadBytes = std::max<std::size_t>(
            512u, value.network.maximumPayloadBytes);
        return value;
    }

    static gameplay::RtsNetworkEndpointConfig endpointConfig() noexcept {
        gameplay::RtsNetworkEndpointConfig value;
        value.fragmentPayloadBytes = 700u;
        value.reliability.resendIntervalMs = 10u;
        value.reliability.maximumAttempts = 100u;
        value.reliability.maximumInFlight = 512u;
        value.reliability.maximumMessageBytes = 900u;
        return value;
    }

    template<class Predicate>
    bool pumpUntil(Predicate&& predicate, std::uint32_t maximumSteps) {
        for (std::uint32_t step = 0; step < maximumSteps; ++step) {
            if (predicate()) return true;
            pump(1u);
        }
        return predicate();
    }

    LoopbackMultiplayerConfig config_;
    network::LoopbackNetworkHub hub_;
    network::LoopbackTransport hostTransport_;
    network::LoopbackTransport clientTransport_;
    gameplay::RtsNetworkEndpoint hostEndpoint_;
    gameplay::RtsNetworkEndpoint clientEndpoint_;
    gameplay::RtsGameSession hostSession_;
    gameplay::RtsGameSession clientSession_;
    gameplay::RtsMultiplayerHost host_;
    gameplay::RtsMultiplayerClient client_;
    std::uint64_t nowMs_{};
    bool connected_{};
    bool started_{};
};

} // namespace rts::rts_desktop
