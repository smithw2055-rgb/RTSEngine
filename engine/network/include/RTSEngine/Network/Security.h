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

struct NetworkSecurityRequest final {
    NetworkEndpointId localEndpoint{};
    NetworkEndpointId remoteEndpoint{};
    std::uint64_t keyId{};
    std::uint64_t epoch{};
    std::uint64_t nonce{};
    const std::vector<std::uint8_t>* associatedData{};
    const std::vector<std::uint8_t>* input{};
};

class INetworkSecurityProvider {
public:
    virtual ~INetworkSecurityProvider() = default;

    virtual std::size_t maximumTagBytes() const noexcept = 0;

    virtual bool seal(
        const NetworkSecurityRequest& request,
        std::vector<std::uint8_t>& ciphertext,
        std::vector<std::uint8_t>& authenticationTag) = 0;

    virtual bool open(
        const NetworkSecurityRequest& request,
        const std::vector<std::uint8_t>& authenticationTag,
        std::vector<std::uint8_t>& plaintext) = 0;
};

class ReplayWindow64 final {
public:
    bool accept(std::uint64_t nonce) noexcept {
        if (nonce == 0) return false;
        if (highest_ == 0) {
            highest_ = nonce;
            receivedBits_ = 0;
            return true;
        }
        if (nonce == highest_) return false;
        if (nonce > highest_) {
            const auto distance = nonce - highest_;
            receivedBits_ = distance >= 64u ? 0u : receivedBits_ << distance;
            if (distance <= 64u) {
                receivedBits_ |= std::uint64_t{1} << (distance - 1u);
            }
            highest_ = nonce;
            return true;
        }

        const auto distance = highest_ - nonce;
        if (distance == 0 || distance > 64u) return false;
        const auto mask = std::uint64_t{1} << (distance - 1u);
        if ((receivedBits_ & mask) != 0) return false;
        receivedBits_ |= mask;
        return true;
    }

    void reset() noexcept {
        highest_ = 0;
        receivedBits_ = 0;
    }

    std::uint64_t highest() const noexcept { return highest_; }

private:
    std::uint64_t highest_{};
    std::uint64_t receivedBits_{};
};

enum class NetworkSecurityOpenResult : std::uint8_t {
    Accepted,
    NotProtected,
    InvalidPacket,
    InvalidContext,
    ReplayRejected,
    AuthenticationFailed
};

struct NetworkSecuritySessionConfig final {
    INetworkSecurityProvider* provider{};
    std::uint64_t keyId{};
    std::uint64_t epoch{1};
    bool requireProtectedTraffic{true};
    std::size_t maximumPacketBytes{160u * 1024u * 1024u};
};

struct NetworkSecurityStats final {
    std::uint64_t protectedSent{};
    std::uint64_t protectedReceived{};
    std::uint64_t clearRejected{};
    std::uint64_t replayRejected{};
    std::uint64_t authenticationFailed{};
};

class NetworkSecuritySession final {
public:
    static constexpr std::uint32_t kMagic = 0x31434553u;
    static constexpr std::uint16_t kVersion = 1u;

    NetworkSecuritySession() = default;

    NetworkSecuritySession(
        NetworkEndpointId localEndpoint,
        NetworkEndpointId remoteEndpoint,
        NetworkSecuritySessionConfig config) noexcept {
        configure(localEndpoint, remoteEndpoint, config);
    }

    bool configure(
        NetworkEndpointId localEndpoint,
        NetworkEndpointId remoteEndpoint,
        NetworkSecuritySessionConfig config) noexcept {
        if (localEndpoint == 0 || remoteEndpoint == 0 ||
            localEndpoint == remoteEndpoint || !config.provider ||
            config.keyId == 0 || config.epoch == 0 ||
            config.maximumPacketBytes < 64u) {
            clear();
            return false;
        }
        localEndpoint_ = localEndpoint;
        remoteEndpoint_ = remoteEndpoint;
        config_ = config;
        nextSendNonce_ = 1;
        replay_.reset();
        stats_ = {};
        active_ = true;
        return true;
    }

    void clear() noexcept {
        localEndpoint_ = 0;
        remoteEndpoint_ = 0;
        config_ = {};
        nextSendNonce_ = 1;
        replay_.reset();
        stats_ = {};
        active_ = false;
    }

    bool active() const noexcept { return active_; }
    bool requireProtectedTraffic() const noexcept {
        return active_ && config_.requireProtectedTraffic;
    }
    std::uint64_t keyId() const noexcept { return config_.keyId; }
    std::uint64_t epoch() const noexcept { return config_.epoch; }
    const NetworkSecurityStats& stats() const noexcept { return stats_; }

