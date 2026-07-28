#pragma once

#include <RTSEngine/Ecs/ComponentSchema.h>
#include <RTSEngine/Rts/Harvesting.h>
#include <RTSEngine/Rts/TeamEconomy.h>

#include <cstdint>
#include <limits>

namespace rts::gameplay {

namespace team_economy_persistence_detail {

inline void WriteEntity(
    foundation::BinaryWriter& writer,
    ecs::Entity entity) {
    writer.writeU32(entity.index);
    writer.writeU32(entity.generation);
}

inline bool ReadEntity(
    foundation::BinaryReader& reader,
    ecs::Entity& entity) {
    if (!reader.readU32(entity.index) ||
        !reader.readU32(entity.generation)) {
        return false;
    }
    return (entity.index == 0 && entity.generation == 0) ||
           (entity.index != 0 && entity.generation != 0);
}

inline void HashEntity(
    foundation::CanonicalHash& hash,
    ecs::Entity entity) {
    hash.WriteU32(entity.index);
    hash.WriteU32(entity.generation);
}

inline void WriteAmount(
    foundation::BinaryWriter& writer,
    ResourceAmount amount) {
    writer.writeU64(static_cast<std::uint64_t>(amount));
}

inline bool ReadAmount(
    foundation::BinaryReader& reader,
    ResourceAmount& amount) {
    std::uint64_t raw = 0;
    if (!reader.readU64(raw) ||
        raw > static_cast<std::uint64_t>(
            std::numeric_limits<ResourceAmount>::max())) {
        return false;
    }
    amount = static_cast<ResourceAmount>(raw);
    return true;
}

inline void HashAmount(
    foundation::CanonicalHash& hash,
    ResourceAmount amount) {
    hash.WriteU64(static_cast<std::uint64_t>(amount));
}

} // namespace team_economy_persistence_detail

inline bool RegisterTeamEconomyComponentSchemas(
    ecs::ComponentSchemaRegistry& schemas) {
    using namespace team_economy_persistence_detail;
    bool ok = true;

    ok = schemas.registerSchema<UnitArchetype>(
        0x52545313u, 1u, "rts.UnitArchetype",
        [](foundation::BinaryWriter& writer, const UnitArchetype& value) {
            writer.writeU32(value.definitionId);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           UnitArchetype& value) {
            return version == 1u &&
                   reader.readU32(value.definitionId) &&
                   value.definitionId != 0;
        },
        [](foundation::CanonicalHash& hash, const UnitArchetype& value) {
            hash.WriteU32(value.definitionId);
        }) && ok;

    ok = schemas.registerSchema<ResourceNode>(
        0x52545314u, 1u, "rts.ResourceNode",
        [](foundation::BinaryWriter& writer, const ResourceNode& value) {
            writer.writeU32(value.id);
            writer.writeU32(value.resourceType);
            WriteAmount(writer, value.remaining);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           ResourceNode& value) {
            return version == 1u && reader.readU32(value.id) &&
                   reader.readU32(value.resourceType) &&
                   ReadAmount(reader, value.remaining) &&
                   value.id != 0 && value.resourceType != 0;
        },
        [](foundation::CanonicalHash& hash, const ResourceNode& value) {
            hash.WriteU32(value.id);
            hash.WriteU32(value.resourceType);
            HashAmount(hash, value.remaining);
        }) && ok;

    ok = schemas.registerSchema<WorkerHarvester>(
        0x52545315u, 1u, "rts.WorkerHarvester",
        [](foundation::BinaryWriter& writer, const WorkerHarvester& value) {
            WriteAmount(writer, value.cargoCapacity);
            WriteAmount(writer, value.harvestAmount);
            writer.writeU32(value.harvestTicks);
            WriteAmount(writer, value.cargo);
            writer.writeU32(value.cargoType);
            WriteEntity(writer, value.targetNode);
            writer.writeU32(value.progressTicks);
            writer.writeU8(static_cast<std::uint8_t>(value.state));
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           WorkerHarvester& value) {
            std::uint8_t rawState = 0;
            if (version != 1u ||
                !ReadAmount(reader, value.cargoCapacity) ||
                !ReadAmount(reader, value.harvestAmount) ||
                !reader.readU32(value.harvestTicks) ||
                !ReadAmount(reader, value.cargo) ||
                !reader.readU32(value.cargoType) ||
                !ReadEntity(reader, value.targetNode) ||
                !reader.readU32(value.progressTicks) ||
                !reader.readU8(rawState) ||
                rawState > static_cast<std::uint8_t>(
                    HarvestState::MovingToDropOff) ||
                value.harvestTicks == 0 ||
                value.cargo > value.cargoCapacity ||
                (value.cargo == 0 && value.cargoType != 0) ||
                (value.cargo > 0 && value.cargoType == 0)) {
                return false;
            }
            value.state = static_cast<HarvestState>(rawState);
            return true;
        },
        [](foundation::CanonicalHash& hash, const WorkerHarvester& value) {
            HashAmount(hash, value.cargoCapacity);
            HashAmount(hash, value.harvestAmount);
            hash.WriteU32(value.harvestTicks);
            HashAmount(hash, value.cargo);
            hash.WriteU32(value.cargoType);
            HashEntity(hash, value.targetNode);
            hash.WriteU32(value.progressTicks);
            hash.WriteU8(static_cast<std::uint8_t>(value.state));
        }) && ok;

    ok = schemas.registerSchema<ResourceDropOff>(
        0x52545316u, 1u, "rts.ResourceDropOff",
        [](foundation::BinaryWriter& writer, const ResourceDropOff& value) {
            writer.writeU32(value.resourceType);
            writer.writeI32(value.accessX);
            writer.writeI32(value.accessY);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           ResourceDropOff& value) {
            return version == 1u &&
                   reader.readU32(value.resourceType) &&
                   reader.readI32(value.accessX) &&
                   reader.readI32(value.accessY) &&
                   value.resourceType != 0;
        },
        [](foundation::CanonicalHash& hash, const ResourceDropOff& value) {
            hash.WriteU32(value.resourceType);
            hash.WriteI32(value.accessX);
            hash.WriteI32(value.accessY);
        }) && ok;

    ok = schemas.registerSchema<SupplyProvider>(
        0x52545317u, 1u, "rts.SupplyProvider",
        [](foundation::BinaryWriter& writer, const SupplyProvider& value) {
            writer.writeU32(value.capacity);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           SupplyProvider& value) {
            return version == 1u &&
                   reader.readU32(value.capacity) &&
                   value.capacity != 0;
        },
        [](foundation::CanonicalHash& hash, const SupplyProvider& value) {
            hash.WriteU32(value.capacity);
        }) && ok;

    ok = schemas.registerSchema<ConstructionEconomyFeatures>(
        0x52545318u, 1u, "rts.ConstructionEconomyFeatures",
        [](foundation::BinaryWriter& writer,
           const ConstructionEconomyFeatures& value) {
            writer.writeU32(value.dropOffResourceType);
            writer.writeI32(value.dropOffAccessX);
            writer.writeI32(value.dropOffAccessY);
            writer.writeU32(value.supplyProvided);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           ConstructionEconomyFeatures& value) {
            return version == 1u &&
                   reader.readU32(value.dropOffResourceType) &&
                   reader.readI32(value.dropOffAccessX) &&
                   reader.readI32(value.dropOffAccessY) &&
                   reader.readU32(value.supplyProvided) &&
                   (value.dropOffResourceType != 0 ||
                    value.supplyProvided != 0);
        },
        [](foundation::CanonicalHash& hash,
           const ConstructionEconomyFeatures& value) {
            hash.WriteU32(value.dropOffResourceType);
            hash.WriteI32(value.dropOffAccessX);
            hash.WriteI32(value.dropOffAccessY);
            hash.WriteU32(value.supplyProvided);
        }) && ok;

    return ok;
}

} // namespace rts::gameplay
