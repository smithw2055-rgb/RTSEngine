#pragma once

#include <RTSEngine/Network/Fragmentation.h>
#include <RTSEngine/Network/ReliableChannel.h>
#include <RTSEngine/Network/Security.h>
#include <RTSEngine/Network/TrafficControl.h>
#include <RTSEngine/Network/Transport.h>
#include <RTSEngine/Rts/RtsNetworkProtocol.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace rts::gameplay {

enum class RtsNetworkDelivery : std::uint8_t {
    Reliable,
    Unreliable
};

struct RtsNetworkEndpointConfig final {
    network::NetworkChannelId reliableChannel{1};
    network::NetworkChannelId unreliableChannel{2};
    std::size_t fragmentPayloadBytes{800u};
    std::size_t maximumMessageBytes{160u * 1024u * 1024u};
    std::size_t maximumQueuedFragments{200000u};
    network::ReliableChannelConfig reliability;
    network::TrafficLimitConfig inboundTraffic;
    network::TrafficLimitConfig outboundTraffic;
    bool reliableLockstepFallback{true};
};

struct RtsReceivedNetworkMessage final {
    network::NetworkEndpointId source{};
    RtsNetworkEnvelope envelope;
};

struct RtsNetworkEndpointPeerStats final {
    network::NetworkEndpointId endpoint{};
    std::size_t pendingReliableMessages{};
    bool securityActive{};
    network::ReliableChannelStats reliability;
    network::NetworkSecurityStats security;
    network::TrafficControlStats inboundTraffic;
    network::TrafficControlStats outboundTraffic;
};

class RtsNetworkEndpoint final {
public:
    RtsNetworkEndpoint(
        network::INetworkTransport& transport,
        RtsNetworkEndpointConfig config = {})
        : transport_(transport),
          config_(sanitize(config)),
          fragmenter_(config_.fragmentPayloadBytes) {}

    network::NetworkEndpointId localEndpoint() const noexcept {
        return transport_.localEndpoint();
    }

    std::uint64_t nowMs() const noexcept { return transport_.nowMs(); }

    bool addPeer(network::NetworkEndpointId endpoint) {
        if (endpoint == 0 || endpoint == localEndpoint()) return false;
        const auto found = lowerPeer(endpoint);
        if (found != peers_.end() && found->endpoint == endpoint) return true;
        auto reliability = config_.reliability;
        reliability.maximumMessageBytes = std::max<std::size_t>(
            reliability.maximumMessageBytes,
            config_.fragmentPayloadBytes + 64u);
        PeerState state{
            endpoint,
            network::ReliableChannel(
                endpoint, config_.reliableChannel, reliability),
            network::MessageReassembler(
                {config_.fragmentPayloadBytes + 64u,
                 config_.maximumMessageBytes,
                 32u,
                 30000u}),
            network::NetworkSecuritySession{},
            network::TrafficGovernor(config_.inboundTraffic),
            network::TrafficGovernor(config_.outboundTraffic),
            {},
            0};
        peers_.insert(found, std::move(state));
        return true;
    }

    bool removePeer(network::NetworkEndpointId endpoint) {
        const auto found = lowerPeer(endpoint);
        if (found == peers_.end() || found->endpoint != endpoint) return false;
        peers_.erase(found);
        return true;
    }

    bool enableSecurity(
        network::NetworkEndpointId endpoint,
        network::NetworkSecuritySessionConfig config) {
        auto found = lowerPeer(endpoint);
        return found != peers_.end() && found->endpoint == endpoint &&
               found->security.configure(localEndpoint(), endpoint, config);
    }

    bool disableSecurity(network::NetworkEndpointId endpoint) {
        auto found = lowerPeer(endpoint);
        if (found == peers_.end() || found->endpoint != endpoint) return false;
        found->security.clear();
        return true;
    }

    bool securityActive(network::NetworkEndpointId endpoint) const noexcept {
        const auto found = lowerPeer(endpoint);
        return found != peers_.end() && found->endpoint == endpoint &&
               found->security.active();
    }

