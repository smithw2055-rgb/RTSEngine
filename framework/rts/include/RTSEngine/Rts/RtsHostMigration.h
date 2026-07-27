#pragma once

#include <RTSEngine/Network/ConnectionQuality.h>
#include <RTSEngine/Rts/RtsLobby.h>
#include <RTSEngine/Rts/RtsLockstepArchive.h>
#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct RtsHostCandidate final {
    sim::LockstepPeerId peerId{};
    network::NetworkEndpointId endpoint{};
    RtsPrincipalId principalId{};
    std::uint64_t confirmedThrough{};
    network::ConnectionQualitySnapshot quality;
    bool active{};
    bool authorizedToHost{};
};

struct RtsHostMigrationDecision final {
    std::uint64_t migrationEpoch{};
    sim::LockstepPeerId peerId{};
    network::NetworkEndpointId endpoint{};
    RtsPrincipalId principalId{};
};

class RtsHostMigrationElection final {
public:
    bool setCandidate(RtsHostCandidate candidate) {
        if (candidate.peerId == 0 || candidate.endpoint == 0) return false;
        const auto found = lowerBound(candidate.peerId);
        if (found != candidates_.end() && found->peerId == candidate.peerId) {
            *found = candidate;
        } else {
            candidates_.insert(found, candidate);
        }
        return true;
    }

    bool removeCandidate(sim::LockstepPeerId peerId) {
        const auto found = lowerBound(peerId);
        if (found == candidates_.end() || found->peerId != peerId) return false;
        candidates_.erase(found);
        return true;
    }

