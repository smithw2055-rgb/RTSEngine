#pragma once

#include <RTSEngine/Network/Security.h>
#include <RTSEngine/Network/Transport.h>
#include <RTSEngine/Rts/RtsLockstepSession.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rts::gameplay {

using RtsPrincipalId = std::uint64_t;
using RtsAuthenticationChallengeId = std::uint64_t;
using RtsOnlineRequestId = std::uint64_t;

inline constexpr std::uint32_t kRtsAuthPermissionPlayer = 1u << 0u;
inline constexpr std::uint32_t kRtsAuthPermissionSpectator = 1u << 1u;
inline constexpr std::uint32_t kRtsAuthPermissionHostCandidate = 1u << 2u;
inline constexpr std::uint32_t kRtsAuthPermissionDedicatedAdmin = 1u << 3u;

struct RtsAuthenticationChallenge final {
    RtsAuthenticationChallengeId challengeId{};
    std::uint64_t issuedAtMs{};
    std::uint64_t expiresAtMs{};
    std::vector<std::uint8_t> nonce;
    std::vector<std::uint8_t> providerData;
};

struct RtsAuthenticationResponse final {
    RtsAuthenticationChallengeId challengeId{};
    std::string accountId;
    std::vector<std::uint8_t> credential;
    std::vector<std::uint8_t> proof;
};

struct RtsAuthenticatedPrincipal final {
    RtsAuthenticationChallengeId challengeId{};
    RtsPrincipalId principalId{};
    std::string accountId;
    std::string displayName;
    std::uint32_t permissions{};
    std::uint64_t securityKeyId{};
    std::uint64_t securityEpoch{1};
    std::vector<std::uint8_t> securityContext;

    bool allows(sim::LockstepPeerRole role) const noexcept {
        return role == sim::LockstepPeerRole::Player
            ? (permissions & kRtsAuthPermissionPlayer) != 0
            : (permissions & kRtsAuthPermissionSpectator) != 0;
    }

    bool hostCandidate() const noexcept {
        return (permissions & kRtsAuthPermissionHostCandidate) != 0;
    }
};

enum class RtsAuthenticationVerifyResult : std::uint8_t {
    Accepted,
    Rejected,
    Expired,
    Replay,
    InvalidResponse
};

class IRtsServerAuthenticationService {
public:
    virtual ~IRtsServerAuthenticationService() = default;

    virtual bool issueChallenge(
        network::NetworkEndpointId endpoint,
        std::uint64_t nowMs,
        RtsAuthenticationChallenge& challenge) = 0;

    virtual RtsAuthenticationVerifyResult verify(
        network::NetworkEndpointId endpoint,
        const RtsAuthenticationChallenge& challenge,
        const RtsAuthenticationResponse& response,
        std::uint64_t nowMs,
        RtsAuthenticatedPrincipal& principal) = 0;

    virtual network::INetworkSecurityProvider* packetSecurityProvider() noexcept {
        return nullptr;
    }
};

class IRtsClientAuthenticationProvider {
public:
    virtual ~IRtsClientAuthenticationProvider() = default;

    virtual bool makeResponse(
        const RtsAuthenticationChallenge& challenge,
        RtsAuthenticationResponse& response) = 0;

    virtual bool acceptPrincipal(
        const RtsAuthenticatedPrincipal& principal) = 0;

    virtual network::INetworkSecurityProvider* packetSecurityProvider() noexcept {
        return nullptr;
    }
};

struct RtsOnlineSessionDescriptor final {
    std::uint64_t sessionId{};
    std::string region;
    std::string address;
    std::uint16_t port{};
    std::uint32_t players{};
    std::uint32_t maximumPlayers{};
    std::uint64_t buildHash{};
    std::uint64_t contentHash{};
    bool dedicated{};
    bool passwordProtected{};
};

class IRtsSessionDirectoryService {
public:
    virtual ~IRtsSessionDirectoryService() = default;

    virtual bool publish(const RtsOnlineSessionDescriptor& session) = 0;
    virtual bool remove(std::uint64_t sessionId) = 0;
    virtual std::vector<RtsOnlineSessionDescriptor> query(
        std::uint64_t buildHash,
        std::uint64_t contentHash,
        const std::string& region) = 0;
};

struct RtsMatchmakingRequest final {
    RtsOnlineRequestId requestId{};
    RtsPrincipalId principalId{};
    std::string region;
    std::uint64_t buildHash{};
    std::uint64_t contentHash{};
    sim::LockstepPeerRole role{sim::LockstepPeerRole::Player};
};

struct RtsMatchmakingResult final {
    RtsOnlineRequestId requestId{};
    bool matched{};
    RtsOnlineSessionDescriptor session;
    std::vector<std::uint8_t> joinTicket;
};

class IRtsMatchmakingService {
public:
    virtual ~IRtsMatchmakingService() = default;

    virtual bool enqueue(const RtsMatchmakingRequest& request) = 0;
    virtual bool cancel(RtsOnlineRequestId requestId) = 0;
    virtual bool poll(RtsOnlineRequestId requestId, RtsMatchmakingResult& result) = 0;
};

struct RtsRelayAllocation final {
    RtsOnlineRequestId requestId{};
    std::string relayAddress;
    std::uint16_t relayPort{};
    std::vector<std::uint8_t> allocationToken;
    std::uint64_t expiresAtMs{};
};

class IRtsRelayService {
public:
    virtual ~IRtsRelayService() = default;

    virtual bool allocate(
        RtsOnlineRequestId requestId,
        std::uint64_t sessionId,
        RtsPrincipalId principalId,
        RtsRelayAllocation& allocation) = 0;

    virtual bool release(RtsOnlineRequestId requestId) = 0;
};

} // namespace rts::gameplay
