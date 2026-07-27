#pragma once

#include <RTSEngine/Ecs/ComponentSchema.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <cstdint>
#include <limits>

namespace rts::gameplay {

inline constexpr std::uint32_t kMaximumPersistentVectorEntries = 1000000u;

namespace persistence_detail {

inline void WriteEntity(foundation::BinaryWriter& writer, ecs::Entity entity) {
    writer.writeU32(entity.index);
    writer.writeU32(entity.generation);
}

inline bool ReadEntity(foundation::BinaryReader& reader, ecs::Entity& entity) {
    if (!reader.readU32(entity.index) || !reader.readU32(entity.generation)) {
        return false;
    }
    return (entity.index == 0 && entity.generation == 0) ||
           (entity.index != 0 && entity.generation != 0);
}

inline void HashEntity(foundation::CanonicalHash& hash, ecs::Entity entity) {
    hash.WriteU32(entity.index);
    hash.WriteU32(entity.generation);
}

inline void WriteGridPoint(
    foundation::BinaryWriter& writer,
    GridPoint point) {
    writer.writeI32(point.x);
    writer.writeI32(point.y);
}

inline bool ReadGridPoint(
    foundation::BinaryReader& reader,
    GridPoint& point) {
    return reader.readI32(point.x) && reader.readI32(point.y);
}

inline void HashGridPoint(
    foundation::CanonicalHash& hash,
    GridPoint point) {
    hash.WriteI32(point.x);
    hash.WriteI32(point.y);
}

inline void WriteCombatStats(
    foundation::BinaryWriter& writer,
    const CombatStats& value) {
    writer.writeI32(value.maximumHealth);
    writer.writeI32(value.armor);
    writer.writeI32(value.weaponDamage);
    writer.writeI32(value.weaponRange);
    writer.writeU32(value.cooldownTicks);
    writer.writeI32(value.bounty);
}

inline bool ReadCombatStats(
    foundation::BinaryReader& reader,
    CombatStats& value) {
    return reader.readI32(value.maximumHealth) &&
           reader.readI32(value.armor) &&
           reader.readI32(value.weaponDamage) &&
           reader.readI32(value.weaponRange) &&
           reader.readU32(value.cooldownTicks) &&
           reader.readI32(value.bounty) &&
           value.maximumHealth >= 0 && value.armor >= 0 &&
           value.weaponDamage >= 0 && value.weaponRange >= 0 &&
           value.cooldownTicks > 0 && value.bounty >= 0;
}

inline void HashCombatStats(
    foundation::CanonicalHash& hash,
    const CombatStats& value) {
    hash.WriteI32(value.maximumHealth);
    hash.WriteI32(value.armor);
    hash.WriteI32(value.weaponDamage);
    hash.WriteI32(value.weaponRange);
    hash.WriteU32(value.cooldownTicks);
    hash.WriteI32(value.bounty);
}

} // namespace persistence_detail

inline bool RegisterRtsComponentSchemas(
    ecs::ComponentSchemaRegistry& schemas) {
    using namespace persistence_detail;
    bool ok = true;

    ok = schemas.registerSchema<Position>(
        0x52545301u, 1u, "rts.Position",
        [](foundation::BinaryWriter& writer, const Position& value) {
            writer.writeI32(value.x);
            writer.writeI32(value.y);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           Position& value) {
            return version == 1u && reader.readI32(value.x) &&
                   reader.readI32(value.y);
        },
        [](foundation::CanonicalHash& hash, const Position& value) {
            hash.WriteI32(value.x);
            hash.WriteI32(value.y);
        }) && ok;

    ok = schemas.registerSchema<MoveSpeed>(
        0x52545302u, 1u, "rts.MoveSpeed",
        [](foundation::BinaryWriter& writer, const MoveSpeed& value) {
            writer.writeI32(value.cellsPerTick);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           MoveSpeed& value) {
            return version == 1u && reader.readI32(value.cellsPerTick) &&
                   value.cellsPerTick >= 0;
        },
        [](foundation::CanonicalHash& hash, const MoveSpeed& value) {
            hash.WriteI32(value.cellsPerTick);
        }) && ok;

    ok = schemas.registerSchema<OrderQueue>(
        0x52545303u, 1u, "rts.OrderQueue",
        [](foundation::BinaryWriter& writer, const OrderQueue& value) {
            writer.writeU32(static_cast<std::uint32_t>(value.pending.size()));
            for (const auto& order : value.pending) {
                writer.writeU8(static_cast<std::uint8_t>(order.type));
                WriteGridPoint(writer, order.target);
            }
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           OrderQueue& value) {
            std::uint32_t count = 0;
            if (version != 1u || !reader.readU32(count) ||
                count > kMaximumPersistentVectorEntries) {
                return false;
            }
            value.pending.resize(count);
            for (auto& order : value.pending) {
                std::uint8_t rawType = 0;
                if (!reader.readU8(rawType) ||
                    rawType > static_cast<std::uint8_t>(OrderType::AttackMove) ||
                    !ReadGridPoint(reader, order.target)) {
                    return false;
                }
                order.type = static_cast<OrderType>(rawType);
            }
            return true;
        },
        [](foundation::CanonicalHash& hash, const OrderQueue& value) {
            hash.WriteU32(static_cast<std::uint32_t>(value.pending.size()));
            for (const auto& order : value.pending) {
                hash.WriteU8(static_cast<std::uint8_t>(order.type));
                HashGridPoint(hash, order.target);
            }
        }) && ok;

    ok = schemas.registerSchema<MovementAgent>(
        0x52545304u, 2u, "rts.MovementAgent",
        [](foundation::BinaryWriter& writer, const MovementAgent& value) {
            writer.writeU32(static_cast<std::uint32_t>(value.path.size()));
            for (const auto point : value.path) WriteGridPoint(writer, point);
            writer.writeU64(static_cast<std::uint64_t>(value.nextPoint));
            writer.writeU64(value.pathRevision);
            WriteGridPoint(writer, value.pathGoal);
            writer.writeBool(value.hasPathGoal);
            writer.writeBool(value.combatPath);
            WriteEntity(writer, value.chaseTarget);
            WriteGridPoint(writer, value.chaseTargetPosition);
            writer.writeU32(value.blockedTicks);
            writer.writeU32(value.yieldOrdinal);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           MovementAgent& value) {
            std::uint32_t count = 0;
            std::uint64_t nextPoint = 0;
            if ((version != 1u && version != 2u) ||
                !reader.readU32(count) ||
                count > kMaximumPersistentVectorEntries) {
                return false;
            }
            value.path.resize(count);
            for (auto& point : value.path) {
                if (!ReadGridPoint(reader, point)) return false;
            }
            if (!reader.readU64(nextPoint) ||
                nextPoint > value.path.size() ||
                nextPoint > std::numeric_limits<std::size_t>::max() ||
                !reader.readU64(value.pathRevision) ||
                !ReadGridPoint(reader, value.pathGoal) ||
                !reader.readBool(value.hasPathGoal) ||
                !reader.readBool(value.combatPath) ||
                !ReadEntity(reader, value.chaseTarget) ||
                !ReadGridPoint(reader, value.chaseTargetPosition)) {
                return false;
            }
            value.nextPoint = static_cast<std::size_t>(nextPoint);
            value.flowFieldPath = false;
            value.flowContext = nullptr;
            value.flowSample = nullptr;
            value.blockedTicks = 0;
            value.yieldOrdinal = 0;
            if (version == 2u &&
                (!reader.readU32(value.blockedTicks) ||
                 !reader.readU32(value.yieldOrdinal))) {
                return false;
            }
            return true;
        },
        [](foundation::CanonicalHash& hash, const MovementAgent& value) {
            hash.WriteU32(static_cast<std::uint32_t>(value.path.size()));
            for (const auto point : value.path) HashGridPoint(hash, point);
            hash.WriteU64(static_cast<std::uint64_t>(value.nextPoint));
            hash.WriteU64(value.pathRevision);
            HashGridPoint(hash, value.pathGoal);
            hash.WriteBool(value.hasPathGoal);
            hash.WriteBool(value.combatPath);
            HashEntity(hash, value.chaseTarget);
            HashGridPoint(hash, value.chaseTargetPosition);
            hash.WriteU32(value.blockedTicks);
            hash.WriteU32(value.yieldOrdinal);
        }) && ok;

    ok = schemas.registerSchema<Team>(
        0x52545305u, 1u, "rts.Team",
        [](foundation::BinaryWriter& writer, const Team& value) {
            writer.writeU32(value.id);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           Team& value) {
            return version == 1u && reader.readU32(value.id);
        },
        [](foundation::CanonicalHash& hash, const Team& value) {
            hash.WriteU32(value.id);
        }) && ok;

    ok = schemas.registerSchema<TunableStats>(
        0x52545306u, 1u, "rts.TunableStats",
        [](foundation::BinaryWriter& writer, const TunableStats& value) {
            writer.writeBool(value.building);
            writer.writeI32(value.baseMoveSpeed);
            WriteCombatStats(writer, value.baseCombat);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           TunableStats& value) {
            return version == 1u && reader.readBool(value.building) &&
                   reader.readI32(value.baseMoveSpeed) &&
                   value.baseMoveSpeed >= 0 &&
                   ReadCombatStats(reader, value.baseCombat);
        },
        [](foundation::CanonicalHash& hash, const TunableStats& value) {
            hash.WriteBool(value.building);
            hash.WriteI32(value.baseMoveSpeed);
            HashCombatStats(hash, value.baseCombat);
        }) && ok;

    ok = schemas.registerSchema<Health>(
        0x52545307u, 1u, "rts.Health",
        [](foundation::BinaryWriter& writer, const Health& value) {
            writer.writeI32(value.current);
            writer.writeI32(value.maximum);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           Health& value) {
            return version == 1u && reader.readI32(value.current) &&
                   reader.readI32(value.maximum) && value.maximum > 0 &&
                   value.current >= 0 && value.current <= value.maximum;
        },
        [](foundation::CanonicalHash& hash, const Health& value) {
            hash.WriteI32(value.current);
            hash.WriteI32(value.maximum);
        }) && ok;

    ok = schemas.registerSchema<Armor>(
        0x52545308u, 1u, "rts.Armor",
        [](foundation::BinaryWriter& writer, const Armor& value) {
            writer.writeI32(value.value);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           Armor& value) {
            return version == 1u && reader.readI32(value.value) &&
                   value.value >= 0;
        },
        [](foundation::CanonicalHash& hash, const Armor& value) {
            hash.WriteI32(value.value);
        }) && ok;

    ok = schemas.registerSchema<Weapon>(
        0x52545309u, 1u, "rts.Weapon",
        [](foundation::BinaryWriter& writer, const Weapon& value) {
            writer.writeI32(value.damage);
            writer.writeI32(value.range);
            writer.writeU32(value.cooldownTicks);
            writer.writeU32(value.cooldownRemaining);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           Weapon& value) {
            return version == 1u && reader.readI32(value.damage) &&
                   reader.readI32(value.range) &&
                   reader.readU32(value.cooldownTicks) &&
                   reader.readU32(value.cooldownRemaining) &&
                   value.damage >= 0 && value.range >= 0 &&
                   value.cooldownTicks > 0 &&
                   value.cooldownRemaining <= value.cooldownTicks;
        },
        [](foundation::CanonicalHash& hash, const Weapon& value) {
            hash.WriteI32(value.damage);
            hash.WriteI32(value.range);
            hash.WriteU32(value.cooldownTicks);
            hash.WriteU32(value.cooldownRemaining);
        }) && ok;

    ok = schemas.registerSchema<CombatTarget>(
        0x5254530au, 1u, "rts.CombatTarget",
        [](foundation::BinaryWriter& writer, const CombatTarget& value) {
            WriteEntity(writer, value.entity);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           CombatTarget& value) {
            return version == 1u && ReadEntity(reader, value.entity);
        },
        [](foundation::CanonicalHash& hash, const CombatTarget& value) {
            HashEntity(hash, value.entity);
        }) && ok;

    ok = schemas.registerSchema<CombatDirective>(
        0x5254530bu, 1u, "rts.CombatDirective",
        [](foundation::BinaryWriter& writer, const CombatDirective& value) {
            writer.writeU8(static_cast<std::uint8_t>(value.mode));
            WriteEntity(writer, value.forcedTarget);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           CombatDirective& value) {
            std::uint8_t rawMode = 0;
            if (version != 1u || !reader.readU8(rawMode) ||
                rawMode > static_cast<std::uint8_t>(CombatMode::HoldPosition) ||
                !ReadEntity(reader, value.forcedTarget)) {
                return false;
            }
            value.mode = static_cast<CombatMode>(rawMode);
            return true;
        },
        [](foundation::CanonicalHash& hash, const CombatDirective& value) {
            hash.WriteU8(static_cast<std::uint8_t>(value.mode));
            HashEntity(hash, value.forcedTarget);
        }) && ok;

    ok = schemas.registerSchema<Bounty>(
        0x5254530cu, 1u, "rts.Bounty",
        [](foundation::BinaryWriter& writer, const Bounty& value) {
            writer.writeI32(value.amount);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           Bounty& value) {
            return version == 1u && reader.readI32(value.amount) &&
                   value.amount >= 0;
        },
        [](foundation::CanonicalHash& hash, const Bounty& value) {
            hash.WriteI32(value.amount);
        }) && ok;

    ok = schemas.registerSchema<BuildingFootprint>(
        0x5254530du, 1u, "rts.BuildingFootprint",
        [](foundation::BinaryWriter& writer, const BuildingFootprint& value) {
            WriteGridPoint(writer, value.origin);
            writer.writeI32(value.width);
            writer.writeI32(value.height);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           BuildingFootprint& value) {
            return version == 1u && ReadGridPoint(reader, value.origin) &&
                   reader.readI32(value.width) &&
                   reader.readI32(value.height) && value.width > 0 &&
                   value.height > 0;
        },
        [](foundation::CanonicalHash& hash, const BuildingFootprint& value) {
            HashGridPoint(hash, value.origin);
            hash.WriteI32(value.width);
            hash.WriteI32(value.height);
        }) && ok;

    ok = schemas.registerSchema<ConstructionSite>(
        0x5254530eu, 1u, "rts.ConstructionSite",
        [](foundation::BinaryWriter& writer, const ConstructionSite& value) {
            writer.writeU32(value.id);
            writer.writeU32(value.definitionId);
            writer.writeI32(value.reservedCost);
            writer.writeU32(value.progressTicks);
            writer.writeU32(value.requiredTicks);
            writer.writeBool(value.producer);
            writer.writeU32(value.ownerTeam);
            writer.writeU32(value.baseRequiredTicks);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           ConstructionSite& value) {
            return version == 1u && reader.readU32(value.id) &&
                   reader.readU32(value.definitionId) &&
                   reader.readI32(value.reservedCost) &&
                   reader.readU32(value.progressTicks) &&
                   reader.readU32(value.requiredTicks) &&
                   reader.readBool(value.producer) &&
                   reader.readU32(value.ownerTeam) &&
                   reader.readU32(value.baseRequiredTicks) && value.id != 0 &&
                   value.definitionId != 0 && value.reservedCost >= 0 &&
                   value.requiredTicks > 0 && value.baseRequiredTicks > 0;
        },
        [](foundation::CanonicalHash& hash, const ConstructionSite& value) {
            hash.WriteU32(value.id);
            hash.WriteU32(value.definitionId);
            hash.WriteI32(value.reservedCost);
            hash.WriteU32(value.progressTicks);
            hash.WriteU32(value.requiredTicks);
            hash.WriteBool(value.producer);
            hash.WriteU32(value.ownerTeam);
            hash.WriteU32(value.baseRequiredTicks);
        }) && ok;

    ok = schemas.registerSchema<Building>(
        0x5254530fu, 1u, "rts.Building",
        [](foundation::BinaryWriter& writer, const Building& value) {
            writer.writeU32(value.definitionId);
            writer.writeBool(value.producer);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           Building& value) {
            return version == 1u && reader.readU32(value.definitionId) &&
                   reader.readBool(value.producer) && value.definitionId != 0;
        },
        [](foundation::CanonicalHash& hash, const Building& value) {
            hash.WriteU32(value.definitionId);
            hash.WriteBool(value.producer);
        }) && ok;

    ok = schemas.registerSchema<ProductionQueue>(
        0x52545310u, 2u, "rts.ProductionQueue",
        [](foundation::BinaryWriter& writer, const ProductionQueue& value) {
            writer.writeU32(static_cast<std::uint32_t>(value.items.size()));
            for (const auto& item : value.items) {
                writer.writeU32(item.id);
                writer.writeU32(item.unitDefinitionId);
                writer.writeI32(item.reservedCost);
                writer.writeU32(item.supplyCost);
                writer.writeU32(item.progressTicks);
                writer.writeU32(item.requiredTicks);
                writer.writeU32(item.baseRequiredTicks);
            }
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           ProductionQueue& value) {
            std::uint32_t count = 0;
            if ((version != 1u && version != 2u) ||
                !reader.readU32(count) ||
                count > kMaximumPersistentVectorEntries) {
                return false;
            }
            value.items.resize(count);
            for (auto& item : value.items) {
                if (!reader.readU32(item.id) ||
                    !reader.readU32(item.unitDefinitionId) ||
                    !reader.readI32(item.reservedCost)) {
                    return false;
                }
                item.supplyCost = 0;
                if (version >= 2u && !reader.readU32(item.supplyCost)) {
                    return false;
                }
                if (!reader.readU32(item.progressTicks) ||
                    !reader.readU32(item.requiredTicks) ||
                    !reader.readU32(item.baseRequiredTicks) || item.id == 0 ||
                    item.unitDefinitionId == 0 || item.reservedCost < 0 ||
                    item.requiredTicks == 0 || item.baseRequiredTicks == 0) {
                    return false;
                }
            }
            return true;
        },
        [](foundation::CanonicalHash& hash, const ProductionQueue& value) {
            hash.WriteU32(static_cast<std::uint32_t>(value.items.size()));
            for (const auto& item : value.items) {
                hash.WriteU32(item.id);
                hash.WriteU32(item.unitDefinitionId);
                hash.WriteI32(item.reservedCost);
                hash.WriteU32(item.supplyCost);
                hash.WriteU32(item.progressTicks);
                hash.WriteU32(item.requiredTicks);
                hash.WriteU32(item.baseRequiredTicks);
            }
        }) && ok;

    ok = schemas.registerSchema<RallyPoint>(
        0x52545311u, 1u, "rts.RallyPoint",
        [](foundation::BinaryWriter& writer, const RallyPoint& value) {
            WriteGridPoint(writer, value.point);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           RallyPoint& value) {
            return version == 1u && ReadGridPoint(reader, value.point);
        },
        [](foundation::CanonicalHash& hash, const RallyPoint& value) {
            HashGridPoint(hash, value.point);
        }) && ok;

    ok = schemas.registerSchema<UnitSupply>(
        0x52545313u, 1u, "rts.UnitSupply",
        [](foundation::BinaryWriter& writer, const UnitSupply& value) {
            writer.writeU32(value.amount);
        },
        [](foundation::BinaryReader& reader,
           ecs::ComponentSchemaVersion version,
           UnitSupply& value) {
            return version == 1u && reader.readU32(value.amount);
        },
        [](foundation::CanonicalHash& hash, const UnitSupply& value) {
            hash.WriteU32(value.amount);
        }) && ok;

    if (ok) schemas.freeze();
    return ok;
}

} // namespace rts::gameplay
