#pragma once

#include <RTSEngine/Network/Fragmentation.h>
#include <RTSEngine/Network/ReliableChannel.h>
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
};

struct RtsReceivedNetworkMessage final {
    network::NetworkEndpointId source{};
    RtsNetworkEnvelope envelope;
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

    bool send(
        network::NetworkEndpointId destination,
        RtsNetworkEnvelope envelope,
        RtsNetworkDelivery delivery = RtsNetworkDelivery::Reliable) {
        auto peer = lowerPeer(destination);
        if (peer == peers_.end() || peer->endpoint != destination) return false;
        auto bytes = EncodeRtsNetworkEnvelope(envelope);
        if (bytes.empty() || bytes.size() > config_.maximumMessageBytes) {
            return false;
        }
        if (delivery == RtsNetworkDelivery::Unreliable) {
            return bytes.size() <= transport_.maximumPayloadBytes() &&
                   transport_.send(
                       destination,
                       config_.unreliableChannel,
                       bytes) == network::TransportSendResult::Accepted;
        }

        if (nextMessageId_ == 0 ||
            nextMessageId_ == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        auto fragments = fragmenter_.split(nextMessageId_++, bytes);
        if (fragments.empty() ||
            peer->queuedFragments.size() - peer->nextQueuedFragment +
                    fragments.size() >
                config_.maximumQueuedFragments) {
            return false;
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
            if (peer == peers_.end() || peer->endpoint != datagram.source) {
                continue;
            }
            if (datagram.channel == config_.unreliableChannel) {
                RtsNetworkEnvelope envelope;
                if (DecodeRtsNetworkEnvelope(
                        datagram.payload,
                        config_.maximumMessageBytes,
                        envelope)) {
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
                RtsNetworkEnvelope envelope;
                if (DecodeRtsNetworkEnvelope(
                        reassembled_,
                        config_.maximumMessageBytes,
                        envelope)) {
                    received_.push_back(
                        {datagram.source, std::move(envelope)});
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
    std::uint64_t nextMessageId_{1};
};

} // namespace rts::gameplay
