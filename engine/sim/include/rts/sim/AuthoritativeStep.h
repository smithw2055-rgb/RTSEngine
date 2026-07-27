#pragma once

#include <cstdint>

namespace rts::sim {

enum class AuthoritativeStepFailure : std::uint8_t {
    None,
    StaleTick,
    NonSequentialTick,
    InnerLayerRejected
};

struct AuthoritativeStepValidation final {
    AuthoritativeStepFailure failure{AuthoritativeStepFailure::None};
    std::uint64_t expectedTick{};
    std::uint64_t receivedTick{};

    constexpr explicit operator bool() const noexcept {
        return failure == AuthoritativeStepFailure::None;
    }
};

// Existing RTSEngine applications historically begin at Tick 0 or Tick 1.
// Preserve that entry policy while requiring exact sequential advancement
// after the first committed Tick. All nested simulation layers use this same
// validation contract so a rejected inner Tick cannot split their timelines.
inline constexpr AuthoritativeStepValidation ValidateAuthoritativeStep(
    bool hasStepped,
    std::uint64_t lastCompletedTick,
    std::uint64_t tick) noexcept {
    if (!hasStepped) {
        if (tick <= 1u) {
            return {AuthoritativeStepFailure::None, tick, tick};
        }
        return {AuthoritativeStepFailure::NonSequentialTick, 0u, tick};
    }

    const auto expected = lastCompletedTick + 1u;
    if (tick <= lastCompletedTick) {
        return {AuthoritativeStepFailure::StaleTick, expected, tick};
    }
    if (tick != expected) {
        return {AuthoritativeStepFailure::NonSequentialTick, expected, tick};
    }
    return {AuthoritativeStepFailure::None, expected, tick};
}

} // namespace rts::sim
