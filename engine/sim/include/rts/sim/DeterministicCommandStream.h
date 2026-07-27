#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace rts::sim {

// Command must expose targetTick, issuer, and sequence fields. The stream owns
// ordering, idempotent retry handling, conflicting duplicate rejection,
// skipped-Tick cleanup, late-command rejection, and canonical persistence.
enum class CommandSubmitResult : std::uint8_t {
    Accepted,
    DuplicateIdentity,
    Late,
    Unauthorized
};

namespace detail {

template<class T, class = void>
struct HasType : std::false_type {};
template<class T>
struct HasType<T, std::void_t<decltype(std::declval<const T&>().type)>>
    : std::true_type {};

template<class T, class = void>
struct HasSubject : std::false_type {};
template<class T>
struct HasSubject<T, std::void_t<decltype(std::declval<const T&>().subject)>>
    : std::true_type {};

template<class T, class = void>
struct HasTargetX : std::false_type {};
template<class T>
struct HasTargetX<T, std::void_t<decltype(std::declval<const T&>().targetX)>>
    : std::true_type {};

template<class T, class = void>
struct HasTargetY : std::false_type {};
template<class T>
struct HasTargetY<T, std::void_t<decltype(std::declval<const T&>().targetY)>>
    : std::true_type {};

template<class T, class = void>
struct HasAppend : std::false_type {};
template<class T>
struct HasAppend<T, std::void_t<decltype(std::declval<const T&>().append)>>
    : std::true_type {};

template<class T, class = void>
struct HasDefinitionId : std::false_type {};
template<class T>
struct HasDefinitionId<
    T, std::void_t<decltype(std::declval<const T&>().definitionId)>>
    : std::true_type {};

template<class T, class = void>
struct HasObjectId : std::false_type {};
template<class T>
struct HasObjectId<T, std::void_t<decltype(std::declval<const T&>().objectId)>>
    : std::true_type {};

template<class T, class = void>
struct HasTargetEntity : std::false_type {};
template<class T>
struct HasTargetEntity<
    T, std::void_t<decltype(std::declval<const T&>().targetEntity)>>
    : std::true_type {};

template<class T, class = void>
struct HasPayload : std::false_type {};
template<class T>
struct HasPayload<T, std::void_t<decltype(std::declval<const T&>().payload)>>
    : std::true_type {};

} // namespace detail

template<class Command>
class DeterministicCommandStream final {
public:
    struct State {
        std::uint64_t committedThrough{};
        std::vector<Command> pending;
    };

    CommandSubmitResult submitDetailed(Command command) {
        if (command.targetTick < committedThrough_) {
            return CommandSubmitResult::Late;
        }

        auto bucket = lowerBucket(command.targetTick);
        if (bucket == buckets_.end() || bucket->tick != command.targetTick) {
            bucket = buckets_.insert(
                bucket, Bucket{command.targetTick, {}});
        }

        auto position = std::lower_bound(
            bucket->commands.begin(),
            bucket->commands.end(),
            command,
            lessWithinTick);
        if (position != bucket->commands.end() &&
            sameIdentity(*position, command)) {
            // Re-sending the exact command is an idempotent success. Reusing
            // the identity for another payload is a deterministic conflict.
            return samePayload(*position, command)
                ? CommandSubmitResult::Accepted
                : CommandSubmitResult::DuplicateIdentity;
        }

        bucket->commands.insert(position, std::move(command));
        ++pendingCount_;
        return CommandSubmitResult::Accepted;
    }

    bool submit(Command command) {
        return submitDetailed(std::move(command)) ==
               CommandSubmitResult::Accepted;
    }

    std::vector<Command> consume(std::uint64_t tick) {
        if (tick < committedThrough_) return {};
        committedThrough_ = tick + 1u;

        // A caller that intentionally skips a Tick gets the historical stream
        // semantics: commands for skipped Ticks are discarded. Authoritative
        // simulations preflight sequential Tick advancement before calling us.
        const auto firstCurrent = lowerBucket(tick);
        for (auto bucket = buckets_.begin(); bucket != firstCurrent; ++bucket) {
            pendingCount_ -= bucket->commands.size();
        }
        buckets_.erase(buckets_.begin(), firstCurrent);

        if (buckets_.empty() || buckets_.front().tick != tick) return {};
        auto result = std::move(buckets_.front().commands);
        pendingCount_ -= result.size();
        buckets_.erase(buckets_.begin());
        return result;
    }

    State snapshot() const {
        State result;
        result.committedThrough = committedThrough_;
        result.pending.reserve(pendingCount_);
        forEachPending([&](const Command& command) {
            result.pending.push_back(command);
        });
        return result;
    }