    bool elect(RtsHostMigrationDecision& decision) {
        const RtsHostCandidate* best = nullptr;
        for (const auto& candidate : candidates_) {
            if (!candidate.active || !candidate.authorizedToHost) continue;
            if (!best || better(candidate, *best)) best = &candidate;
        }
        if (!best || nextEpoch_ == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        decision = {nextEpoch_++, best->peerId, best->endpoint, best->principalId};
        return true;
    }

    const std::vector<RtsHostCandidate>& candidates() const noexcept {
        return candidates_;
    }

private:
    using Iterator = std::vector<RtsHostCandidate>::iterator;

    static std::uint32_t gradeRank(
        network::ConnectionQualityGrade grade) noexcept {
        switch (grade) {
        case network::ConnectionQualityGrade::Excellent: return 0;
        case network::ConnectionQualityGrade::Good: return 1;
        case network::ConnectionQualityGrade::Unknown: return 2;
        case network::ConnectionQualityGrade::Poor: return 3;
        case network::ConnectionQualityGrade::Critical: return 4;
        }
        return 4;
    }

    static bool better(
        const RtsHostCandidate& first,
        const RtsHostCandidate& second) noexcept {
        return std::make_tuple(
                   std::numeric_limits<std::uint64_t>::max() -
                       first.confirmedThrough,
                   gradeRank(first.quality.grade),
                   first.quality.lossPermille,
                   first.quality.smoothedRttMs,
                   first.peerId) <
               std::make_tuple(
                   std::numeric_limits<std::uint64_t>::max() -
                       second.confirmedThrough,
                   gradeRank(second.quality.grade),
                   second.quality.lossPermille,
                   second.quality.smoothedRttMs,
                   second.peerId);
    }

    Iterator lowerBound(sim::LockstepPeerId peerId) noexcept {
        return std::lower_bound(
            candidates_.begin(), candidates_.end(), peerId,
            [](const RtsHostCandidate& value, sim::LockstepPeerId id) {
                return value.peerId < id;
            });
    }

    std::vector<RtsHostCandidate> candidates_;
    std::uint64_t nextEpoch_{1};
};

struct RtsHostMigrationPackage final {
    std::uint64_t sessionId{};
    std::uint64_t migrationEpoch{};
    network::NetworkEndpointId previousHostEndpoint{};
    network::NetworkEndpointId newHostEndpoint{};
    sim::LockstepPeerId newHostPeerId{};
    RtsPrincipalId newHostPrincipalId{};
    std::uint64_t securityEpoch{};
    RtsNetworkContentIdentity identity;
    RtsLobbySnapshot lobby;
    RtsReconnectSnapshot reconnect;
};

inline std::vector<std::uint8_t> EncodeRtsHostMigrationPackage(
    const RtsHostMigrationPackage& value) {
    auto lobbyBytes = EncodeRtsLobbySnapshot(value.lobby);
    auto reconnectBytes = EncodeRtsReconnectSnapshot(value.reconnect);
    if (value.sessionId == 0 || value.migrationEpoch == 0 ||
        value.previousHostEndpoint == 0 || value.newHostEndpoint == 0 ||
        value.previousHostEndpoint == value.newHostEndpoint ||
        value.newHostPeerId == 0 || value.securityEpoch == 0 ||
        value.identity.protocolVersion == 0 || lobbyBytes.empty() ||
        reconnectBytes.empty() || lobbyBytes.size() > 0xFFFFFFFFu ||
        reconnectBytes.size() > RtsGameSessionArchive::kMaximumNestedBytes) {
        return {};
    }
    foundation::BinaryWriter writer;
    writer.writeU32(0x314D4852u);
    writer.writeU16(1u);
    writer.writeU64(value.sessionId);
    writer.writeU64(value.migrationEpoch);
    writer.writeU32(value.previousHostEndpoint);
    writer.writeU32(value.newHostEndpoint);
    writer.writeU32(value.newHostPeerId);
    writer.writeU64(value.newHostPrincipalId);
    writer.writeU64(value.securityEpoch);
    WriteRtsNetworkContentIdentity(writer, value.identity);
    writer.writeU32(static_cast<std::uint32_t>(lobbyBytes.size()));
    writer.writeBytes(lobbyBytes);
    writer.writeU32(static_cast<std::uint32_t>(reconnectBytes.size()));
    writer.writeBytes(reconnectBytes);
    return writer.take();
}

inline bool DecodeRtsHostMigrationPackage(
    const std::vector<std::uint8_t>& bytes,
    RtsHostMigrationPackage& value) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint32_t lobbySize = 0;
    std::uint32_t reconnectSize = 0;
    std::vector<std::uint8_t> lobbyBytes;
    std::vector<std::uint8_t> reconnectBytes;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        !reader.readU64(value.sessionId) ||
        !reader.readU64(value.migrationEpoch) ||
        !reader.readU32(value.previousHostEndpoint) ||
        !reader.readU32(value.newHostEndpoint) ||
        !reader.readU32(value.newHostPeerId) ||
        !reader.readU64(value.newHostPrincipalId) ||
        !reader.readU64(value.securityEpoch) ||
        !ReadRtsNetworkContentIdentity(reader, value.identity) ||
        !reader.readU32(lobbySize) || lobbySize == 0 ||
        lobbySize > 4u * 1024u * 1024u ||
        !reader.readBytes(lobbySize, lobbyBytes, 4u * 1024u * 1024u) ||
        !reader.readU32(reconnectSize) || reconnectSize == 0 ||
        reconnectSize > RtsGameSessionArchive::kMaximumNestedBytes ||
        !reader.readBytes(
            reconnectSize,
            reconnectBytes,
            RtsGameSessionArchive::kMaximumNestedBytes) ||
        !reader.atEnd() || magic != 0x314D4852u || version != 1u ||
        value.sessionId == 0 || value.migrationEpoch == 0 ||
        value.previousHostEndpoint == 0 || value.newHostEndpoint == 0 ||
        value.previousHostEndpoint == value.newHostEndpoint ||
        value.newHostPeerId == 0 || value.securityEpoch == 0 ||
        value.identity.protocolVersion == 0 ||
        !DecodeRtsLobbySnapshot(lobbyBytes, value.lobby) ||
        !DecodeRtsReconnectSnapshot(reconnectBytes, value.reconnect) ||
        value.lobby.sessionId != value.sessionId ||
        value.reconnect.config.sessionId != value.sessionId) {
        return false;
    }

    const auto member = std::find_if(
        value.lobby.members.begin(), value.lobby.members.end(),
        [&value](const RtsLobbyMember& candidate) {
            return candidate.endpoint == value.newHostEndpoint &&
                   candidate.peer.peerId == value.newHostPeerId &&
                   candidate.peer.role == sim::LockstepPeerRole::Player &&
                   candidate.peer.active;
        });
    return member != value.lobby.members.end();
}

} // namespace rts::gameplay
