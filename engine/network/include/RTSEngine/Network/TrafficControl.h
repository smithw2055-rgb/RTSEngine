#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace rts::network {

struct TokenBucketConfig final {
    std::uint64_t capacity{};
    std::uint64_t refillTokens{};
    std::uint64_t refillIntervalMs{1000};
};

class TokenBucket final {
public:
    TokenBucket() = default;

    explicit TokenBucket(TokenBucketConfig config) noexcept {
        configure(config);
    }

    void configure(TokenBucketConfig config) noexcept {
        config_ = sanitize(config);
        tokens_ = config_.capacity;
        lastRefillMs_ = 0;
        initialized_ = false;
    }

    bool enabled() const noexcept {
        return config_.capacity != 0 && config_.refillTokens != 0;
    }

    bool consume(std::uint64_t nowMs, std::uint64_t amount = 1) noexcept {
        if (!enabled() || amount == 0) return true;
        refill(nowMs);
        if (amount > tokens_) return false;
        tokens_ -= amount;
        return true;
    }

    std::uint64_t tokens(std::uint64_t nowMs) noexcept {
        refill(nowMs);
        return enabled() ? tokens_ : std::numeric_limits<std::uint64_t>::max();
    }

    const TokenBucketConfig& config() const noexcept { return config_; }

private:
    static TokenBucketConfig sanitize(TokenBucketConfig value) noexcept {
        if (value.capacity == 0 || value.refillTokens == 0) return {};
        value.refillIntervalMs = std::max<std::uint64_t>(1u, value.refillIntervalMs);
        value.refillTokens = std::min(value.refillTokens, value.capacity);
        return value;
    }

    void refill(std::uint64_t nowMs) noexcept {
        if (!enabled()) return;
        if (!initialized_) {
            lastRefillMs_ = nowMs;
            initialized_ = true;
            return;
        }
        if (nowMs <= lastRefillMs_) return;
        const auto elapsed = nowMs - lastRefillMs_;
        const auto intervals = elapsed / config_.refillIntervalMs;
        if (intervals == 0) return;

        const auto missing = config_.capacity - tokens_;
        const auto maximumIntervals = config_.refillTokens == 0
            ? 0
            : (missing + config_.refillTokens - 1u) / config_.refillTokens;
        const auto appliedIntervals = std::min(intervals, maximumIntervals);
        if (appliedIntervals != 0) {
            const auto addition = appliedIntervals >
                    std::numeric_limits<std::uint64_t>::max() /
                        config_.refillTokens
                ? missing
                : std::min(
                    missing, appliedIntervals * config_.refillTokens);
            tokens_ += addition;
        }
        lastRefillMs_ += intervals * config_.refillIntervalMs;
    }

    TokenBucketConfig config_{};
    std::uint64_t tokens_{};
    std::uint64_t lastRefillMs_{};
    bool initialized_{};
};

struct TrafficLimitConfig final {
    TokenBucketConfig packets;
    TokenBucketConfig bytes;
};

struct TrafficControlStats final {
    std::uint64_t acceptedPackets{};
    std::uint64_t acceptedBytes{};
    std::uint64_t rejectedPackets{};
    std::uint64_t rejectedBytes{};
};

class TrafficGovernor final {
public:
    TrafficGovernor() = default;

    explicit TrafficGovernor(TrafficLimitConfig config) noexcept
        : packetBucket_(config.packets), byteBucket_(config.bytes) {}

    void configure(TrafficLimitConfig config) noexcept {
        packetBucket_.configure(config.packets);
        byteBucket_.configure(config.bytes);
        stats_ = {};
    }

    bool allow(std::uint64_t nowMs, std::size_t bytes) noexcept {
        const auto amount = static_cast<std::uint64_t>(bytes);
        auto packetCandidate = packetBucket_;
        auto byteCandidate = byteBucket_;
        if (packetCandidate.consume(nowMs, 1u) &&
            byteCandidate.consume(nowMs, amount)) {
            packetBucket_ = packetCandidate;
            byteBucket_ = byteCandidate;
            ++stats_.acceptedPackets;
            stats_.acceptedBytes = saturatingAdd(stats_.acceptedBytes, amount);
            return true;
        }
        ++stats_.rejectedPackets;
        stats_.rejectedBytes = saturatingAdd(stats_.rejectedBytes, amount);
        return false;
    }

    const TrafficControlStats& stats() const noexcept { return stats_; }

private:
    static std::uint64_t saturatingAdd(
        std::uint64_t value,
        std::uint64_t addition) noexcept {
        return addition > std::numeric_limits<std::uint64_t>::max() - value
            ? std::numeric_limits<std::uint64_t>::max()
            : value + addition;
    }

    TokenBucket packetBucket_;
    TokenBucket byteBucket_;
    TrafficControlStats stats_{};
};

} // namespace rts::network