    bool protect(
        const std::vector<std::uint8_t>& plaintext,
        std::vector<std::uint8_t>& packet) {
        if (!active_ || !config_.provider || plaintext.empty() ||
            nextSendNonce_ == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }

        const auto nonce = nextSendNonce_;
        const auto associatedData = makeAssociatedData(
            localEndpoint_, remoteEndpoint_, config_.keyId, config_.epoch, nonce);
        NetworkSecurityRequest request{
            localEndpoint_,
            remoteEndpoint_,
            config_.keyId,
            config_.epoch,
            nonce,
            &associatedData,
            &plaintext};
        std::vector<std::uint8_t> ciphertext;
        std::vector<std::uint8_t> tag;
        if (!config_.provider->seal(request, ciphertext, tag) ||
            tag.empty() || tag.size() > config_.provider->maximumTagBytes() ||
            ciphertext.size() > 0xFFFFFFFFu || tag.size() > 0xFFFFu) {
            return false;
        }

        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU16(static_cast<std::uint16_t>(tag.size()));
        writer.writeU32(localEndpoint_);
        writer.writeU32(remoteEndpoint_);
        writer.writeU64(config_.keyId);
        writer.writeU64(config_.epoch);
        writer.writeU64(nonce);
        writer.writeU32(static_cast<std::uint32_t>(ciphertext.size()));
        writer.writeBytes(ciphertext);
        writer.writeBytes(tag);
        if (writer.bytes().size() > config_.maximumPacketBytes) return false;

        packet = writer.take();
        ++nextSendNonce_;
        ++stats_.protectedSent;
        return true;
    }

    NetworkSecurityOpenResult open(
        const std::vector<std::uint8_t>& packet,
        std::vector<std::uint8_t>& plaintext) {
        if (!looksProtected(packet)) {
            if (requireProtectedTraffic()) {
                ++stats_.clearRejected;
                return NetworkSecurityOpenResult::InvalidContext;
            }
            plaintext = packet;
            return NetworkSecurityOpenResult::NotProtected;
        }
        if (!active_ || !config_.provider ||
            packet.size() > config_.maximumPacketBytes) {
            return NetworkSecurityOpenResult::InvalidContext;
        }

        foundation::BinaryReader reader(packet);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint16_t tagBytes = 0;
        NetworkEndpointId source = 0;
        NetworkEndpointId destination = 0;
        std::uint64_t keyId = 0;
        std::uint64_t epoch = 0;
        std::uint64_t nonce = 0;
        std::uint32_t ciphertextBytes = 0;
        std::vector<std::uint8_t> ciphertext;
        std::vector<std::uint8_t> tag;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU16(tagBytes) || !reader.readU32(source) ||
            !reader.readU32(destination) || !reader.readU64(keyId) ||
            !reader.readU64(epoch) || !reader.readU64(nonce) ||
            !reader.readU32(ciphertextBytes) || magic != kMagic ||
            version != kVersion || source != remoteEndpoint_ ||
            destination != localEndpoint_ || keyId != config_.keyId ||
            epoch != config_.epoch || nonce == 0 || tagBytes == 0 ||
            tagBytes > config_.provider->maximumTagBytes() ||
            ciphertextBytes > config_.maximumPacketBytes ||
            !reader.readBytes(
                ciphertextBytes, ciphertext, config_.maximumPacketBytes) ||
            !reader.readBytes(tagBytes, tag, config_.provider->maximumTagBytes()) ||
            !reader.atEnd()) {
            return NetworkSecurityOpenResult::InvalidPacket;
        }

        auto candidateWindow = replay_;
        if (!candidateWindow.accept(nonce)) {
            ++stats_.replayRejected;
            return NetworkSecurityOpenResult::ReplayRejected;
        }

        const auto associatedData = makeAssociatedData(
            source, destination, keyId, epoch, nonce);
        NetworkSecurityRequest request{
            localEndpoint_,
            remoteEndpoint_,
            keyId,
            epoch,
            nonce,
            &associatedData,
            &ciphertext};
        if (!config_.provider->open(request, tag, plaintext)) {
            ++stats_.authenticationFailed;
            return NetworkSecurityOpenResult::AuthenticationFailed;
        }

        replay_ = candidateWindow;
        ++stats_.protectedReceived;
        return NetworkSecurityOpenResult::Accepted;
    }

    static bool looksProtected(
        const std::vector<std::uint8_t>& packet) noexcept {
        if (packet.size() < sizeof(std::uint32_t)) return false;
        const auto value = static_cast<std::uint32_t>(packet[0]) |
            (static_cast<std::uint32_t>(packet[1]) << 8u) |
            (static_cast<std::uint32_t>(packet[2]) << 16u) |
            (static_cast<std::uint32_t>(packet[3]) << 24u);
        return value == kMagic;
    }

private:
    static std::vector<std::uint8_t> makeAssociatedData(
        NetworkEndpointId source,
        NetworkEndpointId destination,
        std::uint64_t keyId,
        std::uint64_t epoch,
        std::uint64_t nonce) {
        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU32(source);
        writer.writeU32(destination);
        writer.writeU64(keyId);
        writer.writeU64(epoch);
        writer.writeU64(nonce);
        return writer.take();
    }

    NetworkEndpointId localEndpoint_{};
    NetworkEndpointId remoteEndpoint_{};
    NetworkSecuritySessionConfig config_{};
    ReplayWindow64 replay_;
    NetworkSecurityStats stats_{};
    std::uint64_t nextSendNonce_{1};
    bool active_{};
};

} // namespace rts::network
