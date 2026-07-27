#pragma once

#include <RTSEngine/Network/Transport.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace rts::network {

struct LoopbackNetworkConfig final {
    std::uint32_t baseLatencyMs{};
    std::uint32_t jitterMs{};
    std::uint32_t reorderDelayMs{};
    std::uint32_t lossBasisPoints{};
    std::uint32_t duplicateBasisPoints{};
    std::size_t maximumPayloadBytes{kDefaultNetworkMtu};
    std::uint64_t randomSeed{0x9E3779B97F4A7C15ull};
};

class LoopbackNetworkHub final {
public:
    explicit LoopbackNetworkHub(LoopbackNetworkConfig config = {}) noexcept
        : config_(sanitize(config)), randomState_(config_.randomSeed) {}

    bool registerEndpoint(NetworkEndpointId endpoint) {
        if (endpoint == 0) return false;
        const auto found = lowerEndpoint(endpoint);
        if (found != endpoints_.end() && found->id == endpoint) return false;
        endpoints_.insert(found, EndpointState{endpoint, {}});
        return true;
    }

    bool unregisterEndpoint(NetworkEndpointId endpoint) {
        const auto found = lowerEndpoint(endpoint);
        if (found == endpoints_.end() || found->id != endpoint) return false;
        endpoints_.erase(found);
        pending_.erase(
            std::remove_if(
                pending_.begin(), pending_.end(),
                [endpoint](const PendingDatagram& value) {
                    return value.datagram.source == endpoint ||
                           value.datagram.destination == endpoint;
                }),
            pending_.end());
        return true;
    }

    bool registered(NetworkEndpointId endpoint) const noexcept {
        const auto found = lowerEndpoint(endpoint);
        return found != endpoints_.end() && found->id == endpoint;
    }

    std::uint64_t nowMs() const noexcept { return nowMs_; }
    const LoopbackNetworkConfig& config() const noexcept { return config_; }

    void update(std::uint64_t nowMs) {
        if (nowMs < nowMs_) return;
        nowMs_ = nowMs;
        while (!pending_.empty() && pending_.front().deliverAtMs <= nowMs_) {
            auto pending = std::move(pending_.front());
            pending_.erase(pending_.begin());
            auto destination = lowerEndpoint(pending.datagram.destination);
            if (destination != endpoints_.end() &&
                destination->id == pending.datagram.destination) {
                pending.datagram.receivedAtMs = nowMs_;
                destination->inbox.push_back(std::move(pending.datagram));
            }
        }
    }

    TransportSendResult send(
        NetworkEndpointId source,
        NetworkEndpointId destination,
        NetworkChannelId channel,
        NetworkPacketSequence sequence,
        const std::vector<std::uint8_t>& payload) {
        if (!registered(source) || !registered(destination)) {
            return TransportSendResult::InvalidEndpoint;
        }
        if (payload.size() > config_.maximumPayloadBytes) {
            return TransportSendResult::PayloadTooLarge;
        }
        if (roll(config_.lossBasisPoints)) {
            return TransportSendResult::Accepted;
        }

        queueDatagram(source, destination, channel, sequence, payload, false);
        if (roll(config_.duplicateBasisPoints)) {
            queueDatagram(source, destination, channel, sequence, payload, true);
        }
        return TransportSendResult::Accepted;
    }

    bool poll(NetworkEndpointId endpoint, TransportDatagram& output) {
        auto found = lowerEndpoint(endpoint);
        if (found == endpoints_.end() || found->id != endpoint ||
            found->inbox.empty()) {
            return false;
        }
        output = std::move(found->inbox.front());
        found->inbox.erase(found->inbox.begin());
        return true;
    }

private:
    struct EndpointState final {
        NetworkEndpointId id{};
        std::vector<TransportDatagram> inbox;
    };

    struct PendingDatagram final {
        std::uint64_t deliverAtMs{};
        std::uint64_t order{};
        TransportDatagram datagram;
    };

    using EndpointIterator = std::vector<EndpointState>::iterator;
    using EndpointConstIterator = std::vector<EndpointState>::const_iterator;

    static LoopbackNetworkConfig sanitize(LoopbackNetworkConfig value) noexcept {
        value.lossBasisPoints = std::min<std::uint32_t>(
            10000u, value.lossBasisPoints);
        value.duplicateBasisPoints = std::min<std::uint32_t>(
            10000u, value.duplicateBasisPoints);
        value.maximumPayloadBytes = std::max<std::size_t>(
            64u, value.maximumPayloadBytes);
        if (value.randomSeed == 0) {
            value.randomSeed = 0x9E3779B97F4A7C15ull;
        }
        return value;
    }

