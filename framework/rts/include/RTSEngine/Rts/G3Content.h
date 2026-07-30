#pragma once

#include <RTSEngine/Assets/CookedAsset.h>
#include <RTSEngine/Rts/G3GameSession.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct G3ContentBundle final {
    std::vector<ProjectileDefinition> projectiles;
    std::vector<StatusEffectDefinition> statuses;
    std::vector<AbilityDefinition> abilities;
    std::vector<ProjectileBinding> unitProjectileBindings;
    std::vector<ProjectileBinding> buildingProjectileBindings;
};

class G3ContentCodec final {
public:
    static constexpr std::uint32_t kMagic = 0x31433347u; // G3C1
    static constexpr std::uint16_t kVersion = 1u;
    static constexpr std::uint32_t kSchemaVersion = 1u;
    static constexpr std::uint32_t kMaximumDefinitions = 65'536u;
    static constexpr std::uint32_t kMaximumEffectsPerAbility = 32u;

    static bool canonicalize(G3ContentBundle& value) {
        sortById(value.projectiles);
        sortById(value.statuses);
        sortById(value.abilities);
        sortBindings(value.unitProjectileBindings);
        sortBindings(value.buildingProjectileBindings);
        return validateCanonical(value);
    }

    static bool validate(G3ContentBundle value) {
        return canonicalize(value);
    }

    static std::uint64_t canonicalHash(G3ContentBundle value) {
        if (!canonicalize(value)) return 0;
        foundation::CanonicalHash hash;
        hash.WriteString("rts.g3-content-bundle.v1");
        appendHash(hash, value);
        return hash.Value();
    }

    static std::vector<std::uint8_t> encode(G3ContentBundle value) {
        if (!canonicalize(value)) return {};
        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU64(canonicalHash(value));
        writeProjectiles(writer, value.projectiles);
        writeStatuses(writer, value.statuses);
        writeAbilities(writer, value.abilities);
        writeBindings(writer, value.unitProjectileBindings);
        writeBindings(writer, value.buildingProjectileBindings);
        return writer.take();
    }

    static bool decode(
        const std::vector<std::uint8_t>& bytes,
        G3ContentBundle& output) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint64_t storedHash = 0;
        G3ContentBundle candidate;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU64(storedHash) || magic != kMagic ||
            version != kVersion ||
            !readProjectiles(reader, candidate.projectiles) ||
            !readStatuses(reader, candidate.statuses) ||
            !readAbilities(reader, candidate.abilities) ||
            !readBindings(reader, candidate.unitProjectileBindings) ||
            !readBindings(reader, candidate.buildingProjectileBindings) ||
            !reader.atEnd() || !canonicalize(candidate) ||
            canonicalHash(candidate) != storedHash) {
            return false;
        }
        output = std::move(candidate);
        return true;
    }

    // G3 content currently uses the generic Binary cooked-asset envelope so
    // existing AssetType values and manifests remain backward compatible.
    static assets::CookedAsset cookedAsset(
        std::uint64_t assetId,
        G3ContentBundle value) {
        assets::CookedAsset result;
        result.key = {assets::AssetType::Binary, assetId};
        result.schemaVersion = kSchemaVersion;
        result.payload = encode(std::move(value));
        if (!result.key.valid() || result.payload.empty() ||
            !assets::CookedAssetCodec::canonicalize(result)) {
            return {};
        }
        return result;
    }

    static bool decodeCookedAsset(
        const assets::CookedAsset& asset,
        G3ContentBundle& output) {
        return asset.key.type == assets::AssetType::Binary &&
               asset.key.id != 0 &&
               asset.schemaVersion == kSchemaVersion &&
               asset.payloadHash ==
                   assets::CookedAssetCodec::payloadHash(asset.payload) &&
               decode(asset.payload, output);
    }

    static bool apply(
        RtsG3GameSession& session,
        G3ContentBundle value) {
        if (!canonicalize(value)) return false;
        for (const auto& definition : value.projectiles) {
            if (!session.registerProjectile(definition)) return false;
        }
        for (const auto& definition : value.statuses) {
            if (!session.registerStatusEffect(definition)) return false;
        }
        for (const auto& definition : value.abilities) {
            if (!session.registerAbility(definition)) return false;
        }
        for (const auto& binding : value.unitProjectileBindings) {
            if (!session.bindUnitProjectile(
                    binding.definitionId,
                    binding.projectileDefinitionId)) {
                return false;
            }
        }
        for (const auto& binding : value.buildingProjectileBindings) {
            if (!session.bindBuildingProjectile(
                    binding.definitionId,
                    binding.projectileDefinitionId)) {
                return false;
            }
        }
        return true;
    }

