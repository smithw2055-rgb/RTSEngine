#pragma once

#include <RTSEngine/Rts/RtsNetworkProtocol.h>
#include <RTSEngine/Rts/RtsOnlineServices.h>
#include <rts/foundation/BinaryArchive.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rts::gameplay {

inline constexpr std::uint16_t kRtsAuthenticationProtocolVersion = 1u;
inline constexpr std::uint32_t kMaximumAuthenticationBlobBytes = 64u * 1024u;
inline constexpr std::uint32_t kMaximumAccountIdBytes = 256u;

struct RtsAuthenticatedHello final {
    RtsNetworkHello hello;
    RtsAuthenticationResponse response;
};

struct RtsAuthenticationChallengeNotice final {
    RtsAuthenticationChallenge challenge;
};

enum class RtsAuthenticationFailureReason : std::uint8_t {
    Rejected,
    Expired,
    Replay,
    InvalidResponse,
    PermissionDenied,
    RateLimited,
    ServiceUnavailable
};

struct RtsAuthenticationFailureNotice final {
    RtsAuthenticationFailureReason reason{
        RtsAuthenticationFailureReason::Rejected};
};

struct RtsAuthenticatedWelcome final {
    RtsNetworkWelcome welcome;
    RtsAuthenticatedPrincipal principal;
};

namespace rts_auth_protocol_detail {

inline void WriteBlob(
    foundation::BinaryWriter& writer,
    const std::vector<std::uint8_t>& value) {
    writer.writeU32(static_cast<std::uint32_t>(value.size()));
    writer.writeBytes(value);
}

inline bool ReadBlob(
    foundation::BinaryReader& reader,
    std::vector<std::uint8_t>& value) {
    std::uint32_t count = 0;
    return reader.readU32(count) &&
           reader.readBytes(count, value, kMaximumAuthenticationBlobBytes);
}

inline void WriteChallenge(
    foundation::BinaryWriter& writer,
    const RtsAuthenticationChallenge& value) {
    writer.writeU64(value.challengeId);
    writer.writeU64(value.issuedAtMs);
    writer.writeU64(value.expiresAtMs);
    WriteBlob(writer, value.nonce);
    WriteBlob(writer, value.providerData);
}

inline bool ReadChallenge(
    foundation::BinaryReader& reader,
    RtsAuthenticationChallenge& value) {
    return reader.readU64(value.challengeId) &&
           reader.readU64(value.issuedAtMs) &&
           reader.readU64(value.expiresAtMs) &&
           ReadBlob(reader, value.nonce) &&
           ReadBlob(reader, value.providerData) &&
           value.challengeId != 0 &&
           value.expiresAtMs > value.issuedAtMs &&
           !value.nonce.empty();
}

inline void WriteResponse(
    foundation::BinaryWriter& writer,
    const RtsAuthenticationResponse& value) {
    writer.writeU64(value.challengeId);
    writer.writeString(value.accountId);
    WriteBlob(writer, value.credential);
    WriteBlob(writer, value.proof);
}

inline bool ReadResponse(
    foundation::BinaryReader& reader,
    RtsAuthenticationResponse& value) {
    return reader.readU64(value.challengeId) &&
           reader.readString(value.accountId, kMaximumAccountIdBytes) &&
           ReadBlob(reader, value.credential) &&
           ReadBlob(reader, value.proof) &&
           value.challengeId != 0 && !value.accountId.empty();
}

inline void WritePrincipal(
    foundation::BinaryWriter& writer,
    const RtsAuthenticatedPrincipal& value) {
    writer.writeU64(value.challengeId);
    writer.writeU64(value.principalId);
    writer.writeString(value.accountId);
    writer.writeString(value.displayName);
    writer.writeU32(value.permissions);
    writer.writeU64(value.securityKeyId);
    writer.writeU64(value.securityEpoch);
    WriteBlob(writer, value.securityContext);
}

inline bool ReadPrincipal(
    foundation::BinaryReader& reader,
    RtsAuthenticatedPrincipal& value) {
    return reader.readU64(value.challengeId) &&
           reader.readU64(value.principalId) &&
           reader.readString(value.accountId, kMaximumAccountIdBytes) &&
           reader.readString(value.displayName, kMaximumPlayerNameBytes) &&
           reader.readU32(value.permissions) &&
           reader.readU64(value.securityKeyId) &&
           reader.readU64(value.securityEpoch) &&
           ReadBlob(reader, value.securityContext) &&
           value.challengeId != 0 && value.principalId != 0 &&
           !value.accountId.empty() && !value.displayName.empty() &&
           value.permissions != 0 && value.securityEpoch != 0;
}

inline void WriteBaseHello(
    foundation::BinaryWriter& writer,
    const RtsNetworkHello& hello) {
    WriteRtsNetworkContentIdentity(writer, hello.identity);
    writer.writeU8(static_cast<std::uint8_t>(hello.requestedRole));
    writer.writeString(hello.displayName);
}

inline bool ReadBaseHello(
    foundation::BinaryReader& reader,
    RtsNetworkHello& hello) {
    std::uint8_t rawRole = 0;
    if (!ReadRtsNetworkContentIdentity(reader, hello.identity) ||
        !reader.readU8(rawRole) ||
        rawRole > static_cast<std::uint8_t>(
            sim::LockstepPeerRole::Spectator) ||
        !reader.readString(hello.displayName, kMaximumPlayerNameBytes) ||
        hello.displayName.empty()) {
        return false;
    }
    hello.requestedRole = static_cast<sim::LockstepPeerRole>(rawRole);
    return true;
}

inline void WriteBaseWelcome(
    foundation::BinaryWriter& writer,
    const RtsNetworkWelcome& welcome) {
    writer.writeU64(welcome.sessionId);
    WriteRtsNetworkPeer(writer, welcome.peer);
}

inline bool ReadBaseWelcome(
    foundation::BinaryReader& reader,
    RtsNetworkWelcome& welcome) {
    return reader.readU64(welcome.sessionId) && welcome.sessionId != 0 &&
           ReadRtsNetworkPeer(reader, welcome.peer);
}

} // namespace rts_auth_protocol_detail

