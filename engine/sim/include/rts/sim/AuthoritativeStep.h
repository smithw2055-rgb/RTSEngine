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

inline constexpr AuthoritativeStepValidation ValidateAuthoritativeStep(
    bool hasStepped,
    std::uint64_t lastCompletedTick,
    std::uint64_t tick) noexcept {
    const auto expected = hasStepped ? lastCompletedTick + 1u : 0u;
    if (hasStepped && tick <= lastCompletedTick) {
        return {AuthoritativeStepFailure::StaleTick, expected, tick};
    }
    if (tick != expected) {
        return {AuthoritativeStepFailure::NonSequentialTick, expected, tick};
    }
    return {AuthoritativeStepFailure::None, expected, tick};
}

} // namespace rts::sim
