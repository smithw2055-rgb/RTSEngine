#pragma once

#include <RTSEngine/Network/TrafficControl.h>
#include <RTSEngine/Rts/RtsAuthenticationProtocol.h>
#include <RTSEngine/Rts/RtsOnlineServices.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts::gameplay {

struct RtsAuthenticationRuntimeConfig final {
    IRtsServerAuthenticationService* service{};
    bool required{};
    std::size_t maximumPendingChallenges{1024u};
    std::size_t maximumRememberedChallenges{4096u};
    network::TokenBucketConfig attemptsPerEndpoint{5u, 5u, 10000u};
};

enum class RtsAuthenticationBeginResult : std::uint8_t {
    Bypassed,
    ChallengeIssued,
    RateLimited,
    ServiceUnavailable,
    InvalidRequest
};

class RtsServerAuthenticationRuntime final {
public:
    explicit RtsServerAuthenticationRuntime(
        RtsAuthenticationRuntimeConfig config = {}) noexcept
        : config_(sanitize(config)) {}

    bool required() const noexcept { return config_.required; }

    IRtsServerAuthenticationService* service() const noexcept {
        return config_.service;
    }

    RtsAuthenticationBeginResult begin(
        network::NetworkEndpointId endpoint,
        const RtsNetworkHello& hello,
        std::uint64_t nowMs,
        RtsAuthenticationChallengeNotice& notice) {
        expire(nowMs);
        if (!config_.required) return RtsAuthenticationBeginResult::Bypassed;
        if (!config_.service) {
            return RtsAuthenticationBeginResult::ServiceUnavailable;
        }
        if (endpoint == 0 || hello.displayName.empty()) {
            return RtsAuthenticationBeginResult::InvalidRequest;
        }
        auto& attempts = attemptState(endpoint);
        if (!attempts.bucket.consume(nowMs, 1u)) {
            return RtsAuthenticationBeginResult::RateLimited;
        }

        const auto existing = lowerPending(endpoint);
        if (existing != pending_.end() && existing->endpoint == endpoint) {
            notice.challenge = existing->challenge;
            return RtsAuthenticationBeginResult::ChallengeIssued;
        }
        if (pending_.size() >= config_.maximumPendingChallenges) {
            return RtsAuthenticationBeginResult::RateLimited;
        }

        RtsAuthenticationChallenge challenge;
        if (!config_.service->issueChallenge(endpoint, nowMs, challenge) ||
            challenge.challengeId == 0 ||
            challenge.issuedAtMs > nowMs ||
            challenge.expiresAtMs <= nowMs || challenge.nonce.empty() ||
            remembered(challenge.challengeId)) {
            return RtsAuthenticationBeginResult::ServiceUnavailable;
        }
        pending_.insert(
            existing,
            PendingChallenge{endpoint, hello, challenge});
        notice.challenge = std::move(challenge);
        return RtsAuthenticationBeginResult::ChallengeIssued;
    }

    RtsAuthenticationVerifyResult verify(
        network::NetworkEndpointId endpoint,
        const RtsAuthenticatedHello& authenticatedHello,
        std::uint64_t nowMs,
        RtsAuthenticatedPrincipal& principal) {
        expire(nowMs);
        if (!config_.required || !config_.service || endpoint == 0) {
            return RtsAuthenticationVerifyResult::InvalidResponse;
        }
        auto& attempts = attemptState(endpoint);
        if (!attempts.bucket.consume(nowMs, 1u)) {
            return RtsAuthenticationVerifyResult::Rejected;
        }

        const auto found = lowerPending(endpoint);
        if (found == pending_.end() || found->endpoint != endpoint) {
            return remembered(authenticatedHello.response.challengeId)
                ? RtsAuthenticationVerifyResult::Replay
                : RtsAuthenticationVerifyResult::InvalidResponse;
        }
        const auto challenge = found->challenge;
        const auto originalHello = found->hello;
        pending_.erase(found);
        remember(challenge.challengeId);

        if (authenticatedHello.response.challengeId != challenge.challengeId ||
            !sameHello(originalHello, authenticatedHello.hello)) {
            return RtsAuthenticationVerifyResult::InvalidResponse;
        }
        if (nowMs >= challenge.expiresAtMs) {
            return RtsAuthenticationVerifyResult::Expired;
        }
        const auto result = config_.service->verify(
            endpoint,
            challenge,
            authenticatedHello.response,
            nowMs,
            principal);
        if (result != RtsAuthenticationVerifyResult::Accepted) return result;
        if (principal.challengeId != challenge.challengeId ||
            principal.principalId == 0 || principal.accountId.empty() ||
            principal.displayName.empty() || principal.permissions == 0 ||
            !principal.allows(originalHello.requestedRole) ||
            principal.securityEpoch == 0) {
            return RtsAuthenticationVerifyResult::InvalidResponse;
        }
        return RtsAuthenticationVerifyResult::Accepted;
    }

