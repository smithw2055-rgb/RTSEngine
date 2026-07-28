#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/TechTree.h>
#include <rts/foundation/CanonicalHash.h>

#include <cstdint>

namespace rts::gameplay {

inline std::uint64_t HashTechTreeState(
    const TechTreeRuntime& tech) noexcept {
    foundation::CanonicalHash hash;
    tech.appendHash(hash);
    return hash.Value();
}

inline std::uint64_t HashResearchQueueState(
    const ecs::World& world) {
    foundation::CanonicalHash hash;
    std::uint32_t count = 0;
    world.eachRef<ResearchQueue>(
        [&](ecs::Entity, const ResearchQueue&) { ++count; });
    hash.WriteU32(count);
    world.eachRef<ResearchQueue>(
        [&](ecs::Entity entity, const ResearchQueue& queue) {
            hash.WriteU32(entity.index);
            hash.WriteU32(entity.generation);
            hash.WriteU32(static_cast<std::uint32_t>(queue.items.size()));
            for (const auto& item : queue.items) {
                hash.WriteU32(item.id);
                hash.WriteU32(item.researchDefinitionId);
                hash.WriteU32(static_cast<std::uint32_t>(
                    item.reservedCosts.size()));
                for (const auto& cost : item.reservedCosts) {
                    hash.WriteU32(cost.resourceType);
                    hash.WriteU64(static_cast<std::uint64_t>(cost.amount));
                }
                hash.WriteU32(item.progressTicks);
                hash.WriteU32(item.requiredTicks);
                hash.WriteU32(item.baseRequiredTicks);
            }
        });
    return hash.Value();
}

} // namespace rts::gameplay
