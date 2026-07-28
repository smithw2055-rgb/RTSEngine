#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::network {

enum class ConnectionQualityGrade : std::uint8_t {
    Unknown,
    Excellent,
    Good,
    Poor,
    Critical
};

struct ConnectionQualityConfig final {
    std::uint64_t pingIntervalMs{1000};
    std::uint64_t pingTimeoutMs{3000};
    std::uint32_t poorRttMs{180};
    std::uint32_t criticalRttMs{350};
    std::uint32_t poorLossPermille{80};
    std::uint32_t criticalLossPermille{200};
    std::size_t maximumPendingPings{64};
};

struct ConnectionQualitySnapshot final {
    ConnectionQualityGrade grade{ConnectionQualityGrade::Unknown};
    std::uint32_t smoothedRttMs{};
    std::uint32_t jitterMs{};
    std::uint32_t lossPermille{};
    std::uint64_t pingsSent{};
    std::uint64_t pingsAcknowledged{};
    std::uint64_t pingsLost{};
    std::uint64_t lastResponseAtMs{};
};

class ConnectionQualityTracker final {
public:
    explicit ConnectionQualityTracker(
        ConnectionQualityConfig config = {}) noexcept
        : config_(sanitize(config)) {}

    bool pingDue(std::uint64_t nowMs) const noexcept {
        return pending_.size() < config_.maximumPendingPings &&
               (lastPingAtMs_ == 0 ||
                nowMs >= lastPingAtMs_ + config_.pingIntervalMs);
    }

    std::uint64_t beginPing(std::uint64_t nowMs) {
        expire(nowMs);
        if (!pingDue(nowMs) ||
            nextPingId_ == std::numeric_limits<std::uint64_t>::max()) {
            return 0;
        }
        const auto id = nextPingId_++;
        pending_.push_back({id, nowMs});
        lastPingAtMs_ = nowMs;
        ++snapshot_.pingsSent;
        refreshGrade();
        return id;
    }

    bool acknowledge(std::uint64_t pingId, std::uint64_t nowMs) {
        expire(nowMs);
        const auto found = std::find_if(
            pending_.begin(), pending_.end(),
            [pingId](const PendingPing& value) { return value.id == pingId; });
        if (found == pending_.end() || nowMs < found->sentAtMs) return false;
        const auto sample64 = nowMs - found->sentAtMs;
        const auto sample = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            sample64, std::numeric_limits<std::uint32_t>::max()));
        if (snapshot_.pingsAcknowledged == 0) {
            snapshot_.smoothedRttMs = sample;
            snapshot_.jitterMs = 0;
        } else {
            const auto previous = snapshot_.smoothedRttMs;
            snapshot_.smoothedRttMs = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(previous) * 7u + sample) / 8u);
            const auto deviation = previous > sample
                ? previous - sample
                : sample - previous;
            snapshot_.jitterMs = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(snapshot_.jitterMs) * 3u +
                 deviation) /
                4u);
        }
        ++snapshot_.pingsAcknowledged;
        snapshot_.lastResponseAtMs = nowMs;
        pending_.erase(found);
        refreshLoss();
        refreshGrade();
        return true;
    }

    void expire(std::uint64_t nowMs) {
        const auto before = pending_.size();
        pending_.erase(
            std::remove_if(
                pending_.begin(), pending_.end(),
                [this, nowMs](const PendingPing& value) {
                    return nowMs >= value.sentAtMs + config_.pingTimeoutMs;
                }),
            pending_.end());
        snapshot_.pingsLost += static_cast<std::uint64_t>(before - pending_.size());
        refreshLoss();
        refreshGrade();
    }

    const ConnectionQualitySnapshot& snapshot() const noexcept {
        return snapshot_;
    }

    bool preferReliableDelivery() const noexcept {
        return snapshot_.grade == ConnectionQualityGrade::Unknown ||
               snapshot_.grade == ConnectionQualityGrade::Poor ||
               snapshot_.grade == ConnectionQualityGrade::Critical;
    }

    std::uint32_t recommendedInputDelayTicks(
        std::uint32_t tickMilliseconds,
        std::uint32_t minimumTicks = 1,
        std::uint32_t maximumTicks = 12) const noexcept {
        tickMilliseconds = std::max<std::uint32_t>(1u, tickMilliseconds);
        minimumTicks = std::max<std::uint32_t>(1u, minimumTicks);
        maximumTicks = std::max(minimumTicks, maximumTicks);
        if (snapshot_.grade == ConnectionQualityGrade::Unknown) return minimumTicks;
        const auto budget = static_cast<std::uint64_t>(snapshot_.smoothedRttMs) / 2u +
                            snapshot_.jitterMs * 2u;
        const auto ticks = static_cast<std::uint32_t>(
            (budget + tickMilliseconds - 1u) / tickMilliseconds) + 1u;
        return std::clamp(ticks, minimumTicks, maximumTicks);
    }

private:
    struct PendingPing final {
        std::uint64_t id{};
        std::uint64_t sentAtMs{};
    };

    static ConnectionQualityConfig sanitize(
        ConnectionQualityConfig value) noexcept {
        value.pingIntervalMs = std::max<std::uint64_t>(1u, value.pingIntervalMs);
        value.pingTimeoutMs = std::max(value.pingIntervalMs, value.pingTimeoutMs);
        value.criticalRttMs = std::max(value.poorRttMs, value.criticalRttMs);
        value.criticalLossPermille = std::max(
            value.poorLossPermille, value.criticalLossPermille);
        value.maximumPendingPings = std::clamp<std::size_t>(
            value.maximumPendingPings, 1u, 1024u);
        return value;
    }

    void refreshLoss() noexcept {
        const auto total = snapshot_.pingsAcknowledged + snapshot_.pingsLost;
        snapshot_.lossPermille = total == 0
            ? 0u
            : static_cast<std::uint32_t>(std::min<std::uint64_t>(
                1000u,
                (snapshot_.pingsLost * 1000u) / total));
    }

    void refreshGrade() noexcept {
        if (snapshot_.pingsAcknowledged == 0) {
            snapshot_.grade = snapshot_.pingsLost == 0
                ? ConnectionQualityGrade::Unknown
                : ConnectionQualityGrade::Critical;
            return;
        }
        if (snapshot_.smoothedRttMs >= config_.criticalRttMs ||
            snapshot_.lossPermille >= config_.criticalLossPermille) {
            snapshot_.grade = ConnectionQualityGrade::Critical;
        } else if (snapshot_.smoothedRttMs >= config_.poorRttMs ||
                   snapshot_.lossPermille >= config_.poorLossPermille) {
            snapshot_.grade = ConnectionQualityGrade::Poor;
        } else if (snapshot_.smoothedRttMs <= config_.poorRttMs / 2u &&
                   snapshot_.lossPermille <= config_.poorLossPermille / 4u) {
            snapshot_.grade = ConnectionQualityGrade::Excellent;
        } else {
            snapshot_.grade = ConnectionQualityGrade::Good;
        }
    }

    ConnectionQualityConfig config_;
    ConnectionQualitySnapshot snapshot_{};
    std::vector<PendingPing> pending_;
    std::uint64_t nextPingId_{1};
    std::uint64_t lastPingAtMs_{};
};

} // namespace rts::network