    void expire(std::uint64_t nowMs) {
        for (auto iterator = pending_.begin(); iterator != pending_.end();) {
            if (nowMs >= iterator->challenge.expiresAtMs) {
                remember(iterator->challenge.challengeId);
                iterator = pending_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    std::size_t pendingCount() const noexcept { return pending_.size(); }

private:
    struct PendingChallenge final {
        network::NetworkEndpointId endpoint{};
        RtsNetworkHello hello;
        RtsAuthenticationChallenge challenge;
    };

    struct AttemptState final {
        network::NetworkEndpointId endpoint{};
        network::TokenBucket bucket;
    };

    using PendingIterator = std::vector<PendingChallenge>::iterator;
    using AttemptIterator = std::vector<AttemptState>::iterator;

    static RtsAuthenticationRuntimeConfig sanitize(
        RtsAuthenticationRuntimeConfig value) noexcept {
        value.maximumPendingChallenges = std::clamp<std::size_t>(
            value.maximumPendingChallenges, 1u, 65536u);
        value.maximumRememberedChallenges = std::clamp<std::size_t>(
            value.maximumRememberedChallenges, 1u, 1000000u);
        return value;
    }

    static bool sameHello(
        const RtsNetworkHello& first,
        const RtsNetworkHello& second) noexcept {
        return first.identity == second.identity &&
               first.requestedRole == second.requestedRole &&
               first.displayName == second.displayName;
    }

    PendingIterator lowerPending(network::NetworkEndpointId endpoint) noexcept {
        return std::lower_bound(
            pending_.begin(), pending_.end(), endpoint,
            [](const PendingChallenge& value, network::NetworkEndpointId id) {
                return value.endpoint < id;
            });
    }

    AttemptIterator lowerAttempt(network::NetworkEndpointId endpoint) noexcept {
        return std::lower_bound(
            attempts_.begin(), attempts_.end(), endpoint,
            [](const AttemptState& value, network::NetworkEndpointId id) {
                return value.endpoint < id;
            });
    }

    AttemptState& attemptState(network::NetworkEndpointId endpoint) {
        const auto found = lowerAttempt(endpoint);
        if (found != attempts_.end() && found->endpoint == endpoint) {
            return *found;
        }
        return *attempts_.insert(
            found,
            AttemptState{
                endpoint, network::TokenBucket(config_.attemptsPerEndpoint)});
    }

    bool remembered(RtsAuthenticationChallengeId id) const noexcept {
        return id != 0 && std::binary_search(
            remembered_.begin(), remembered_.end(), id);
    }

    void remember(RtsAuthenticationChallengeId id) {
        if (id == 0 || remembered(id)) return;
        const auto found = std::lower_bound(
            remembered_.begin(), remembered_.end(), id);
        remembered_.insert(found, id);
        if (remembered_.size() > config_.maximumRememberedChallenges) {
            remembered_.erase(remembered_.begin());
        }
    }

    RtsAuthenticationRuntimeConfig config_;
    std::vector<PendingChallenge> pending_;
    std::vector<AttemptState> attempts_;
    std::vector<RtsAuthenticationChallengeId> remembered_;
};

inline RtsAuthenticationFailureReason ToAuthenticationFailureReason(
    RtsAuthenticationVerifyResult result) noexcept {
    switch (result) {
    case RtsAuthenticationVerifyResult::Expired:
        return RtsAuthenticationFailureReason::Expired;
    case RtsAuthenticationVerifyResult::Replay:
        return RtsAuthenticationFailureReason::Replay;
    case RtsAuthenticationVerifyResult::InvalidResponse:
        return RtsAuthenticationFailureReason::InvalidResponse;
    case RtsAuthenticationVerifyResult::Rejected:
        return RtsAuthenticationFailureReason::Rejected;
    case RtsAuthenticationVerifyResult::Accepted:
        return RtsAuthenticationFailureReason::InvalidResponse;
    }
    return RtsAuthenticationFailureReason::Rejected;
}

} // namespace rts::gameplay
