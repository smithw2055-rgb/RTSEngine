#pragma once

#include <RTSEngine/Rts/RtsNetworkProtocol.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct RtsLobbyHostConfig final {
    std::uint64_t sessionId{};
    RtsNetworkContentIdentity identity;
    std::uint32_t maximumPlayers{8};
    std::uint32_t maximumSpectators{16};
};

enum class RtsLobbyJoinResult : std::uint8_t {
    Accepted,
    Rejected
};

class RtsLobbyHost final {
public:
    explicit RtsLobbyHost(RtsLobbyHostConfig config) noexcept
        : config_(sanitize(config)) {}

    const RtsLobbyHostConfig& config() const noexcept { return config_; }
    bool started() const noexcept { return started_; }
    std::uint64_t revision() const noexcept { return revision_; }

    bool registerHost(
        network::NetworkEndpointId endpoint,
        std::string displayName,
        bool ready = true) {
        if (started_ || endpoint == 0 || displayName.empty() ||
            displayName.size() > kMaximumPlayerNameBytes ||
            !members_.empty()) {
            return false;
        }
        RtsLobbyMember member;
        member.endpoint = endpoint;
        member.peer.peerId = nextPeerId_++;
        member.peer.playerSlot = 1;
        member.peer.issuer = 1;
        member.peer.role = sim::LockstepPeerRole::Player;
        member.peer.active = true;
        member.ready = ready;
        member.displayName = std::move(displayName);
        members_.push_back(std::move(member));
        ++revision_;
        return true;
    }

    RtsLobbyJoinResult join(
        network::NetworkEndpointId endpoint,
        const RtsNetworkHello& hello,
        RtsNetworkWelcome& welcome,
        RtsNetworkReject& reject) {
        reject.expectedIdentity = config_.identity;
        if (started_) return rejectWith(
            RtsNetworkRejectReason::SessionStarted, reject);
        if (endpoint == 0 || hello.displayName.empty() ||
            hello.displayName.size() > kMaximumPlayerNameBytes) {
            return rejectWith(RtsNetworkRejectReason::InvalidRequest, reject);
        }
        if (findByEndpoint(endpoint)) {
            return rejectWith(RtsNetworkRejectReason::DuplicateEndpoint, reject);
        }
        if (hello.identity.protocolVersion != config_.identity.protocolVersion) {
            return rejectWith(RtsNetworkRejectReason::ProtocolMismatch, reject);
        }
        if (hello.identity.buildHash != config_.identity.buildHash) {
            return rejectWith(RtsNetworkRejectReason::BuildMismatch, reject);
        }
        if (hello.identity.contentHash != config_.identity.contentHash) {
            return rejectWith(RtsNetworkRejectReason::ContentMismatch, reject);
        }

        const auto players = countRole(sim::LockstepPeerRole::Player);
        const auto spectators = countRole(sim::LockstepPeerRole::Spectator);
        if ((hello.requestedRole == sim::LockstepPeerRole::Player &&
             players >= config_.maximumPlayers) ||
            (hello.requestedRole == sim::LockstepPeerRole::Spectator &&
             spectators >= config_.maximumSpectators)) {
            return rejectWith(RtsNetworkRejectReason::LobbyFull, reject);
        }

        RtsLobbyMember member;
        member.endpoint = endpoint;
        member.peer.peerId = nextPeerId_++;
        member.peer.role = hello.requestedRole;
        member.peer.active = true;
        if (hello.requestedRole == sim::LockstepPeerRole::Player) {
            member.peer.playerSlot = nextPlayerSlot();
            member.peer.issuer = member.peer.playerSlot;
        }
        member.displayName = hello.displayName;
        members_.push_back(member);
        sortMembers();
        ++revision_;

        welcome.sessionId = config_.sessionId;
        welcome.peer = member.peer;
        return RtsLobbyJoinResult::Accepted;
    }

    bool setReady(
        network::NetworkEndpointId endpoint,
        sim::LockstepPeerId peerId,
        bool ready) {
        if (started_) return false;
        auto* member = findByEndpoint(endpoint);
        if (!member || member->peer.peerId != peerId ||
            member->peer.role != sim::LockstepPeerRole::Player) {
            return false;
        }
        if (member->ready == ready) return true;
        member->ready = ready;
        ++revision_;
        return true;
    }

    bool remove(network::NetworkEndpointId endpoint) {
        if (started_) return false;
        const auto found = std::find_if(
            members_.begin(), members_.end(),
            [endpoint](const RtsLobbyMember& value) {
                return value.endpoint == endpoint;
            });
        if (found == members_.end()) return false;
        members_.erase(found);
        ++revision_;
        return true;
    }

    bool canStart() const noexcept {
        std::size_t players = 0;
        for (const auto& member : members_) {
            if (!member.peer.active ||
                member.peer.role != sim::LockstepPeerRole::Player) {
                continue;
            }
            ++players;
            if (!member.ready) return false;
        }
        return players != 0;
    }

    bool start() {
        if (started_ || !canStart()) return false;
        started_ = true;
        ++revision_;
        return true;
    }

    RtsLobbySnapshot snapshot() const {
        return {config_.sessionId, revision_, started_, members_};
    }