private:
    template<class Definition>
    static void sortById(std::vector<Definition>& values) {
        std::sort(
            values.begin(), values.end(),
            [](const Definition& first, const Definition& second) {
                return first.id < second.id;
            });
    }

    static void sortBindings(std::vector<ProjectileBinding>& values) {
        std::sort(
            values.begin(), values.end(),
            [](const ProjectileBinding& first,
               const ProjectileBinding& second) {
                return first.definitionId < second.definitionId;
            });
    }

    template<class Definition>
    static bool uniqueNonZeroIds(const std::vector<Definition>& values) {
        if (values.size() > kMaximumDefinitions) return false;
        std::uint32_t previous = 0;
        for (const auto& value : values) {
            if (value.id == 0 || value.id <= previous) return false;
            previous = value.id;
        }
        return true;
    }

    static bool containsProjectile(
        const G3ContentBundle& value,
        std::uint32_t id) {
        if (id == 0) return false;
        const auto found = std::lower_bound(
            value.projectiles.begin(), value.projectiles.end(), id,
            [](const ProjectileDefinition& current, std::uint32_t key) {
                return current.id < key;
            });
        return found != value.projectiles.end() && found->id == id;
    }

    static bool containsStatus(
        const G3ContentBundle& value,
        std::uint32_t id) {
        if (id == 0) return false;
        const auto found = std::lower_bound(
            value.statuses.begin(), value.statuses.end(), id,
            [](const StatusEffectDefinition& current, std::uint32_t key) {
                return current.id < key;
            });
        return found != value.statuses.end() && found->id == id;
    }

    static bool validateBindings(
        const G3ContentBundle& value,
        const std::vector<ProjectileBinding>& bindings) {
        if (bindings.size() > kMaximumDefinitions) return false;
        std::uint32_t previous = 0;
        for (const auto& binding : bindings) {
            if (binding.definitionId == 0 ||
                binding.definitionId <= previous ||
                !containsProjectile(
                    value, binding.projectileDefinitionId)) {
                return false;
            }
            previous = binding.definitionId;
        }
        return true;
    }

    static bool validateCanonical(const G3ContentBundle& value) {
        if (!uniqueNonZeroIds(value.projectiles) ||
            !uniqueNonZeroIds(value.statuses) ||
            !uniqueNonZeroIds(value.abilities) ||
            !validateBindings(value, value.unitProjectileBindings) ||
            !validateBindings(value, value.buildingProjectileBindings)) {
            return false;
        }
        for (const auto& definition : value.projectiles) {
            if (definition.speedQ16 == 0 ||
                definition.lifetimeTicks == 0 ||
                definition.damage < 0 ||
                static_cast<std::uint8_t>(definition.damageType) >
                    static_cast<std::uint8_t>(
                        G3DamageType::TrueDamage) ||
                (definition.statusEffectId != 0 &&
                 !containsStatus(value, definition.statusEffectId))) {
                return false;
            }
        }
        for (const auto& definition : value.statuses) {
            if (definition.durationTicks == 0 ||
                definition.maxStacks == 0 ||
                definition.moveScalePermille > 4000 ||
                definition.damageScalePermille > 4000 ||
                static_cast<std::uint8_t>(definition.stacking) >
                    static_cast<std::uint8_t>(
                        StatusStackingPolicy::Independent) ||
                static_cast<std::uint8_t>(
                    definition.periodicDamageType) >
                    static_cast<std::uint8_t>(
                        G3DamageType::TrueDamage)) {
                return false;
            }
        }
        for (const auto& definition : value.abilities) {
            if (definition.cooldownTicks == 0 ||
                definition.effects.empty() ||
                definition.effects.size() >
                    kMaximumEffectsPerAbility ||
                static_cast<std::uint8_t>(definition.targetKind) >
                    static_cast<std::uint8_t>(
                        AbilityTargetKind::Point)) {
                return false;
            }
            for (const auto& effect : definition.effects) {
                if (static_cast<std::uint8_t>(effect.kind) >
                    static_cast<std::uint8_t>(
                        AbilityEffectKind::SpawnProjectile)) {
                    return false;
                }
                if (effect.kind ==
                        AbilityEffectKind::SpawnProjectile &&
                    !containsProjectile(
                        value, effect.projectileDefinitionId)) {
                    return false;
                }
                if (effect.kind ==
                        AbilityEffectKind::ApplyStatus &&
                    !containsStatus(value, effect.statusEffectId)) {
                    return false;
                }
            }
        }
        return true;
    }

    static void writeProjectiles(
        foundation::BinaryWriter& writer,
        const std::vector<ProjectileDefinition>& values) {
        writer.writeU32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) {
            writer.writeU32(value.id);
            writer.writeU32(value.speedQ16);
            writer.writeU32(value.lifetimeTicks);
            writer.writeU32(value.hitRadiusQ16);
            writer.writeI32(value.damage);
            writer.writeU8(static_cast<std::uint8_t>(value.damageType));
            writer.writeU32(value.splashRadiusQ16);
            writer.writeBool(value.homing);
            writer.writeBool(value.friendlyFire);
            writer.writeU32(value.statusEffectId);
        }
    }

    static bool readProjectiles(
        foundation::BinaryReader& reader,
        std::vector<ProjectileDefinition>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) ||
            count > kMaximumDefinitions) {
            return false;
        }
        values.resize(count);
        for (auto& value : values) {
            std::uint8_t damageType = 0;
            if (!reader.readU32(value.id) ||
                !reader.readU32(value.speedQ16) ||
                !reader.readU32(value.lifetimeTicks) ||
                !reader.readU32(value.hitRadiusQ16) ||
                !reader.readI32(value.damage) ||
                !reader.readU8(damageType) ||
                !reader.readU32(value.splashRadiusQ16) ||
                !reader.readBool(value.homing) ||
                !reader.readBool(value.friendlyFire) ||
                !reader.readU32(value.statusEffectId) ||
                damageType >
                    static_cast<std::uint8_t>(
                        G3DamageType::TrueDamage)) {
                return false;
            }
            value.damageType =
                static_cast<G3DamageType>(damageType);
        }
        return true;
    }

    static void writeStatuses(
        foundation::BinaryWriter& writer,
        const std::vector<StatusEffectDefinition>& values) {
        writer.writeU32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) {
            writer.writeU32(value.id);
            writer.writeU32(value.durationTicks);
            writer.writeU32(value.periodTicks);
            writer.writeU16(value.maxStacks);
            writer.writeU8(static_cast<std::uint8_t>(value.stacking));
            writer.writeI32(value.periodicHealthDelta);
            writer.writeU8(
                static_cast<std::uint8_t>(
                    value.periodicDamageType));
            writer.writeU16(value.moveScalePermille);
            writer.writeU16(value.damageScalePermille);
            writer.writeI32(value.armorAdd);
            writer.writeBool(value.stunned);
        }
    }

    static bool readStatuses(
        foundation::BinaryReader& reader,
        std::vector<StatusEffectDefinition>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) ||
            count > kMaximumDefinitions) {
            return false;
        }
        values.resize(count);
        for (auto& value : values) {
            std::uint8_t stacking = 0;
            std::uint8_t damageType = 0;
            if (!reader.readU32(value.id) ||
                !reader.readU32(value.durationTicks) ||
                !reader.readU32(value.periodTicks) ||
                !reader.readU16(value.maxStacks) ||
                !reader.readU8(stacking) ||
                !reader.readI32(value.periodicHealthDelta) ||
                !reader.readU8(damageType) ||
                !reader.readU16(value.moveScalePermille) ||
                !reader.readU16(value.damageScalePermille) ||
                !reader.readI32(value.armorAdd) ||
                !reader.readBool(value.stunned) ||
                stacking >
                    static_cast<std::uint8_t>(
                        StatusStackingPolicy::Independent) ||
                damageType >
                    static_cast<std::uint8_t>(
                        G3DamageType::TrueDamage)) {
                return false;
            }
            value.stacking =
                static_cast<StatusStackingPolicy>(stacking);
            value.periodicDamageType =
                static_cast<G3DamageType>(damageType);
        }
        return true;
    }

    static void writeAbilities(
        foundation::BinaryWriter& writer,
        const std::vector<AbilityDefinition>& values) {
        writer.writeU32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) {
            writer.writeU32(value.id);
            writer.writeU32(value.cooldownTicks);
            writer.writeU32(value.castTicks);
            writer.writeU32(value.rangeQ16);
            writer.writeU8(
                static_cast<std::uint8_t>(value.targetKind));
            writer.writeBool(value.targetAllies);
            writer.writeBool(value.targetEnemies);
            writer.writeU32(
                static_cast<std::uint32_t>(
                    value.effects.size()));
            for (const auto& effect : value.effects) {
                writer.writeU8(
                    static_cast<std::uint8_t>(effect.kind));
                writer.writeI32(effect.amount);
                writer.writeU32(effect.radiusQ16);
                writer.writeU8(
                    static_cast<std::uint8_t>(
                        effect.damageType));
                writer.writeU32(
                    effect.projectileDefinitionId);
                writer.writeU32(effect.statusEffectId);
            }
        }
    }

    static bool readAbilities(
        foundation::BinaryReader& reader,
        std::vector<AbilityDefinition>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) ||
            count > kMaximumDefinitions) {
            return false;
        }
        values.resize(count);
        for (auto& value : values) {
            std::uint8_t targetKind = 0;
            std::uint32_t effectCount = 0;
            if (!reader.readU32(value.id) ||
                !reader.readU32(value.cooldownTicks) ||
                !reader.readU32(value.castTicks) ||
                !reader.readU32(value.rangeQ16) ||
                !reader.readU8(targetKind) ||
                !reader.readBool(value.targetAllies) ||
                !reader.readBool(value.targetEnemies) ||
                !reader.readU32(effectCount) ||
                targetKind >
                    static_cast<std::uint8_t>(
                        AbilityTargetKind::Point) ||
                effectCount == 0 ||
                effectCount > kMaximumEffectsPerAbility) {
                return false;
            }
            value.targetKind =
                static_cast<AbilityTargetKind>(targetKind);
            value.effects.resize(effectCount);
            for (auto& effect : value.effects) {
                std::uint8_t kind = 0;
                std::uint8_t damageType = 0;
                if (!reader.readU8(kind) ||
                    !reader.readI32(effect.amount) ||
                    !reader.readU32(effect.radiusQ16) ||
                    !reader.readU8(damageType) ||
                    !reader.readU32(
                        effect.projectileDefinitionId) ||
                    !reader.readU32(effect.statusEffectId) ||
                    kind >
                        static_cast<std::uint8_t>(
                            AbilityEffectKind::SpawnProjectile) ||
                    damageType >
                        static_cast<std::uint8_t>(
                            G3DamageType::TrueDamage)) {
                    return false;
                }
                effect.kind =
                    static_cast<AbilityEffectKind>(kind);
                effect.damageType =
                    static_cast<G3DamageType>(damageType);
            }
        }
        return true;
    }

    static void writeBindings(
        foundation::BinaryWriter& writer,
        const std::vector<ProjectileBinding>& values) {
        writer.writeU32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) {
            writer.writeU32(value.definitionId);
            writer.writeU32(value.projectileDefinitionId);
        }
    }

    static bool readBindings(
        foundation::BinaryReader& reader,
        std::vector<ProjectileBinding>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) ||
            count > kMaximumDefinitions) {
            return false;
        }
        values.resize(count);
        for (auto& value : values) {
            if (!reader.readU32(value.definitionId) ||
                !reader.readU32(
                    value.projectileDefinitionId)) {
                return false;
            }
        }
        return true;
    }

    static void appendHash(
        foundation::CanonicalHash& hash,
        const G3ContentBundle& value) {
        hash.WriteU32(
            static_cast<std::uint32_t>(
                value.projectiles.size()));
        for (const auto& item : value.projectiles) {
            hash.WriteU32(item.id);
            hash.WriteU32(item.speedQ16);
            hash.WriteU32(item.lifetimeTicks);
            hash.WriteU32(item.hitRadiusQ16);
            hash.WriteI32(item.damage);
            hash.WriteU8(
                static_cast<std::uint8_t>(item.damageType));
            hash.WriteU32(item.splashRadiusQ16);
            hash.WriteBool(item.homing);
            hash.WriteBool(item.friendlyFire);
            hash.WriteU32(item.statusEffectId);
        }
        hash.WriteU32(
            static_cast<std::uint32_t>(
                value.statuses.size()));
        for (const auto& item : value.statuses) {
            hash.WriteU32(item.id);
            hash.WriteU32(item.durationTicks);
            hash.WriteU32(item.periodTicks);
            hash.WriteU16(item.maxStacks);
            hash.WriteU8(
                static_cast<std::uint8_t>(item.stacking));
            hash.WriteI32(item.periodicHealthDelta);
            hash.WriteU8(
                static_cast<std::uint8_t>(
                    item.periodicDamageType));
            hash.WriteU16(item.moveScalePermille);
            hash.WriteU16(item.damageScalePermille);
            hash.WriteI32(item.armorAdd);
            hash.WriteBool(item.stunned);
        }
        hash.WriteU32(
            static_cast<std::uint32_t>(
                value.abilities.size()));
        for (const auto& item : value.abilities) {
            hash.WriteU32(item.id);
            hash.WriteU32(item.cooldownTicks);
            hash.WriteU32(item.castTicks);
            hash.WriteU32(item.rangeQ16);
            hash.WriteU8(
                static_cast<std::uint8_t>(item.targetKind));
            hash.WriteBool(item.targetAllies);
            hash.WriteBool(item.targetEnemies);
            hash.WriteU32(
                static_cast<std::uint32_t>(
                    item.effects.size()));
            for (const auto& effect : item.effects) {
                hash.WriteU8(
                    static_cast<std::uint8_t>(effect.kind));
                hash.WriteI32(effect.amount);
                hash.WriteU32(effect.radiusQ16);
                hash.WriteU8(
                    static_cast<std::uint8_t>(
                        effect.damageType));
                hash.WriteU32(
                    effect.projectileDefinitionId);
                hash.WriteU32(effect.statusEffectId);
            }
        }
        appendBindingHash(hash, value.unitProjectileBindings);
        appendBindingHash(hash, value.buildingProjectileBindings);
    }

    static void appendBindingHash(
        foundation::CanonicalHash& hash,
        const std::vector<ProjectileBinding>& values) {
        hash.WriteU32(
            static_cast<std::uint32_t>(values.size()));
        for (const auto& item : values) {
            hash.WriteU32(item.definitionId);
            hash.WriteU32(item.projectileDefinitionId);
        }
    }
};

} // namespace rts::gameplay
