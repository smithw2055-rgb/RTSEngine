#pragma once

#include <cstdint>
#include <string_view>

#include <rts/foundation/CanonicalHash.h>

namespace rts::foundation {

using RandomStreamId = std::uint64_t;

struct RandomStreamState {
    RandomStreamId id{};
    std::uint64_t state{};
    std::uint64_t increment{1};

    friend bool operator==(
        const RandomStreamState& a,
        const RandomStreamState& b) noexcept {
        return a.id == b.id &&
               a.state == b.state &&
               a.increment == b.increment;
    }
};

inline RandomStreamId MakeRandomStreamId(std::string_view name) noexcept {
    CanonicalHash hash;
    hash.WriteString(name);
    return hash.Value();
}

class RandomStream final {
public:
    RandomStream(std::uint64_t rootSeed, RandomStreamId id) noexcept
        : id_(id) {
        CanonicalHash hash;
        hash.WriteU64(rootSeed);
        hash.WriteU64(id);
        state_ = SplitMix64(hash.Value());
        increment_ =
            (SplitMix64(hash.Value() ^ 0x9E3779B97F4A7C15ull) << 1u) |
            1u;
        (void)NextU32();
    }

    [[nodiscard]] std::uint32_t NextU32() noexcept {
        const std::uint64_t oldState = state_;
        state_ = oldState * 6364136223846793005ull + increment_;
        const std::uint32_t xorshifted = static_cast<std::uint32_t>(
            ((oldState >> 18u) ^ oldState) >> 27u);
        const std::uint32_t rotation =
            static_cast<std::uint32_t>(oldState >> 59u);
        return (xorshifted >> rotation) |
               (xorshifted << ((-rotation) & 31u));
    }

    [[nodiscard]] std::uint32_t NextBounded(
        std::uint32_t bound) noexcept {
        if (bound == 0) return 0;
        const std::uint32_t threshold =
            static_cast<std::uint32_t>(-bound) % bound;
        for (;;) {
            const auto value = NextU32();
            if (value >= threshold) return value % bound;
        }
    }

    [[nodiscard]] RandomStreamState Snapshot() const noexcept {
        return {id_, state_, increment_};
    }

    bool Restore(RandomStreamState value) noexcept {
        if ((value.increment & 1u) == 0) return false;
        id_ = value.id;
        state_ = value.state;
        increment_ = value.increment;
        return true;
    }

    [[nodiscard]] RandomStreamId Id() const noexcept { return id_; }
    [[nodiscard]] std::uint64_t State() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t Increment() const noexcept {
        return increment_;
    }

private:
    static std::uint64_t SplitMix64(std::uint64_t value) noexcept {
        value += 0x9E3779B97F4A7C15ull;
        value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31u);
    }

    RandomStreamId id_{};
    std::uint64_t state_{0};
    std::uint64_t increment_{1};
};

} // namespace rts::foundation
