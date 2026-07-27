#pragma once

#include <RTSEngine/Network/Transport.h>
#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::network {

struct ReliableChannelConfig final {
    std::uint32_t resendIntervalMs{100};
    std::uint32_t maximumAttempts{20};
    std::size_t maximumInFlight{256};
    std::size_t maximumMessageBytes{1024};
};

struct ReliableMessage final {
    NetworkEndpointId source{};
    std::uint64_t sequence{};
    std::vector<std::uint8_t> payload;
};

struct ReliableChannelStats final {
    std::uint64_t packetsSent{};
    std::uint64_t packetsReceived{};
    std::uint64_t packetsResent{};
    std::uint64_t duplicatesSuppressed{};
    std::uint64_t messagesAcknowledged{};
    std::uint64_t messagesFailed{};
};

class ReliableChannel final {
public:
    ReliableChannel(
        NetworkEndpointId remoteEndpoint,
        NetworkChannelId transportChannel,
        ReliableChannelConfig config = {}) noexcept
        : remoteEndpoint_(remoteEndpoint),
          transportChannel_(transportChannel),
          config_(sanitize(config)) {}

    NetworkEndpointId remoteEndpoint() const noexcept { return remoteEndpoint_; }
    NetworkChannelId transportChannel() const noexcept { return transportChannel_; }
    std::size_t pendingCount() const noexcept { return pending_.size(); }
    const ReliableChannelStats& stats() const noexcept { return stats_; }

    bool queue(std::vector<std::uint8_t> payload) {
        if (remoteEndpoint_ == 0 || payload.size() > config_.maximumMessageBytes ||
            pending_.size() >= config_.maximumInFlight ||
            nextSendSequence_ == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        pending_.push_back({nextSendSequence_++, std::move(payload), 0, 0});
        return true;
    }

    void flush(INetworkTransport& transport) {
        if (!transport.open() || transport.localEndpoint() == 0 ||
            transport.localEndpoint() == remoteEndpoint_) return;

        const auto now = transport.nowMs();
        if (ackDirty_) {
            const auto packet = encodePacket(PacketKind::Ack, 0, {});
            if (packet.size() <= transport.maximumPayloadBytes() &&
                transport.send(remoteEndpoint_, transportChannel_, packet) ==
                    TransportSendResult::Accepted) {
                ++stats_.packetsSent;
                ackDirty_ = false;
            }
        }

        for (auto& message : pending_) {
            const bool first = message.attempts == 0;
            const bool due = first ||
                now >= message.lastSentAtMs + config_.resendIntervalMs;
            if (!due || message.attempts >= config_.maximumAttempts) continue;
            const auto packet = encodePacket(
                PacketKind::Data, message.sequence, message.payload);
            if (packet.size() > transport.maximumPayloadBytes()) {
                message.attempts = config_.maximumAttempts;
                continue;
            }
            if (transport.send(remoteEndpoint_, transportChannel_, packet) ==
                TransportSendResult::Accepted) {
                message.lastSentAtMs = now;
                ++message.attempts;
                ++stats_.packetsSent;
                if (!first) ++stats_.packetsResent;
            }
        }

        const auto failed = std::count_if(
            pending_.begin(), pending_.end(),
            [this](const PendingMessage& value) {
                return value.attempts >= config_.maximumAttempts;
            });
        stats_.messagesFailed += static_cast<std::uint64_t>(failed);
        pending_.erase(
            std::remove_if(
                pending_.begin(), pending_.end(),
                [this](const PendingMessage& value) {
                    return value.attempts >= config_.maximumAttempts;
                }),
            pending_.end());
    }

    bool receive(
        const TransportDatagram& datagram,
        std::vector<ReliableMessage>& delivered) {
        if (datagram.source != remoteEndpoint_ ||
            datagram.channel != transportChannel_) return false;

        Packet packet;
        if (!decodePacket(datagram.payload, packet)) return false;
        ++stats_.packetsReceived;
        acknowledge(packet.acknowledgement, packet.acknowledgementBits);
        if (packet.kind == PacketKind::Ack) return true;

        ackDirty_ = true;
        if (packet.sequence < nextDeliverSequence_ ||
            !markReceived(packet.sequence)) {
            ++stats_.duplicatesSuppressed;
            return true;
        }
        const auto found = std::lower_bound(
            buffered_.begin(), buffered_.end(), packet.sequence,
            [](const BufferedMessage& value, std::uint64_t sequence) {
                return value.sequence < sequence;
            });
        buffered_.insert(
            found,
            BufferedMessage{packet.sequence, std::move(packet.payload)});
        while (!buffered_.empty() &&
               buffered_.front().sequence == nextDeliverSequence_) {
            delivered.push_back(
                {datagram.source,
                 buffered_.front().sequence,
                 std::move(buffered_.front().payload)});
            buffered_.erase(buffered_.begin());
            ++nextDeliverSequence_;
        }
        return true;
    }

private:
    static constexpr std::uint32_t kMagic = 0x314C4552u;
    static constexpr std::uint16_t kVersion = 1u;

    enum class PacketKind : std::uint8_t { Data, Ack };

    struct PendingMessage final {
        std::uint64_t sequence{};
        std::vector<std::uint8_t> payload;
        std::uint64_t lastSentAtMs{};
        std::uint32_t attempts{};
    };

    struct BufferedMessage final {
        std::uint64_t sequence{};
        std::vector<std::uint8_t> payload;
    };

    struct Packet final {
        PacketKind kind{PacketKind::Data};
        std::uint64_t sequence{};
        std::uint64_t acknowledgement{};
        std::uint64_t acknowledgementBits{};
        std::vector<std::uint8_t> payload;
    };

    static ReliableChannelConfig sanitize(ReliableChannelConfig value) noexcept {
        value.resendIntervalMs = std::max<std::uint32_t>(1u, value.resendIntervalMs);
        value.maximumAttempts = std::max<std::uint32_t>(1u, value.maximumAttempts);
        value.maximumInFlight = std::max<std::size_t>(1u, value.maximumInFlight);
        value.maximumMessageBytes = std::max<std::size_t>(1u, value.maximumMessageBytes);
        return value;
    }

    std::vector<std::uint8_t> encodePacket(
        PacketKind kind,
        std::uint64_t sequence,
        const std::vector<std::uint8_t>& payload) const {
        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU8(static_cast<std::uint8_t>(kind));
        writer.writeU8(0);
        writer.writeU64(sequence);
        writer.writeU64(highestReceived_);
        writer.writeU64(receivedBits_);
        writer.writeU32(static_cast<std::uint32_t>(payload.size()));
        writer.writeBytes(payload);
        return writer.take();
    }

    bool decodePacket(
        const std::vector<std::uint8_t>& bytes,
        Packet& packet) const {
        foundation::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint8_t rawKind = 0;
        std::uint8_t reserved = 0;
        std::uint32_t payloadBytes = 0;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU8(rawKind) || !reader.readU8(reserved) ||
            !reader.readU64(packet.sequence) ||
            !reader.readU64(packet.acknowledgement) ||
            !reader.readU64(packet.acknowledgementBits) ||
            !reader.readU32(payloadBytes) ||
            magic != kMagic || version != kVersion || reserved != 0 ||
            rawKind > static_cast<std::uint8_t>(PacketKind::Ack) ||
            payloadBytes > config_.maximumMessageBytes ||
            !reader.readBytes(
                payloadBytes, packet.payload, config_.maximumMessageBytes) ||
            !reader.atEnd()) return false;
        packet.kind = static_cast<PacketKind>(rawKind);
        return packet.kind == PacketKind::Data
            ? packet.sequence != 0
            : packet.sequence == 0 && packet.payload.empty();
    }