    bool setPeerTrafficLimits(
        network::NetworkEndpointId endpoint,
        network::TrafficLimitConfig inbound,
        network::TrafficLimitConfig outbound) {
        auto found = lowerPeer(endpoint);
        if (found == peers_.end() || found->endpoint != endpoint) return false;
        found->inbound.configure(inbound);
        found->outbound.configure(outbound);
        return true;
    }

    bool peerStats(
        network::NetworkEndpointId endpoint,
        RtsNetworkEndpointPeerStats& output) const noexcept {
        const auto found = lowerPeer(endpoint);
        if (found == peers_.end() || found->endpoint != endpoint) return false;
        output.endpoint = endpoint;
        output.pendingReliableMessages =
            found->reliable.pendingCount() +
            (found->queuedFragments.size() - found->nextQueuedFragment);
        output.securityActive = found->security.active();
        output.reliability = found->reliable.stats();
        output.security = found->security.stats();
        output.inboundTraffic = found->inbound.stats();
        output.outboundTraffic = found->outbound.stats();
        return true;
    }

    bool send(
        network::NetworkEndpointId destination,
        RtsNetworkEnvelope envelope,
        RtsNetworkDelivery delivery = RtsNetworkDelivery::Reliable) {
        auto peer = lowerPeer(destination);
        if (peer == peers_.end() || peer->endpoint != destination) return false;

        const bool reliableFallback =
            delivery == RtsNetworkDelivery::Unreliable &&
            config_.reliableLockstepFallback &&
            envelope.kind == RtsNetworkMessageKind::LockstepFrame;
        auto bytes = EncodeRtsNetworkEnvelope(envelope);
        if (bytes.empty() || bytes.size() > config_.maximumMessageBytes) {
            return false;
        }
        if (peer->security.active()) {
            protectedBytes_.clear();
            if (!peer->security.protect(bytes, protectedBytes_)) return false;
            bytes = protectedBytes_;
        }
        if (!peer->outbound.allow(transport_.nowMs(), bytes.size())) return false;

        bool lowLatencyAccepted = false;
        if (delivery == RtsNetworkDelivery::Unreliable) {
            lowLatencyAccepted =
                bytes.size() <= transport_.maximumPayloadBytes() &&
                transport_.send(
                    destination,
                    config_.unreliableChannel,
                    bytes) == network::TransportSendResult::Accepted;
            if (!reliableFallback) return lowLatencyAccepted;
        }

        if (nextMessageId_ == 0 ||
            nextMessageId_ == std::numeric_limits<std::uint64_t>::max()) {
            return lowLatencyAccepted;
        }
        auto fragments = fragmenter_.split(nextMessageId_++, bytes);
        if (fragments.empty() ||
            peer->queuedFragments.size() - peer->nextQueuedFragment +
                    fragments.size() >
                config_.maximumQueuedFragments) {
            return lowLatencyAccepted;
        }
        peer->queuedFragments.insert(
            peer->queuedFragments.end(),
            std::make_move_iterator(fragments.begin()),
            std::make_move_iterator(fragments.end()));
        return true;
    }

    void update(std::uint64_t nowMs) {
        transport_.update(nowMs);
        network::TransportDatagram datagram;
        while (transport_.poll(datagram)) {
            auto peer = lowerPeer(datagram.source);
            if (peer == peers_.end() || peer->endpoint != datagram.source ||
                !peer->inbound.allow(nowMs, datagram.payload.size())) {
                continue;
            }
            if (datagram.channel == config_.unreliableChannel) {
                RtsNetworkEnvelope envelope;
                if (decodeWire(*peer, datagram.payload, envelope)) {
                    received_.push_back(
                        {datagram.source, std::move(envelope)});
                }
                continue;
            }
            if (datagram.channel != config_.reliableChannel) continue;
            reliableMessages_.clear();
            if (!peer->reliable.receive(datagram, reliableMessages_)) continue;
            for (const auto& message : reliableMessages_) {
                reassembled_.clear();
                if (!peer->reassembler.receive(
                        datagram.source,
                        message.payload,
                        nowMs,
                        reassembled_) ||
                    reassembled_.empty()) {
                    continue;
                }
                RtsNetworkEnvelope decoded;
                if (decodeWire(*peer, reassembled_, decoded)) {
                    received_.push_back(
                        {datagram.source, std::move(decoded)});
                }
            }
        }

        for (auto& peer : peers_) {
            while (peer.nextQueuedFragment < peer.queuedFragments.size() &&
                   peer.reliable.queue(
                       peer.queuedFragments[peer.nextQueuedFragment])) {
                ++peer.nextQueuedFragment;
            }
            if (peer.nextQueuedFragment == peer.queuedFragments.size()) {
                peer.queuedFragments.clear();
                peer.nextQueuedFragment = 0;
            }
            peer.reliable.flush(transport_);
            peer.reassembler.expire(nowMs);
        }
    }

