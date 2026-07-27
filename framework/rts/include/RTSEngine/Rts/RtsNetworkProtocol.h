#pragma once

#include <RTSEngine/Network/Transport.h>
#include <RTSEngine/Rts/RtsLockstepArchive.h>
#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rts::gameplay {

inline constexpr std::uint16_t kRtsNetworkProtocolVersion = 1u;
inline constexpr std::uint32_t kMaximumLobbyMembers = 256u;
inline constexpr std::uint32_t kMaximumPlayerNameBytes = 64u;

struct RtsNetworkContentIdentity final {
    std::uint16_t protocolVersion{kRtsNetworkProtocolVersion};
    std::uint64_t buildHash{};
    std::uint64_t contentHash{};

    friend bool operator==(
        const RtsNetworkContentIdentity& first,
        const RtsNetworkContentIdentity& second) noexcept {
        return first.protocolVersion == second.protocolVersion &&
               first.buildHash == second.buildHash &&
               first.contentHash == second.contentHash;
    }
};

enum class RtsNetworkMessageKind : std::uint8_t {
    Hello,
    Welcome,
    Reject,
    LobbySnapshot,
    Ready,
    Start,
    LockstepFrame,
    HashReport,
    ReconnectRequest,
    ReconnectSnapshot,
    Ping,
    Pong,
    Disconnect
};

enum class RtsNetworkRejectReason : std::uint8_t {
    ProtocolMismatch,
    BuildMismatch,
    ContentMismatch,
    LobbyFull,
    SessionStarted,
    DuplicateEndpoint,
    InvalidRequest
};

struct RtsNetworkEnvelope final {
    RtsNetworkMessageKind kind{RtsNetworkMessageKind::Hello};
    std::uint64_t sessionId{};
    std::uint64_t requestId{};
    std::vector<std::uint8_t> payload;
};

struct RtsNetworkHello final {
    RtsNetworkContentIdentity identity;
    sim::LockstepPeerRole requestedRole{sim::LockstepPeerRole::Player};
    std::string displayName;
};

struct RtsNetworkWelcome final {
    std::uint64_t sessionId{};
    sim::LockstepPeer peer;
};

struct RtsNetworkReject final {
    RtsNetworkRejectReason reason{RtsNetworkRejectReason::InvalidRequest};
    RtsNetworkContentIdentity expectedIdentity;
};

struct RtsLobbyMember final {
    network::NetworkEndpointId endpoint{};
    sim::LockstepPeer peer;
    bool ready{};
    std::string displayName;
};

struct RtsLobbySnapshot final {
    std::uint64_t sessionId{};
    std::uint64_t revision{};
    bool started{};
    std::vector<RtsLobbyMember> members;
};

struct RtsReadyRequest final {
    sim::LockstepPeerId peerId{};
    bool ready{};
};

struct RtsStartNotice final {
    RtsLockstepConfig lockstep;
    std::vector<sim::LockstepPeer> peers;
};

inline std::vector<std::uint8_t> EncodeRtsNetworkEnvelope(
    const RtsNetworkEnvelope& envelope) {
    if (envelope.payload.size() > 0xFFFFFFFFu) return {};
    foundation::BinaryWriter writer;
    writer.writeU32(0x314E5352u);
    writer.writeU16(kRtsNetworkProtocolVersion);
    writer.writeU8(static_cast<std::uint8_t>(envelope.kind));
    writer.writeU8(0);
    writer.writeU64(envelope.sessionId);
    writer.writeU64(envelope.requestId);
    writer.writeU32(static_cast<std::uint32_t>(envelope.payload.size()));
    writer.writeBytes(envelope.payload);
    return writer.take();
}

inline bool DecodeRtsNetworkEnvelope(
    const std::vector<std::uint8_t>& bytes,
    std::size_t maximumPayloadBytes,
    RtsNetworkEnvelope& envelope) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t rawKind = 0;
    std::uint8_t reserved = 0;
    std::uint32_t payloadBytes = 0;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        !reader.readU8(rawKind) || !reader.readU8(reserved) ||
        !reader.readU64(envelope.sessionId) ||
        !reader.readU64(envelope.requestId) ||
        !reader.readU32(payloadBytes) ||
        magic != 0x314E5352u || version != kRtsNetworkProtocolVersion ||
        reserved != 0 ||
        rawKind > static_cast<std::uint8_t>(
            RtsNetworkMessageKind::Disconnect) ||
        payloadBytes > maximumPayloadBytes ||
        !reader.readBytes(
            payloadBytes, envelope.payload, maximumPayloadBytes) ||
        !reader.atEnd()) {
        return false;
    }
    envelope.kind = static_cast<RtsNetworkMessageKind>(rawKind);
    return true;
}

inline void WriteRtsNetworkContentIdentity(
    foundation::BinaryWriter& writer,
    const RtsNetworkContentIdentity& identity) {
    writer.writeU16(identity.protocolVersion);
    writer.writeU64(identity.buildHash);
    writer.writeU64(identity.contentHash);
}

