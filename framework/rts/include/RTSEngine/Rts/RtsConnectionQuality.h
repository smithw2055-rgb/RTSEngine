#pragma once

#include <RTSEngine/Network/ConnectionQuality.h>
#include <RTSEngine/Network/Transport.h>
#include <RTSEngine/Rts/RtsNetworkEndpoint.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::gameplay {

struct RtsPeerConnectionQuality final {
    network::NetworkEndpointId endpoint{};
    network::ConnectionQualityTracker tracker;
};

class RtsConnectionQualityTable final {
public:
    explicit RtsConnectionQualityTable(
        network::ConnectionQualityConfig config = {}) noexcept
        : config_(config) {}

    bool addPeer(network::NetworkEndpointId endpoint) {
        if (endpoint == 0) return false;
        const auto found = lowerBound(endpoint);
        if (found != peers_.end() && found->endpoint == endpoint) return true;
        peers_.insert(
            found,
            RtsPeerConnectionQuality{
                endpoint, network::ConnectionQualityTracker(config_)});
        return true;
    }

    bool removePeer(network::NetworkEndpointId endpoint) {
        const auto found = lowerBound(endpoint);
        if (found == peers_.end() || found->endpoint != endpoint) return false;
        peers_.erase(found);
        return true;
    }

    std::uint64_t beginPing(
        network::NetworkEndpointId endpoint,
        std::uint64_t nowMs) {
        auto found = lowerBound(endpoint);
        return found != peers_.end() && found->endpoint == endpoint
            ? found->tracker.beginPing(nowMs)
            : 0;
    }

    bool pingDue(
        network::NetworkEndpointId endpoint,
        std::uint64_t nowMs) const noexcept {
        const auto found = lowerBound(endpoint);
        return found != peers_.end() && found->endpoint == endpoint &&
               found->tracker.pingDue(nowMs);
    }

    bool acknowledge(
        network::NetworkEndpointId endpoint,
        std::uint64_t pingId,
        std::uint64_t nowMs) {
        auto found = lowerBound(endpoint);
        return found != peers_.end() && found->endpoint == endpoint &&
               found->tracker.acknowledge(pingId, nowMs);
    }

    void update(std::uint64_t nowMs) {
        for (auto& peer : peers_) peer.tracker.expire(nowMs);
    }

    RtsNetworkDelivery delivery(
        network::NetworkEndpointId endpoint,
        RtsNetworkDelivery fallback,
        bool adaptive) const noexcept {
        if (!adaptive) return fallback;
        const auto found = lowerBound(endpoint);
        return found == peers_.end() || found->endpoint != endpoint ||
                found->tracker.preferReliableDelivery()
            ? RtsNetworkDelivery::Reliable
            : RtsNetworkDelivery::Unreliable;
    }

    std::uint32_t recommendedInputDelayTicks(
        network::NetworkEndpointId endpoint,
        std::uint32_t tickMilliseconds,
        std::uint32_t fallback) const noexcept {
        const auto found = lowerBound(endpoint);
        return found != peers_.end() && found->endpoint == endpoint
            ? found->tracker.recommendedInputDelayTicks(
                tickMilliseconds, fallback, 12u)
            : fallback;
    }

    const network::ConnectionQualitySnapshot* snapshot(
        network::NetworkEndpointId endpoint) const noexcept {
        const auto found = lowerBound(endpoint);
        return found != peers_.end() && found->endpoint == endpoint
            ? &found->tracker.snapshot()
            : nullptr;
    }

    const std::vector<RtsPeerConnectionQuality>& peers() const noexcept {
        return peers_;
    }

private:
    using Iterator = std::vector<RtsPeerConnectionQuality>::iterator;
    using ConstIterator = std::vector<RtsPeerConnectionQuality>::const_iterator;

    Iterator lowerBound(network::NetworkEndpointId endpoint) noexcept {
        return std::lower_bound(
            peers_.begin(), peers_.end(), endpoint,
            [](const RtsPeerConnectionQuality& value,
               network::NetworkEndpointId id) {
                return value.endpoint < id;
            });
    }

    ConstIterator lowerBound(
        network::NetworkEndpointId endpoint) const noexcept {
        return std::lower_bound(
            peers_.begin(), peers_.end(), endpoint,
            [](const RtsPeerConnectionQuality& value,
               network::NetworkEndpointId id) {
                return value.endpoint < id;
            });
    }

    network::ConnectionQualityConfig config_;
    std::vector<RtsPeerConnectionQuality> peers_;
};

} // namespace rts::gameplay
