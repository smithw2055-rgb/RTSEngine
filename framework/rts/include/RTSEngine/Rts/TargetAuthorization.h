#pragma once

#include <cstdint>

namespace rts::gameplay {

class RtsSimulation;

using TargetAuthorizationCallback = bool(*)(
    const void* context,
    std::uint32_t observerTeam,
    std::uint32_t targetTeam);

void BindRtsTargetAuthorization(
    const RtsSimulation& simulation,
    const void* context,
    TargetAuthorizationCallback callback) noexcept;

void UnbindRtsTargetAuthorization(
    const RtsSimulation& simulation) noexcept;

bool IsRtsTargetAuthorized(
    const RtsSimulation& simulation,
    std::uint32_t observerTeam,
    std::uint32_t targetTeam) noexcept;

} // namespace rts::gameplay