inline std::vector<std::uint8_t> EncodeRtsAuthenticatedHello(
    const RtsAuthenticatedHello& value) {
    if (value.hello.displayName.empty() ||
        value.hello.displayName.size() > kMaximumPlayerNameBytes ||
        value.response.challengeId == 0 || value.response.accountId.empty() ||
        value.response.accountId.size() > kMaximumAccountIdBytes ||
        value.response.credential.size() > kMaximumAuthenticationBlobBytes ||
        value.response.proof.size() > kMaximumAuthenticationBlobBytes) {
        return {};
    }
    foundation::BinaryWriter writer;
    writer.writeU32(0x31484152u);
    writer.writeU16(kRtsAuthenticationProtocolVersion);
    rts_auth_protocol_detail::WriteBaseHello(writer, value.hello);
    rts_auth_protocol_detail::WriteResponse(writer, value.response);
    return writer.take();
}

inline bool DecodeRtsAuthenticatedHello(
    const std::vector<std::uint8_t>& bytes,
    RtsAuthenticatedHello& value) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    return reader.readU32(magic) && reader.readU16(version) &&
           magic == 0x31484152u &&
           version == kRtsAuthenticationProtocolVersion &&
           rts_auth_protocol_detail::ReadBaseHello(reader, value.hello) &&
           rts_auth_protocol_detail::ReadResponse(reader, value.response) &&
           reader.atEnd();
}

inline std::vector<std::uint8_t> EncodeRtsAuthenticationChallengeNotice(
    const RtsAuthenticationChallengeNotice& value) {
    if (value.challenge.challengeId == 0 ||
        value.challenge.expiresAtMs <= value.challenge.issuedAtMs ||
        value.challenge.nonce.empty() ||
        value.challenge.nonce.size() > kMaximumAuthenticationBlobBytes ||
        value.challenge.providerData.size() > kMaximumAuthenticationBlobBytes) {
        return {};
    }
    foundation::BinaryWriter writer;
    writer.writeU32(0x31434152u);
    writer.writeU16(kRtsAuthenticationProtocolVersion);
    rts_auth_protocol_detail::WriteChallenge(writer, value.challenge);
    return writer.take();
}

inline bool DecodeRtsAuthenticationChallengeNotice(
    const std::vector<std::uint8_t>& bytes,
    RtsAuthenticationChallengeNotice& value) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    return reader.readU32(magic) && reader.readU16(version) &&
           magic == 0x31434152u &&
           version == kRtsAuthenticationProtocolVersion &&
           rts_auth_protocol_detail::ReadChallenge(reader, value.challenge) &&
           reader.atEnd();
}

inline std::vector<std::uint8_t> EncodeRtsAuthenticationFailureNotice(
    const RtsAuthenticationFailureNotice& value) {
    foundation::BinaryWriter writer;
    writer.writeU32(0x31464152u);
    writer.writeU16(kRtsAuthenticationProtocolVersion);
    writer.writeU8(static_cast<std::uint8_t>(value.reason));
    return writer.take();
}

inline bool DecodeRtsAuthenticationFailureNotice(
    const std::vector<std::uint8_t>& bytes,
    RtsAuthenticationFailureNotice& value) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t rawReason = 0;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        !reader.readU8(rawReason) || magic != 0x31464152u ||
        version != kRtsAuthenticationProtocolVersion ||
        rawReason > static_cast<std::uint8_t>(
            RtsAuthenticationFailureReason::ServiceUnavailable) ||
        !reader.atEnd()) {
        return false;
    }
    value.reason = static_cast<RtsAuthenticationFailureReason>(rawReason);
    return true;
}

inline std::vector<std::uint8_t> EncodeRtsAuthenticatedWelcome(
    const RtsAuthenticatedWelcome& value) {
    if (value.welcome.sessionId == 0 || value.welcome.peer.peerId == 0 ||
        value.principal.challengeId == 0 || value.principal.principalId == 0 ||
        value.principal.accountId.empty() || value.principal.displayName.empty() ||
        value.principal.securityContext.size() >
            kMaximumAuthenticationBlobBytes) {
        return {};
    }
    foundation::BinaryWriter writer;
    writer.writeU32(0x31574152u);
    writer.writeU16(kRtsAuthenticationProtocolVersion);
    rts_auth_protocol_detail::WriteBaseWelcome(writer, value.welcome);
    rts_auth_protocol_detail::WritePrincipal(writer, value.principal);
    return writer.take();
}

inline bool DecodeRtsAuthenticatedWelcome(
    const std::vector<std::uint8_t>& bytes,
    RtsAuthenticatedWelcome& value) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    return reader.readU32(magic) && reader.readU16(version) &&
           magic == 0x31574152u &&
           version == kRtsAuthenticationProtocolVersion &&
           rts_auth_protocol_detail::ReadBaseWelcome(reader, value.welcome) &&
           rts_auth_protocol_detail::ReadPrincipal(reader, value.principal) &&
           reader.atEnd();
}

} // namespace rts::gameplay