    void acknowledge(std::uint64_t acknowledgement, std::uint64_t bits) {
        if (acknowledgement == 0 || pending_.empty()) return;
        const auto before = pending_.size();
        pending_.erase(
            std::remove_if(
                pending_.begin(), pending_.end(),
                [acknowledgement, bits](const PendingMessage& value) {
                    if (value.sequence == acknowledgement) return true;
                    if (value.sequence > acknowledgement) return false;
                    const auto distance = acknowledgement - value.sequence;
                    return distance >= 1u && distance <= 64u &&
                           (bits & (std::uint64_t{1} << (distance - 1u))) != 0;
                }),
            pending_.end());
        stats_.messagesAcknowledged +=
            static_cast<std::uint64_t>(before - pending_.size());
    }

    bool markReceived(std::uint64_t sequence) noexcept {
        if (highestReceived_ == 0) {
            highestReceived_ = sequence;
            receivedBits_ = 0;
            return true;
        }
        if (sequence == highestReceived_) return false;
        if (sequence > highestReceived_) {
            const auto distance = sequence - highestReceived_;
            receivedBits_ = distance >= 64u ? 0u : receivedBits_ << distance;
            if (distance <= 64u) {
                receivedBits_ |= std::uint64_t{1} << (distance - 1u);
            }
            highestReceived_ = sequence;
            return true;
        }
        const auto distance = highestReceived_ - sequence;
        if (distance == 0 || distance > 64u) return false;
        const auto mask = std::uint64_t{1} << (distance - 1u);
        if ((receivedBits_ & mask) != 0) return false;
        receivedBits_ |= mask;
        return true;
    }

    NetworkEndpointId remoteEndpoint_{};
    NetworkChannelId transportChannel_{};
    ReliableChannelConfig config_;
    std::vector<PendingMessage> pending_;
    std::vector<BufferedMessage> buffered_;
    ReliableChannelStats stats_;
    std::uint64_t nextSendSequence_{1};
    std::uint64_t nextDeliverSequence_{1};
    std::uint64_t highestReceived_{};
    std::uint64_t receivedBits_{};
    bool ackDirty_{};
};

} // namespace rts::network