    std::vector<sim::LockstepPeer> lockstepPeers() const {
        std::vector<sim::LockstepPeer> peers;
        peers.reserve(members_.size());
        for (const auto& member : members_) peers.push_back(member.peer);
        std::sort(
            peers.begin(), peers.end(),
            [](const auto& first, const auto& second) {
                return first.peerId < second.peerId;
            });
        return peers;
    }

    const std::vector<RtsLobbyMember>& members() const noexcept {
        return members_;
    }

private:
    static RtsLobbyHostConfig sanitize(RtsLobbyHostConfig value) noexcept {
        value.maximumPlayers = std::max<std::uint32_t>(1u, value.maximumPlayers);
        value.maximumSpectators = std::max<std::uint32_t>(
            1u, value.maximumSpectators);
        return value;
    }

    RtsLobbyJoinResult rejectWith(
        RtsNetworkRejectReason reason,
        RtsNetworkReject& reject) const noexcept {
        reject.reason = reason;
        return RtsLobbyJoinResult::Rejected;
    }

    std::size_t countRole(sim::LockstepPeerRole role) const noexcept {
        return static_cast<std::size_t>(std::count_if(
            members_.begin(), members_.end(),
            [role](const RtsLobbyMember& value) {
                return value.peer.active && value.peer.role == role;
            }));
    }

    std::uint32_t nextPlayerSlot() const noexcept {
        std::uint32_t candidate = 1;
        for (;;) {
            const auto used = std::any_of(
                members_.begin(), members_.end(),
                [candidate](const RtsLobbyMember& value) {
                    return value.peer.role == sim::LockstepPeerRole::Player &&
                           value.peer.playerSlot == candidate;
                });
            if (!used) return candidate;
            ++candidate;
        }
    }

    RtsLobbyMember* findByEndpoint(
        network::NetworkEndpointId endpoint) noexcept {
        const auto found = std::find_if(
            members_.begin(), members_.end(),
            [endpoint](const RtsLobbyMember& value) {
                return value.endpoint == endpoint;
            });
        return found == members_.end() ? nullptr : &*found;
    }

    const RtsLobbyMember* findByEndpoint(
        network::NetworkEndpointId endpoint) const noexcept {
        const auto found = std::find_if(
            members_.begin(), members_.end(),
            [endpoint](const RtsLobbyMember& value) {
                return value.endpoint == endpoint;
            });
        return found == members_.end() ? nullptr : &*found;
    }

    void sortMembers() {
        std::sort(
            members_.begin(), members_.end(),
            [](const RtsLobbyMember& first, const RtsLobbyMember& second) {
                return first.peer.peerId < second.peer.peerId;
            });
    }

    RtsLobbyHostConfig config_;
    std::vector<RtsLobbyMember> members_;
    std::uint64_t revision_{};
    sim::LockstepPeerId nextPeerId_{1};
    bool started_{};
};

enum class RtsLobbyClientState : std::uint8_t {
    Disconnected,
    Joining,
    Joined,
    Ready,
    Started,
    Rejected
};

class RtsLobbyClient final {
public:
    explicit RtsLobbyClient(RtsNetworkHello hello)
        : hello_(std::move(hello)) {}

    const RtsNetworkHello& hello() const noexcept { return hello_; }
    RtsLobbyClientState state() const noexcept { return state_; }
    const RtsNetworkWelcome& welcome() const noexcept { return welcome_; }
    const RtsLobbySnapshot& lobby() const noexcept { return lobby_; }
    const RtsNetworkReject& rejection() const noexcept { return rejection_; }

    void markJoining() noexcept { state_ = RtsLobbyClientState::Joining; }

    bool acceptWelcome(RtsNetworkWelcome welcome) {
        if (state_ != RtsLobbyClientState::Joining ||
            welcome.sessionId == 0 || welcome.peer.peerId == 0) {
            return false;
        }
        welcome_ = welcome;
        state_ = RtsLobbyClientState::Joined;
        return true;
    }

    bool applySnapshot(RtsLobbySnapshot snapshot) {
        if (state_ == RtsLobbyClientState::Disconnected ||
            state_ == RtsLobbyClientState::Rejected ||
            (welcome_.sessionId != 0 &&
             snapshot.sessionId != welcome_.sessionId) ||
            snapshot.revision < lobby_.revision) {
            return false;
        }
        lobby_ = std::move(snapshot);
        if (lobby_.started) state_ = RtsLobbyClientState::Started;
        return true;
    }

    bool markReady(bool ready) {
        if (state_ != RtsLobbyClientState::Joined &&
            state_ != RtsLobbyClientState::Ready) {
            return false;
        }
        state_ = ready
            ? RtsLobbyClientState::Ready
            : RtsLobbyClientState::Joined;
        return true;
    }

    void reject(RtsNetworkReject value) {
        rejection_ = value;
        state_ = RtsLobbyClientState::Rejected;
    }

private:
    RtsNetworkHello hello_;
    RtsNetworkWelcome welcome_;
    RtsLobbySnapshot lobby_;
    RtsNetworkReject rejection_;
    RtsLobbyClientState state_{RtsLobbyClientState::Disconnected};
};

} // namespace rts::gameplay