inline bool ReadRtsNetworkContentIdentity(
    foundation::BinaryReader& reader,
    RtsNetworkContentIdentity& identity) {
    return reader.readU16(identity.protocolVersion) &&
           reader.readU64(identity.buildHash) &&
           reader.readU64(identity.contentHash) &&
           identity.protocolVersion != 0;
}

inline void WriteRtsNetworkPeer(
    foundation::BinaryWriter& writer,
    const sim::LockstepPeer& peer) {
    writer.writeU32(peer.peerId);
    writer.writeU32(peer.playerSlot);
    writer.writeU32(peer.issuer);
    writer.writeU8(static_cast<std::uint8_t>(peer.role));
    writer.writeBool(peer.active);
}

inline bool ReadRtsNetworkPeer(
    foundation::BinaryReader& reader,
    sim::LockstepPeer& peer) {
    std::uint8_t rawRole = 0;
    if (!reader.readU32(peer.peerId) ||
        !reader.readU32(peer.playerSlot) ||
        !reader.readU32(peer.issuer) ||
        !reader.readU8(rawRole) ||
        !reader.readBool(peer.active) ||
        peer.peerId == 0 ||
        rawRole > static_cast<std::uint8_t>(sim::LockstepPeerRole::Spectator)) {
        return false;
    }
    peer.role = static_cast<sim::LockstepPeerRole>(rawRole);
    return peer.role == sim::LockstepPeerRole::Player
        ? peer.playerSlot != 0 && peer.issuer != 0
        : peer.issuer == 0;
}

inline std::vector<std::uint8_t> EncodeRtsNetworkHello(
    const RtsNetworkHello& hello) {
    if (hello.displayName.empty() ||
        hello.displayName.size() > kMaximumPlayerNameBytes) {
        return {};
    }
    foundation::BinaryWriter writer;
    WriteRtsNetworkContentIdentity(writer, hello.identity);
    writer.writeU8(static_cast<std::uint8_t>(hello.requestedRole));
    writer.writeString(hello.displayName);
    return writer.take();
}

inline bool DecodeRtsNetworkHello(
    const std::vector<std::uint8_t>& bytes,
    RtsNetworkHello& hello) {
    foundation::BinaryReader reader(bytes);
    std::uint8_t rawRole = 0;
    return ReadRtsNetworkContentIdentity(reader, hello.identity) &&
           reader.readU8(rawRole) &&
           rawRole <= static_cast<std::uint8_t>(
               sim::LockstepPeerRole::Spectator) &&
           reader.readString(hello.displayName, kMaximumPlayerNameBytes) &&
           !hello.displayName.empty() && reader.atEnd() &&
           ((hello.requestedRole = static_cast<sim::LockstepPeerRole>(rawRole)),
            true);
}

inline std::vector<std::uint8_t> EncodeRtsNetworkWelcome(
    const RtsNetworkWelcome& welcome) {
    if (welcome.sessionId == 0 || welcome.peer.peerId == 0) return {};
    foundation::BinaryWriter writer;
    writer.writeU64(welcome.sessionId);
    WriteRtsNetworkPeer(writer, welcome.peer);
    return writer.take();
}

inline bool DecodeRtsNetworkWelcome(
    const std::vector<std::uint8_t>& bytes,
    RtsNetworkWelcome& welcome) {
    foundation::BinaryReader reader(bytes);
    return reader.readU64(welcome.sessionId) && welcome.sessionId != 0 &&
           ReadRtsNetworkPeer(reader, welcome.peer) && reader.atEnd();
}

inline std::vector<std::uint8_t> EncodeRtsNetworkReject(
    const RtsNetworkReject& reject) {
    foundation::BinaryWriter writer;
    writer.writeU8(static_cast<std::uint8_t>(reject.reason));
    WriteRtsNetworkContentIdentity(writer, reject.expectedIdentity);
    return writer.take();
}

inline bool DecodeRtsNetworkReject(
    const std::vector<std::uint8_t>& bytes,
    RtsNetworkReject& reject) {
    foundation::BinaryReader reader(bytes);
    std::uint8_t rawReason = 0;
    if (!reader.readU8(rawReason) ||
        rawReason > static_cast<std::uint8_t>(
            RtsNetworkRejectReason::InvalidRequest) ||
        !ReadRtsNetworkContentIdentity(reader, reject.expectedIdentity) ||
        !reader.atEnd()) {
        return false;
    }
    reject.reason = static_cast<RtsNetworkRejectReason>(rawReason);
    return true;
}