    // Canonical allocation-free traversal for Tick hashing, telemetry and
    // read-only inspection. Commands are visited by Tick, issuer and sequence.
    template<class Function>
    void forEachPending(Function&& function) const {
        auto&& callback = function;
        for (const auto& bucket : buckets_) {
            for (const auto& command : bucket.commands) {
                callback(command);
            }
        }
    }

    bool restore(State state) {
        if (std::any_of(
                state.pending.begin(),
                state.pending.end(),
                [&](const Command& command) {
                    return command.targetTick < state.committedThrough;
                }) ||
            !normalize(state.pending)) {
            return false;
        }

        std::vector<Bucket> buckets;
        buckets.reserve(state.pending.size());
        for (auto& command : state.pending) {
            if (buckets.empty() ||
                buckets.back().tick != command.targetTick) {
                buckets.push_back({command.targetTick, {}});
            }
            buckets.back().commands.push_back(std::move(command));
        }

        committedThrough_ = state.committedThrough;
        pendingCount_ = 0;
        for (const auto& bucket : buckets) {
            pendingCount_ += bucket.commands.size();
        }
        buckets_ = std::move(buckets);
        return true;
    }

    void clearPending() noexcept {
        buckets_.clear();
        pendingCount_ = 0;
    }

    std::size_t pending() const noexcept { return pendingCount_; }
    std::size_t bucketCount() const noexcept { return buckets_.size(); }
    std::uint64_t committedThrough() const noexcept {
        return committedThrough_;
    }

private:
    struct Bucket final {
        std::uint64_t tick{};
        std::vector<Command> commands;
    };

    using BucketIterator = typename std::vector<Bucket>::iterator;
    using ConstBucketIterator = typename std::vector<Bucket>::const_iterator;

    BucketIterator lowerBucket(std::uint64_t tick) {
        return std::lower_bound(
            buckets_.begin(),
            buckets_.end(),
            tick,
            [](const Bucket& bucket, std::uint64_t value) {
                return bucket.tick < value;
            });
    }

    ConstBucketIterator lowerBucket(std::uint64_t tick) const {
        return std::lower_bound(
            buckets_.begin(),
            buckets_.end(),
            tick,
            [](const Bucket& bucket, std::uint64_t value) {
                return bucket.tick < value;
            });
    }

    static bool sameIdentity(const Command& a, const Command& b) noexcept {
        return a.targetTick == b.targetTick &&
               a.issuer == b.issuer &&
               a.sequence == b.sequence;
    }

    static bool samePayload(const Command& a, const Command& b) noexcept {
        bool same = true;
        if constexpr (detail::HasType<Command>::value) {
            same = same && a.type == b.type;
        }
        if constexpr (detail::HasSubject<Command>::value) {
            same = same && a.subject == b.subject;
        }
        if constexpr (detail::HasTargetX<Command>::value) {
            same = same && a.targetX == b.targetX;
        }
        if constexpr (detail::HasTargetY<Command>::value) {
            same = same && a.targetY == b.targetY;
        }
        if constexpr (detail::HasAppend<Command>::value) {
            same = same && a.append == b.append;
        }
        if constexpr (detail::HasDefinitionId<Command>::value) {
            same = same && a.definitionId == b.definitionId;
        }
        if constexpr (detail::HasObjectId<Command>::value) {
            same = same && a.objectId == b.objectId;
        }
        if constexpr (detail::HasTargetEntity<Command>::value) {
            same = same && a.targetEntity == b.targetEntity;
        }
        if constexpr (detail::HasPayload<Command>::value) {
            same = same && a.payload == b.payload;
        }
        return same;
    }

    static bool lessWithinTick(const Command& a, const Command& b) noexcept {
        if (a.issuer != b.issuer) return a.issuer < b.issuer;
        return a.sequence < b.sequence;
    }

    static bool less(const Command& a, const Command& b) noexcept {
        if (a.targetTick != b.targetTick) {
            return a.targetTick < b.targetTick;
        }
        return lessWithinTick(a, b);
    }

    static bool normalize(std::vector<Command>& commands) {
        std::stable_sort(commands.begin(), commands.end(), less);
        auto output = commands.begin();
        for (auto current = commands.begin(); current != commands.end();
             ++current) {
            if (output != commands.begin() &&
                sameIdentity(*(output - 1), *current)) {
                if (!samePayload(*(output - 1), *current)) return false;
                continue;
            }
            if (output != current) *output = std::move(*current);
            ++output;
        }
        commands.erase(output, commands.end());
        return true;
    }

    std::vector<Bucket> buckets_;
    std::size_t pendingCount_{};
    std::uint64_t committedThrough_{};
};

} // namespace rts::sim
