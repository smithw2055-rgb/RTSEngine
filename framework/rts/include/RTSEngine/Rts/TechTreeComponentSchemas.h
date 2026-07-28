#pragma once

#include <RTSEngine/Ecs/ComponentSchema.h>
#include <RTSEngine/Rts/TechTree.h>

#include <cstdint>
#include <limits>

namespace rts::gameplay {

inline bool RegisterTechTreeComponentSchemas(
    ecs::ComponentSchemaRegistry& schemas) {
    return schemas.registerSchema<ResearchQueue>(
        0x52545319u,
        1u,
        "rts.ResearchQueue",
        [](foundation::BinaryWriter& writer, const ResearchQueue& value) {
            writer.writeU32(static_cast<std::uint32_t>(value.items.size()));
            for (const auto& item : value.items) {
                writer.writeU32(item.id);
                writer.writeU32(item.researchDefinitionId);
                writer.writeU32(static_cast<std::uint32_t>(
                    item.reservedCosts.size()));
                for (const auto& cost : item.reservedCosts) {
                    writer.writeU32(cost.resourceType);
                    writer.writeU64(static_cast<std::uint64_t>(cost.amount));
                }
                writer.writeU32(item.progressTicks);
                writer.writeU32(item.requiredTicks);
                writer.writeU32(item.baseRequiredTicks);
            }
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           ResearchQueue& value) {
            std::uint32_t count = 0;
            if (version != 1u || !reader.readU32(count) ||
                count > 1000000u) {
                return false;
            }
            value.items.clear();
            value.items.resize(count);
            for (auto& item : value.items) {
                std::uint32_t costCount = 0;
                if (!reader.readU32(item.id) ||
                    !reader.readU32(item.researchDefinitionId) ||
                    !reader.readU32(costCount) ||
                    costCount > 65536u || item.id == 0 ||
                    item.researchDefinitionId == 0) {
                    return false;
                }
                item.reservedCosts.resize(costCount);
                for (auto& cost : item.reservedCosts) {
                    std::uint64_t amount = 0;
                    if (!reader.readU32(cost.resourceType) ||
                        !reader.readU64(amount) ||
                        cost.resourceType == 0 ||
                        amount > static_cast<std::uint64_t>(
                            std::numeric_limits<ResourceAmount>::max())) {
                        return false;
                    }
                    cost.amount = static_cast<ResourceAmount>(amount);
                }
                if (!ValidateResourceCosts(item.reservedCosts) ||
                    !reader.readU32(item.progressTicks) ||
                    !reader.readU32(item.requiredTicks) ||
                    !reader.readU32(item.baseRequiredTicks) ||
                    item.requiredTicks == 0 ||
                    item.baseRequiredTicks == 0) {
                    return false;
                }
            }
            return true;
        },
        [](foundation::CanonicalHash& hash, const ResearchQueue& value) {
            hash.WriteU32(static_cast<std::uint32_t>(value.items.size()));
            for (const auto& item : value.items) {
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
}

} // namespace rts::gameplay