inline std::vector<std::uint8_t> EncodeRtsLobbySnapshot(
    RtsLobbySnapshot snapshot) {
    if (snapshot.sessionId == 0 ||
        snapshot.members.size() > kMaximumLobbyMembers) {
        return {};
    }
    std::sort(
        snapshot.members.begin(), snapshot.members.end(),
        [](const RtsLobbyMember& first, const RtsLobbyMember& second) {
            return first.peer.peerId < second.peer.peerId;
        });
    foundation::BinaryWriter writer;
    writer.writeU64(snapshot.sessionId);
    writer.writeU64(snapshot.revision);
    writer.writeBool(snapshot.started);
    writer.writeU32(static_cast<std::uint32_t>(snapshot.members.size()));
    for (const auto& member : snapshot.members) {
        if (member.endpoint == 0 || member.displayName.empty() ||
            member.displayName.size() > kMaximumPlayerNameBytes) {
            return {};
        }
        writer.writeU32(member.endpoint);
        WriteRtsNetworkPeer(writer, member.peer);
        writer.writeBool(member.ready);
        writer.writeString(member.displayName);
    }
    return writer.take();
}

inline bool DecodeRtsLobbySnapshot(
    const std::vector<std::uint8_t>& bytes,
    RtsLobbySnapshot& snapshot) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t count = 0;
    if (!reader.readU64(snapshot.sessionId) || snapshot.sessionId == 0 ||
        !reader.readU64(snapshot.revision) ||
        !reader.readBool(snapshot.started) ||
        !reader.readU32(count) || count > kMaximumLobbyMembers) {
        return false;
    }
    snapshot.members.resize(count);
    for (auto& member : snapshot.members) {
        if (!reader.readU32(member.endpoint) || member.endpoint == 0 ||
            !ReadRtsNetworkPeer(reader, member.peer) ||
            !reader.readBool(member.ready) ||
            !reader.readString(
                member.displayName, kMaximumPlayerNameBytes) ||
            member.displayName.empty()) {
            return false;
        }
    }
    if (!reader.atEnd()) return false;
    std::sort(
        snapshot.members.begin(), snapshot.members.end(),
        [](const RtsLobbyMember& first, const RtsLobbyMember& second) {
            return first.peer.peerId < second.peer.peerId;
        });
    for (std::size_t index = 1; index < snapshot.members.size(); ++index) {
        if (snapshot.members[index - 1].peer.peerId ==
                snapshot.members[index].peer.peerId ||
            snapshot.members[index - 1].endpoint ==
                snapshot.members[index].endpoint) {
            return false;
        }
    }
    return true;
}

inline std::vector<std::uint8_t> EncodeRtsReadyRequest(
    const RtsReadyRequest& request) {
    if (request.peerId == 0) return {};
    foundation::BinaryWriter writer;
    writer.writeU32(request.peerId);
    writer.writeBool(request.ready);
    return writer.take();
}

inline bool DecodeRtsReadyRequest(
    const std::vector<std::uint8_t>& bytes,
    RtsReadyRequest& request) {
    foundation::BinaryReader reader(bytes);
    return reader.readU32(request.peerId) && request.peerId != 0 &&
           reader.readBool(request.ready) && reader.atEnd();
}

inline std::vector<std::uint8_t> EncodeRtsStartNotice(
    const RtsStartNotice& notice) {
    if (notice.lockstep.sessionId == 0 || notice.peers.empty() ||
        notice.peers.size() > kMaximumLobbyMembers) {
        return {};
    }
    foundation::BinaryWriter writer;
    writer.writeU64(notice.lockstep.sessionId);
    writer.writeU32(notice.lockstep.inputDelayTicks);
    writer.writeU32(notice.lockstep.maximumPredictionTicks);
    writer.writeU32(notice.lockstep.checkpointIntervalTicks);
    writer.writeU32(notice.lockstep.checkpointCapacity);
    writer.writeU32(notice.lockstep.hashExchangeIntervalTicks);
    writer.writeU32(notice.lockstep.maximumCommandsPerFrame);
    writer.writeU32(static_cast<std::uint32_t>(notice.peers.size()));
    for (const auto& peer : notice.peers) WriteRtsNetworkPeer(writer, peer);
    return writer.take();
}

inline bool DecodeRtsStartNotice(
    const std::vector<std::uint8_t>& bytes,
    RtsStartNotice& notice) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t count = 0;
    if (!reader.readU64(notice.lockstep.sessionId) ||
        notice.lockstep.sessionId == 0 ||
        !reader.readU32(notice.lockstep.inputDelayTicks) ||
        !reader.readU32(notice.lockstep.maximumPredictionTicks) ||
        !reader.readU32(notice.lockstep.checkpointIntervalTicks) ||
        !reader.readU32(notice.lockstep.checkpointCapacity) ||
        !reader.readU32(notice.lockstep.hashExchangeIntervalTicks) ||
        !reader.readU32(notice.lockstep.maximumCommandsPerFrame) ||
        notice.lockstep.checkpointIntervalTicks == 0 ||
        notice.lockstep.checkpointCapacity == 0 ||
        notice.lockstep.hashExchangeIntervalTicks == 0 ||
        notice.lockstep.maximumCommandsPerFrame == 0 ||
        !reader.readU32(count) || count == 0 ||
        count > kMaximumLobbyMembers) {
        return false;
    }
    notice.peers.resize(count);
    for (auto& peer : notice.peers) {
        if (!ReadRtsNetworkPeer(reader, peer)) return false;
    }
    return reader.atEnd();
}

} // namespace rts::gameplay
