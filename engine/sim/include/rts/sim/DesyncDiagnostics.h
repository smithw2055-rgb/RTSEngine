#pragma once

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

#include <rts/sim/Lockstep.h>

namespace rts::sim {

struct StateHashReport final {
    LockstepSessionId sessionId{};
    LockstepPeerId peerId{};
    std::uint64_t tick{};
    std::uint64_t authoritativeHash{};
};

struct DesyncIncident final {
    LockstepPeerId peerId{};
    std::uint64_t tick{};
    std::uint64_t localHash{};
    std::uint64_t remoteHash{};
    std::uint64_t previousMatchingTick{};
    bool hasPreviousMatch{};
};

enum class HashReportSubmitResult : std::uint8_t {
    AcceptedPendingLocal,
    Match,
    Mismatch,
    Duplicate,
    Conflict,
    WrongSession,
    InvalidPeer
};

class DesyncMonitor final {
public:
    explicit DesyncMonitor(LockstepSessionId sessionId = {}) noexcept
        : sessionId_(sessionId) {}

    LockstepSessionId sessionId() const noexcept { return sessionId_; }

    bool recordLocal(std::uint64_t tick, std::uint64_t hash) {
        const auto found = lowerLocal(tick);
        if (found != local_.end() && found->tick == tick) {
            if (found->worldHash == hash) return false;
            found->worldHash = hash;
        } else {
            local_.insert(found, {tick, hash});
        }
        removeIncidentsAt(tick);
        for (const auto& report : remote_) {
            if (report.tick == tick) evaluate(report);
        }
        return true;
    }

    HashReportSubmitResult submitRemote(StateHashReport report) {
        if (report.sessionId != sessionId_) {
            return HashReportSubmitResult::WrongSession;
        }
        if (report.peerId == 0) {
            return HashReportSubmitResult::InvalidPeer;
        }
        const auto found = lowerRemote(report.peerId, report.tick);
        if (found != remote_.end() &&
            found->peerId == report.peerId && found->tick == report.tick) {
            return found->authoritativeHash == report.authoritativeHash
                ? HashReportSubmitResult::Duplicate
                : HashReportSubmitResult::Conflict;
        }
        const auto inserted = remote_.insert(found, report);
        const auto local = lowerLocal(report.tick);
        if (local == local_.end() || local->tick != report.tick) {
            return HashReportSubmitResult::AcceptedPendingLocal;
        }
        return evaluate(*inserted)
            ? HashReportSubmitResult::Match
            : HashReportSubmitResult::Mismatch;
    }

    const std::vector<WorldHashCheckpoint>& localHashes() const noexcept {
        return local_;
    }

    const std::vector<StateHashReport>& remoteReports() const noexcept {
        return remote_;
    }

    const std::vector<DesyncIncident>& incidents() const noexcept {
        return incidents_;
    }

    const WorldHashCheckpoint* localHash(std::uint64_t tick) const noexcept {
        const auto found = lowerLocal(tick);
        return found != local_.end() && found->tick == tick ? &*found : nullptr;
    }

    void rewindFrom(std::uint64_t tick) {
        local_.erase(lowerLocal(tick), local_.end());
        incidents_.erase(
            std::lower_bound(
                incidents_.begin(), incidents_.end(), tick,
                [](const DesyncIncident& value, std::uint64_t target) {
                    return value.tick < target;
                }),
            incidents_.end());
    }

    void clear() noexcept {
        local_.clear();
        remote_.clear();
        incidents_.clear();
    }

private:
    using LocalIterator = std::vector<WorldHashCheckpoint>::iterator;
    using LocalConstIterator =
        std::vector<WorldHashCheckpoint>::const_iterator;
    using RemoteIterator = std::vector<StateHashReport>::iterator;

    LocalIterator lowerLocal(std::uint64_t tick) noexcept {
        return std::lower_bound(
            local_.begin(), local_.end(), tick,
            [](const WorldHashCheckpoint& value, std::uint64_t target) {
                return value.tick < target;
            });
    }

    LocalConstIterator lowerLocal(std::uint64_t tick) const noexcept {
        return std::lower_bound(
            local_.begin(), local_.end(), tick,
            [](const WorldHashCheckpoint& value, std::uint64_t target) {
                return value.tick < target;
            });
    }

    RemoteIterator lowerRemote(
        LockstepPeerId peerId,
        std::uint64_t tick) noexcept {
        const auto identity = std::make_tuple(peerId, tick);
        return std::lower_bound(
            remote_.begin(), remote_.end(), identity,
            [](const StateHashReport& value, const auto& target) {
                return std::make_tuple(value.peerId, value.tick) < target;
            });
    }

    bool evaluate(const StateHashReport& report) {
        const auto local = lowerLocal(report.tick);
        if (local == local_.end() || local->tick != report.tick) return true;
        if (local->worldHash == report.authoritativeHash) return true;

        DesyncIncident incident;
        incident.peerId = report.peerId;
        incident.tick = report.tick;
        incident.localHash = local->worldHash;
        incident.remoteHash = report.authoritativeHash;
        for (auto iterator = remote_.begin(); iterator != remote_.end();
             ++iterator) {
            if (iterator->peerId != report.peerId ||
                iterator->tick >= report.tick) {
                continue;
            }
            const auto previousLocal = lowerLocal(iterator->tick);
            if (previousLocal != local_.end() &&
                previousLocal->tick == iterator->tick &&
                previousLocal->worldHash == iterator->authoritativeHash &&
                (!incident.hasPreviousMatch ||
                 iterator->tick > incident.previousMatchingTick)) {
                incident.previousMatchingTick = iterator->tick;
                incident.hasPreviousMatch = true;
            }
        }

        const auto found = std::lower_bound(
            incidents_.begin(), incidents_.end(),
            std::make_tuple(incident.tick, incident.peerId),
            [](const DesyncIncident& value, const auto& identity) {
                return std::make_tuple(value.tick, value.peerId) < identity;
            });
        if (found != incidents_.end() &&
            found->tick == incident.tick &&
            found->peerId == incident.peerId) {
            *found = incident;
        } else {
            incidents_.insert(found, incident);
        }
        return false;
    }

    void removeIncidentsAt(std::uint64_t tick) {
        incidents_.erase(
            std::remove_if(
                incidents_.begin(), incidents_.end(),
                [tick](const DesyncIncident& value) {
                    return value.tick == tick;
                }),
            incidents_.end());
    }

    LockstepSessionId sessionId_{};
    std::vector<WorldHashCheckpoint> local_;
    std::vector<StateHashReport> remote_;
    std::vector<DesyncIncident> incidents_;
};

} // namespace rts::sim
