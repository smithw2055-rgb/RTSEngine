#pragma once

#include <RTSEngine/Ecs/ComponentSchema.h>
#include <RTSEngine/Ecs/World.h>
#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace rts::ecs {

class WorldArchive final {
public:
    static constexpr std::uint32_t kMagic = 0x31444c57u; // "WLD1"
    static constexpr std::uint16_t kVersion = 1u;
    static constexpr std::uint32_t kMaximumPools = 4096u;
    static constexpr std::uint32_t kMaximumComponentsPerPool = 1000000u;
    static constexpr std::uint64_t kMaximumTotalComponents = 4000000ull;

    static bool write(foundation::BinaryWriter& writer,
                      const World& world,
                      const ComponentSchemaRegistry& schemas) {
        if (!schemas.frozen()) {
            return false;
        }

        struct PoolRecord final {
            const ComponentSchemaDescriptor* schema{};
            const IComponentPool* pool{};
        };

        std::vector<PoolRecord> records;
        records.reserve(world.pools_.size());
        std::uint64_t totalComponents = 0;
        for (const auto& item : world.pools_) {
            const auto& pool = *item.second;
            if (pool.size() == 0) {
                continue;
            }
            const auto* schema = schemas.find(pool.cppType());
            if (!schema || pool.size() > kMaximumComponentsPerPool) {
                return false;
            }
            totalComponents += pool.size();
            if (totalComponents > kMaximumTotalComponents) {
                return false;
            }
            records.push_back({schema, &pool});
        }

        if (records.size() > kMaximumPools) {
            return false;
        }
        std::sort(records.begin(), records.end(),
                  [](const PoolRecord& left, const PoolRecord& right) {
                      return left.schema->typeId < right.schema->typeId;
                  });

        foundation::BinaryWriter archive;
        archive.writeU32(kMagic);
        archive.writeU16(kVersion);
        if (!world.entities_.writeState(archive)) {
            return false;
        }
        archive.writeU32(static_cast<std::uint32_t>(records.size()));

        ComponentTypeId previousTypeId = 0;
        for (const auto& record : records) {
            if (record.schema->typeId <= previousTypeId) {
                return false;
            }
            previousTypeId = record.schema->typeId;

            const auto entities = record.pool->orderedEntities();
            if (entities.size() != record.pool->size()) {
                return false;
            }

            archive.writeU32(record.schema->typeId);
            archive.writeU16(record.schema->version);
            archive.writeU32(static_cast<std::uint32_t>(entities.size()));

            Entity previousEntity{};
            bool hasPrevious = false;
            for (const auto entity : entities) {
                if (!world.entities_.alive(entity) ||
                    (hasPrevious && !(previousEntity < entity))) {
                    return false;
                }
                previousEntity = entity;
                hasPrevious = true;

                const void* value = record.pool->value(entity);
                foundation::BinaryWriter payload;
                if (!schemas.writePayload(record.pool->cppType(), payload, value) ||
                    payload.bytes().size() > ComponentSchemaRegistry::kMaximumPayloadBytes) {
                    return false;
                }

                archive.writeU32(entity.index);
                archive.writeU32(entity.generation);
                archive.writeU32(static_cast<std::uint32_t>(payload.bytes().size()));
                archive.writeBytes(payload.bytes());
            }
        }

        writer.writeBytes(archive.bytes());
        return true;
    }

    static bool read(foundation::BinaryReader& reader,
                     const ComponentSchemaRegistry& schemas,
                     World& world) {
        if (!schemas.frozen()) {
            return false;
        }

        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            magic != kMagic || version != kVersion) {
            return false;
        }

        EntityRegistryState entityState;
        if (!EntityRegistry::readState(reader, entityState)) {
            return false;
        }

        World staged;
        if (!staged.entities_.restore(std::move(entityState))) {
            return false;
        }

        std::uint32_t poolCount = 0;
        if (!reader.readU32(poolCount) || poolCount > kMaximumPools ||
            poolCount > schemas.size()) {
            return false;
        }

        ComponentTypeId previousTypeId = 0;
        std::uint64_t totalComponents = 0;
        for (std::uint32_t poolIndex = 0; poolIndex < poolCount; ++poolIndex) {
            ComponentTypeId typeId = 0;
            ComponentSchemaVersion storedVersion = 0;
            std::uint32_t componentCount = 0;
            if (!reader.readU32(typeId) || !reader.readU16(storedVersion) ||
                !reader.readU32(componentCount) || typeId <= previousTypeId ||
                storedVersion == 0 || componentCount == 0 ||
                componentCount > kMaximumComponentsPerPool ||
                componentCount >= staged.entities_.capacity()) {
                return false;
            }
            previousTypeId = typeId;
            totalComponents += componentCount;
            if (totalComponents > kMaximumTotalComponents || !schemas.find(typeId)) {
                return false;
            }

            auto pool = schemas.createPool(typeId);
            if (!pool) {
                return false;
            }

            Entity previousEntity{};
            bool hasPrevious = false;
            for (std::uint32_t componentIndex = 0;
                 componentIndex < componentCount; ++componentIndex) {
                Entity entity;
                std::uint32_t payloadSize = 0;
                if (!reader.readU32(entity.index) ||
                    !reader.readU32(entity.generation) ||
                    !reader.readU32(payloadSize) ||
                    payloadSize > ComponentSchemaRegistry::kMaximumPayloadBytes ||
                    !staged.entities_.alive(entity) ||
                    (hasPrevious && !(previousEntity < entity))) {
                    return false;
                }
                previousEntity = entity;
                hasPrevious = true;

                std::vector<std::uint8_t> payloadBytes;
                if (!reader.readBytes(
                        payloadSize, payloadBytes,
                        ComponentSchemaRegistry::kMaximumPayloadBytes)) {
                    return false;
                }
                foundation::BinaryReader payloadReader(payloadBytes);
                if (!schemas.readIntoPool(
                        typeId, storedVersion, payloadReader, entity, *pool)) {
                    return false;
                }
            }

            const auto cppType = pool->cppType();
            if (staged.pools_.find(cppType) != staged.pools_.end()) {
                return false;
            }
            staged.pools_.emplace(cppType, std::move(pool));
        }

        if (!reader.atEnd()) {
            return false;
        }
        world = std::move(staged);
        return true;
    }
};

} // namespace rts::ecs
