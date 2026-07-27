#pragma once

#include <RTSEngine/Rts/Navigation.h>
#include <rts/foundation/CanonicalHash.h>

#include <cstdint>

namespace rts::gameplay {

inline std::uint64_t FinalizeRtsAuthoritativeWorldHash(
    std::uint64_t baseWorldHash,
    std::uint64_t entityRegistryHash,
    GridPoint requiredPathStart,
    GridPoint requiredPathGoal,
    std::uint32_t nextConstructionId,
    std::uint32_t nextProductionId,
    std::uint32_t playerTeamId,
    std::uint16_t compatibilityVersion = 3u,
    std::uint32_t nextResourceNodeId = 0u) noexcept {
    if (compatibilityVersion < 3u) return baseWorldHash;

    foundation::CanonicalHash hash;
    hash.WriteString(
        compatibilityVersion >= 4u
            ? "rts.authoritative-world.v4"
            : "rts.authoritative-world.v3");
    hash.WriteU64(baseWorldHash);
    hash.WriteU64(entityRegistryHash);
    hash.WriteI32(requiredPathStart.x);
    hash.WriteI32(requiredPathStart.y);
    hash.WriteI32(requiredPathGoal.x);
    hash.WriteI32(requiredPathGoal.y);
    hash.WriteU32(nextConstructionId);
    hash.WriteU32(nextProductionId);
    hash.WriteU32(playerTeamId);
    if (compatibilityVersion >= 4u) {
        hash.WriteU32(nextResourceNodeId);
    }
    return hash.Value();
}

} // namespace rts::gameplay