    EndpointIterator lowerEndpoint(NetworkEndpointId endpoint) noexcept {
        return std::lower_bound(
            endpoints_.begin(), endpoints_.end(), endpoint,
            [](const EndpointState& value, NetworkEndpointId id) {
                return value.id < id;
            });
    }

    EndpointConstIterator lowerEndpoint(NetworkEndpointId endpoint) const noexcept {
        return std::lower_bound(
            endpoints_.begin(), endpoints_.end(), endpoint,
            [](const EndpointState& value, NetworkEndpointId id) {
                return value.id < id;
            });
    }

    std::uint64_t nextRandom() noexcept {
        randomState_ ^= randomState_ >> 12u;
        randomState_ ^= randomState_ << 25u;
        randomState_ ^= randomState_ >> 27u;
        return randomState_ * 0x2545F4914F6CDD1Dull;
    }

    bool roll(std::uint32_t basisPoints) noexcept {
        return basisPoints != 0 &&
               static_cast<std::uint32_t>(nextRandom() % 10000u) < basisPoints;
    }

    std::uint32_t jitter() noexcept {
        if (config_.jitterMs == 0) return 0;
        return static_cast<std::uint32_t>(
            nextRandom() % (static_cast<std::uint64_t>(config_.jitterMs) + 1u));
    }

    void queueDatagram(
        NetworkEndpointId source,
        NetworkEndpointId destination,
        NetworkChannelId channel,
        NetworkPacketSequence sequence,
        const std::vector<std::uint8_t>& payload,
        bool duplicate) {
        std::uint64_t delay = config_.baseLatencyMs + jitter();
        if (config_.reorderDelayMs != 0 && (nextRandom() & 1u) != 0) {
            delay += static_cast<std::uint32_t>(
                nextRandom() %
                (static_cast<std::uint64_t>(config_.reorderDelayMs) + 1u));
        }
        if (duplicate) ++delay;
        PendingDatagram pending;
        pending.deliverAtMs = nowMs_ + delay;
        pending.order = nextOrder_++;
        pending.datagram.source = source;
        pending.datagram.destination = destination;
        pending.datagram.channel = channel;
        pending.datagram.sequence = sequence;
        pending.datagram.sentAtMs = nowMs_;
        pending.datagram.payload = payload;
        const auto found = std::lower_bound(
            pending_.begin(), pending_.end(), pending,
            [](const PendingDatagram& first, const PendingDatagram& second) {
                return std::tie(first.deliverAtMs, first.order) <
                       std::tie(second.deliverAtMs, second.order);
            });
        pending_.insert(found, std::move(pending));
    }

    LoopbackNetworkConfig config_;
    std::vector<EndpointState> endpoints_;
    std::vector<PendingDatagram> pending_;
    std::uint64_t randomState_{};
    std::uint64_t nowMs_{};
    std::uint64_t nextOrder_{};
};

class LoopbackTransport final : public INetworkTransport {
public:
    LoopbackTransport(
        LoopbackNetworkHub& hub,
        NetworkEndpointId endpoint)
        : hub_(&hub), endpoint_(endpoint), open_(hub.registerEndpoint(endpoint)) {}

    ~LoopbackTransport() override {
        if (hub_ && open_) hub_->unregisterEndpoint(endpoint_);
    }

    LoopbackTransport(const LoopbackTransport&) = delete;
    LoopbackTransport& operator=(const LoopbackTransport&) = delete;

    NetworkEndpointId localEndpoint() const noexcept override {
        return endpoint_;
    }

    std::uint64_t nowMs() const noexcept override {
        return hub_ ? hub_->nowMs() : 0;
    }

    std::size_t maximumPayloadBytes() const noexcept override {
        return hub_ ? hub_->config().maximumPayloadBytes : 0;
    }

    bool open() const noexcept override { return open_; }

    TransportSendResult send(
        NetworkEndpointId destination,
        NetworkChannelId channel,
        const std::vector<std::uint8_t>& payload) override {
        if (!hub_ || !open_) return TransportSendResult::NotOpen;
        const auto sequence = ++nextSequence_;
        return hub_->send(
            endpoint_, destination, channel, sequence, payload);
    }

    bool poll(TransportDatagram& datagram) override {
        return hub_ && open_ && hub_->poll(endpoint_, datagram);
    }

    void update(std::uint64_t nowMs) override {
        if (hub_) hub_->update(nowMs);
    }

private:
    LoopbackNetworkHub* hub_{};
    NetworkEndpointId endpoint_{};
    NetworkPacketSequence nextSequence_{};
    bool open_{};
};

} // namespace rts::network