    bool poll(RtsReceivedNetworkMessage& message) {
        if (received_.empty()) return false;
        message = std::move(received_.front());
        received_.erase(received_.begin());
        return true;
    }

    std::size_t pendingReliableMessages(
        network::NetworkEndpointId endpoint) const noexcept {
        const auto peer = lowerPeer(endpoint);
        return peer != peers_.end() && peer->endpoint == endpoint
            ? peer->reliable.pendingCount() +
                  (peer->queuedFragments.size() - peer->nextQueuedFragment)
            : 0u;
    }

private:
    struct PeerState final {
        network::NetworkEndpointId endpoint{};
        network::ReliableChannel reliable;
        network::MessageReassembler reassembler;
        network::NetworkSecuritySession security;
        network::TrafficGovernor inbound;
        network::TrafficGovernor outbound;
        std::vector<std::vector<std::uint8_t>> queuedFragments;
        std::size_t nextQueuedFragment{};
    };

    using PeerIterator = std::vector<PeerState>::iterator;
    using PeerConstIterator = std::vector<PeerState>::const_iterator;

    static RtsNetworkEndpointConfig sanitize(
        RtsNetworkEndpointConfig value) noexcept {
        value.fragmentPayloadBytes = std::max<std::size_t>(
            128u, value.fragmentPayloadBytes);
        value.maximumMessageBytes = std::max(
            value.fragmentPayloadBytes, value.maximumMessageBytes);
        value.maximumQueuedFragments = std::max<std::size_t>(
            1u, value.maximumQueuedFragments);
        return value;
    }

    bool decodeWire(
        PeerState& peer,
        const std::vector<std::uint8_t>& wireBytes,
        RtsNetworkEnvelope& envelope) {
        const std::vector<std::uint8_t>* clear = &wireBytes;
        if (peer.security.active() ||
            network::NetworkSecuritySession::looksProtected(wireBytes)) {
            clearBytes_.clear();
            const auto result = peer.security.open(wireBytes, clearBytes_);
            if (result != network::NetworkSecurityOpenResult::Accepted &&
                result != network::NetworkSecurityOpenResult::NotProtected) {
                return false;
            }
            clear = &clearBytes_;
        }
        return DecodeRtsNetworkEnvelope(
            *clear, config_.maximumMessageBytes, envelope);
    }

    PeerIterator lowerPeer(network::NetworkEndpointId endpoint) noexcept {
        return std::lower_bound(
            peers_.begin(), peers_.end(), endpoint,
            [](const PeerState& value, network::NetworkEndpointId id) {
                return value.endpoint < id;
            });
    }

    PeerConstIterator lowerPeer(
        network::NetworkEndpointId endpoint) const noexcept {
        return std::lower_bound(
            peers_.begin(), peers_.end(), endpoint,
            [](const PeerState& value, network::NetworkEndpointId id) {
                return value.endpoint < id;
            });
    }

    network::INetworkTransport& transport_;
    RtsNetworkEndpointConfig config_;
    network::MessageFragmenter fragmenter_;
    std::vector<PeerState> peers_;
    std::vector<RtsReceivedNetworkMessage> received_;
    std::vector<network::ReliableMessage> reliableMessages_;
    std::vector<std::uint8_t> reassembled_;
    std::vector<std::uint8_t> protectedBytes_;
    std::vector<std::uint8_t> clearBytes_;
    std::uint64_t nextMessageId_{1};
};

} // namespace rts::gameplay
