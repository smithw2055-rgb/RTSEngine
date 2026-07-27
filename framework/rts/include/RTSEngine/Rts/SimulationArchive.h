#pragma once

#include <RTSEngine/Ecs/WorldArchive.h>
#include <RTSEngine/Rts/AuthoritativeStateHash.h>
#include <RTSEngine/Rts/Replay.h>
#include <RTSEngine/Rts/RtsComponentSchemas.h>
#include <RTSEngine/Rts/Simulation.h>
#include <RTSEngine/Rts/VisionComponentSchema.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/SessionSchema.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

class RtsSimulationArchive final {
public:
    static constexpr std::uint32_t kMagic = 0x31535452u; // "RTS1"
    static constexpr std::uint16_t kVersion = 4u;
    static constexpr std::uint16_t kMinimumVersion = 1u;
    static constexpr std::uint32_t kMaximumWorldBytes = 128u * 1024u * 1024u;
    static constexpr std::uint32_t kMaximumModifierEntries = 4096u;
    static constexpr std::uint32_t kMaximumEconomyAccounts = 4096u;

    static std::vector<std::uint8_t> encode(
        const RtsSimulation& simulation) {
        if (!simulation.structuralCommands_.empty()) return {};

        ecs::ComponentSchemaRegistry schemas;
        if (!registerSchemas(schemas)) return {};

        foundation::BinaryWriter worldWriter;
        if (!ecs::WorldArchive::write(worldWriter, simulation.world_, schemas) ||
            worldWriter.bytes().size() > kMaximumWorldBytes) {
            return {};
        }

        const auto commandState = simulation.commands_.snapshot();
        if (commandState.pending.size() > sim::kMaximumArchiveEntries ||
            simulation.modifiers_.entries().size() > kMaximumModifierEntries ||
            simulation.economies_.accounts().size() > kMaximumEconomyAccounts) {
            return {};
        }

        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU64(contentHash(simulation, kVersion));
        writer.writeU32(static_cast<std::uint32_t>(worldWriter.bytes().size()));
        writer.writeBytes(worldWriter.bytes());
        if (!simulation.navigation_.writeState(writer) ||
            !simulation.vision_.writeExploredState(writer)) {
            return {};
        }

        writer.writeU32(static_cast<std::uint32_t>(
            simulation.economies_.accounts().size()));
        for (const auto& account : simulation.economies_.accounts()) {
            writer.writeU32(account.teamId);
            writer.writeI32(account.resources.available);
            writer.writeI32(account.resources.reserved);
            writer.writeI32(account.resources.spent);
        }

        writer.writeU32(static_cast<std::uint32_t>(
            simulation.modifiers_.entries().size()));
        for (const auto& entry : simulation.modifiers_.entries()) {
            writer.writeU32(entry.teamId);
            writeModifierProfile(writer, entry.profile);
        }

        writer.writeU64(commandState.committedThrough);
        writer.writeU32(static_cast<std::uint32_t>(commandState.pending.size()));
        for (const auto& command : commandState.pending) {
            WriteTickCommand(writer, command);
        }

        writeGridPoint(writer, simulation.requiredPathStart_);
        writeGridPoint(writer, simulation.requiredPathGoal_);
        writer.writeU32(simulation.building_.nextConstructionId());
        writer.writeU32(simulation.nextProductionId_);
        writer.writeU32(simulation.playerTeamId_);
        writer.writeU64(simulation.lastCompletedTick_);
        writer.writeBool(simulation.hasStepped_);
        writer.writeU64(
            simulation.hasStepped_ ? simulation.snapshot_.worldHash : 0u);
        return writer.take();
    }

    static bool decode(
        const std::vector<std::uint8_t>& bytes,
        RtsSimulation& simulation) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint64_t storedContentHash = 0;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU64(storedContentHash) || magic != kMagic ||
            version < kMinimumVersion || version > kVersion ||
            storedContentHash != contentHash(simulation, version)) {
            return false;
        }

        std::uint32_t worldByteCount = 0;
        std::vector<std::uint8_t> worldBytes;
        if (!reader.readU32(worldByteCount) ||
            worldByteCount > kMaximumWorldBytes ||
            !reader.readBytes(worldByteCount, worldBytes, kMaximumWorldBytes)) {
            return false;
        }

        ecs::ComponentSchemaRegistry schemas;
        if (!registerSchemas(schemas)) return false;
        ecs::World worldCandidate;
        foundation::BinaryReader worldReader(worldBytes);
        if (!ecs::WorldArchive::read(worldReader, schemas, worldCandidate)) {
            return false;
        }

        NavigationGridState navigationState;
        if (!NavigationGrid::readState(reader, navigationState) ||
            navigationState.width != simulation.navigation_.width() ||
            navigationState.height != simulation.navigation_.height()) {
            return false;
        }
        NavigationGrid navigationCandidate;
        if (!navigationCandidate.restore(std::move(navigationState))) {
            return false;
        }

        VisionRuntime visionCandidate(
            navigationCandidate.width(), navigationCandidate.height());
        if (version >= 2u &&
            !VisionRuntime::readExploredState(
                reader,
                navigationCandidate.width(),
                navigationCandidate.height(),
                visionCandidate)) {
            return false;
        }

        ResourceLedger legacyResources;
        std::vector<TeamEconomyAccount> economyAccounts;
        if (version >= 4u) {
            std::uint32_t accountCount = 0;
            if (!reader.readU32(accountCount) ||
                accountCount > kMaximumEconomyAccounts) {
                return false;
            }
            economyAccounts.resize(accountCount);
            std::uint32_t previousTeam = 0;
            for (auto& account : economyAccounts) {
                if (!reader.readU32(account.teamId) ||
                    account.teamId == 0 || account.teamId <= previousTeam ||
                    !reader.readI32(account.resources.available) ||
                    !reader.readI32(account.resources.reserved) ||
                    !reader.readI32(account.resources.spent) ||
                    !account.resources.valid()) {
                    return false;
                }
                previousTeam = account.teamId;
            }
        } else {
            if (!reader.readI32(legacyResources.available) ||
                !reader.readI32(legacyResources.reserved) ||
                !reader.readI32(legacyResources.spent) ||
                !legacyResources.valid()) {
                return false;
            }
        }

        std::vector<TeamModifierEntry> modifierEntries;
        std::uint32_t modifierCount = 0;
        if (!reader.readU32(modifierCount) ||
            modifierCount > kMaximumModifierEntries) {
            return false;
        }
        modifierEntries.resize(modifierCount);
        std::uint32_t previousTeam = 0;
        bool hasPreviousTeam = false;
        for (auto& entry : modifierEntries) {
            if (!reader.readU32(entry.teamId) ||
                (hasPreviousTeam && entry.teamId <= previousTeam) ||
                !readModifierProfile(reader, entry.profile)) {
                return false;
            }
            previousTeam = entry.teamId;
            hasPreviousTeam = true;
        }

        TickCommandStream::State commandState;
        std::uint32_t commandCount = 0;
        if (!reader.readU64(commandState.committedThrough) ||
            !reader.readU32(commandCount) ||
            commandCount > sim::kMaximumArchiveEntries) {
            return false;
        }
        commandState.pending.resize(commandCount);
        for (auto& command : commandState.pending) {
            if (!ReadTickCommand(reader, command) ||
                command.targetTick < commandState.committedThrough) {
                return false;
            }
        }
        TickCommandStream commandCandidate;
        if (!commandCandidate.restore(std::move(commandState))) return false;

        GridPoint requiredStart;
        GridPoint requiredGoal;
        ConstructionId nextConstructionId = 0;
        ProductionId nextProductionId = 0;
        std::uint32_t playerTeamId = 0;
        std::uint64_t lastCompletedTick = 0;
        bool hasStepped = false;
        std::uint64_t storedWorldHash = 0;
        if (!readGridPoint(reader, requiredStart) ||
            !readGridPoint(reader, requiredGoal) ||
            !reader.readU32(nextConstructionId) ||
            !reader.readU32(nextProductionId) ||
            !reader.readU32(playerTeamId) || playerTeamId == 0 ||
            !reader.readU64(lastCompletedTick) ||
            !reader.readBool(hasStepped) ||
            !reader.readU64(storedWorldHash) || !reader.atEnd()) {
            return false;
        }

        if (!navigationCandidate.contains(requiredStart) ||
            !navigationCandidate.contains(requiredGoal) ||
            (hasStepped &&
             (lastCompletedTick == std::numeric_limits<std::uint64_t>::max() ||
              commandCandidate.committedThrough() != lastCompletedTick + 1u)) ||
            (!hasStepped &&
             (lastCompletedTick != 0 ||
              commandCandidate.committedThrough() != 0 ||
              storedWorldHash != 0 ||
              visionCandidate.layerCount() != 0u))) {
            return false;
        }

        TeamEconomyRuntime economiesCandidate;
        if (version >= 4u) {
            if (!economiesCandidate.restoreAccounts(
                    std::move(economyAccounts)) ||
                !economiesCandidate.find(playerTeamId)) {
                return false;
            }
        } else {
            economyAccounts.push_back(
                {playerTeamId, legacyResources, 0, 0, 0});
            if (!economiesCandidate.restoreAccounts(
                    std::move(economyAccounts))) {
                return false;
            }
        }

        GameplayModifierSystem modifiersCandidate;
        for (const auto& entry : modifierEntries) {
            modifiersCandidate.setProfile<MoveSpeed>(
                worldCandidate, entry.teamId, entry.profile);
        }

        EconomySupplySystem::rebuild(
            worldCandidate,
            economiesCandidate,
            simulation.buildingDefinitions_);
        if (!validateWorld(
                worldCandidate,
                navigationCandidate,
                economiesCandidate,
                nextConstructionId,
                nextProductionId)) {
            return false;
        }

        if (version >= 4u) {
            foundation::BinaryWriter canonicalWorld;
            if (!ecs::WorldArchive::write(
                    canonicalWorld, worldCandidate, schemas) ||
                canonicalWorld.bytes() != worldBytes) {
                return false;
            }
        }

        WorldSnapshot snapshotCandidate;
        if (hasStepped) {
            VisionSystem::run(
                worldCandidate, navigationCandidate, visionCandidate);
            SnapshotBuilder::build(
                worldCandidate,
                lastCompletedTick,
                {economiesCandidate,
                 playerTeamId,
                 modifiersCandidate,
                 navigationCandidate,
                 commandCandidate,
                 snapshotCandidate,
                 version >= 2u ? &visionCandidate : nullptr,
                 version});
            snapshotCandidate.worldHash = FinalizeRtsAuthoritativeWorldHash(
                snapshotCandidate.worldHash,
                worldCandidate.entityRegistryHash(),
                requiredStart,
                requiredGoal,
                nextConstructionId,
                nextProductionId,
                playerTeamId,
                version);
            if (snapshotCandidate.worldHash != storedWorldHash) return false;
        }

        simulation.world_ = std::move(worldCandidate);
        simulation.navigation_ = std::move(navigationCandidate);
        simulation.vision_ = std::move(visionCandidate);
        simulation.economies_ = std::move(economiesCandidate);
        simulation.modifiers_ = std::move(modifiersCandidate);
        simulation.commands_ = std::move(commandCandidate);
        simulation.requiredPathStart_ = requiredStart;
        simulation.requiredPathGoal_ = requiredGoal;
        simulation.building_.restoreNextConstructionId(nextConstructionId);
        simulation.nextProductionId_ = nextProductionId;
        simulation.playerTeamId_ = playerTeamId;
        simulation.lastCompletedTick_ = lastCompletedTick;
        simulation.hasStepped_ = hasStepped;
        simulation.configurationFrozen_ = hasStepped;
        simulation.snapshot_ = std::move(snapshotCandidate);
        simulation.structuralCommands_ = {};
        simulation.activeCommands_.clear();
        simulation.events_.clear();
        simulation.deathSideEffects_.clear();
        simulation.influence_.clear();
        simulation.influenceWorldHash_ = RtsSimulation::kInvalidDerivedHash;
        return true;
    }

private:
    static bool registerSchemas(ecs::ComponentSchemaRegistry& schemas) {
        return RegisterVisionComponentSchema(schemas) &&
               RegisterRtsComponentSchemas(schemas);
    }

    static void writeGridPoint(
        foundation::BinaryWriter& writer,
        GridPoint point) {
        writer.writeI32(point.x);
        writer.writeI32(point.y);
    }

    static bool readGridPoint(
        foundation::BinaryReader& reader,
        GridPoint& point) {
        return reader.readI32(point.x) && reader.readI32(point.y);
    }

    static void writeModifierProfile(
        foundation::BinaryWriter& writer,
        const TeamModifierProfile& value) {
        writer.writeI32(value.unitHealth);
        writer.writeI32(value.unitDamage);
        writer.writeI32(value.unitArmorAdd);
        writer.writeI32(value.unitMoveSpeed);
        writer.writeI32(value.buildingHealth);
        writer.writeI32(value.buildingDamage);
        writer.writeI32(value.constructionSpeed);
        writer.writeI32(value.productionSpeed);
        writer.writeI32(value.bountyMultiplier);
    }

    static bool readModifierProfile(
        foundation::BinaryReader& reader,
        TeamModifierProfile& value) {
        if (!reader.readI32(value.unitHealth) ||
            !reader.readI32(value.unitDamage) ||
            !reader.readI32(value.unitArmorAdd) ||
            !reader.readI32(value.unitMoveSpeed) ||
            !reader.readI32(value.buildingHealth) ||
            !reader.readI32(value.buildingDamage) ||
            !reader.readI32(value.constructionSpeed) ||
            !reader.readI32(value.productionSpeed) ||
            !reader.readI32(value.bountyMultiplier)) {
            return false;
        }
        return SanitizeTeamModifierProfile(value) == value;
    }

    static void hashCombatStats(
        foundation::CanonicalHash& hash,
        const CombatStats& value) {
        hash.WriteI32(value.maximumHealth);
        hash.WriteI32(value.armor);
        hash.WriteI32(value.weaponDamage);
        hash.WriteI32(value.weaponRange);
        hash.WriteU32(value.cooldownTicks);
        hash.WriteI32(value.bounty);
    }

    static std::uint64_t contentHash(
        const RtsSimulation& simulation,
        std::uint16_t version) {
        foundation::CanonicalHash hash;
        hash.WriteU32(static_cast<std::uint32_t>(
            simulation.buildingDefinitions_.values().size()));
        for (const auto& definition :
             simulation.buildingDefinitions_.values()) {
            hash.WriteU32(definition.id);
            hash.WriteI32(definition.cost);
            hash.WriteU32(definition.buildTicks);
            hash.WriteI32(definition.width);
            hash.WriteI32(definition.height);
            hash.WriteBool(definition.producer);
            hashCombatStats(hash, definition.combat);
            if (version >= 2u) hash.WriteI32(definition.visionRange);
            if (version >= 4u) {
                hash.WriteU32(definition.supplyProvided);
                hash.WriteU32(definition.productionQueueCapacity);
                hash.WriteU32(static_cast<std::uint32_t>(
                    definition.trainableUnits.size()));
                for (const auto unitId : definition.trainableUnits) {
                    hash.WriteU32(unitId);
                }
            }
        }
        hash.WriteU32(static_cast<std::uint32_t>(
            simulation.unitDefinitions_.values().size()));
        for (const auto& definition : simulation.unitDefinitions_.values()) {
            hash.WriteU32(definition.id);
            hash.WriteI32(definition.cost);
            hash.WriteU32(definition.trainTicks);
            hash.WriteI32(definition.cellsPerTick);
            hashCombatStats(hash, definition.combat);
            if (version >= 2u) hash.WriteI32(definition.visionRange);
            if (version >= 4u) hash.WriteU32(definition.supplyCost);
        }
        return hash.Value();
    }

    static void addReserved(
        std::vector<std::pair<std::uint32_t, std::int64_t>>& values,
        std::uint32_t teamId,
        std::int32_t amount) {
        auto found = std::lower_bound(
            values.begin(),
            values.end(),
            teamId,
            [](const auto& entry, std::uint32_t value) {
                return entry.first < value;
            });
        if (found == values.end() || found->first != teamId) {
            values.insert(found, {teamId, amount});
        } else {
            found->second += amount;
        }
    }

    static std::int64_t reservedFor(
        const std::vector<std::pair<std::uint32_t, std::int64_t>>& values,
        std::uint32_t teamId) {
        const auto found = std::lower_bound(
            values.begin(),
            values.end(),
            teamId,
            [](const auto& entry, std::uint32_t value) {
                return entry.first < value;
            });
        return found != values.end() && found->first == teamId
            ? found->second
            : 0;
    }

    static bool validateWorld(
        const ecs::World& world,
        const NavigationGrid& navigation,
        const TeamEconomyRuntime& economies,
        ConstructionId nextConstructionId,
        ProductionId nextProductionId) {
        std::vector<std::pair<std::uint32_t, std::int64_t>> reserved;
        ConstructionId maximumConstructionId = 0;
        ProductionId maximumProductionId = 0;

        world.eachRef<ConstructionSite>(
            [&](ecs::Entity, const ConstructionSite& site) {
                addReserved(reserved, site.ownerTeam, site.reservedCost);
                maximumConstructionId = std::max(
                    maximumConstructionId, site.id);
            });
        bool validQueues = true;
        world.eachRef<ProductionQueue, Team>(
            [&](ecs::Entity,
                const ProductionQueue& queue,
                const Team& team) {
                for (const auto& item : queue.items) {
                    if (item.id == 0 || item.unitDefinitionId == 0 ||
                        item.reservedCost < 0 || item.requiredTicks == 0 ||
                        item.baseRequiredTicks == 0) {
                        validQueues = false;
                        return;
                    }
                    addReserved(reserved, team.id, item.reservedCost);
                    maximumProductionId = std::max(
                        maximumProductionId, item.id);
                }
            });
        if (!validQueues || nextConstructionId < maximumConstructionId ||
            nextProductionId < maximumProductionId) {
            return false;
        }

        for (const auto& account : economies.accounts()) {
            const auto expected = reservedFor(reserved, account.teamId);
            if (!account.resources.valid() ||
                expected != account.resources.reserved ||
                expected > std::numeric_limits<std::int32_t>::max()) {
                return false;
            }
        }
        for (const auto& entry : reserved) {
            if (!economies.find(entry.first)) return false;
        }

        bool valid = true;
        world.eachRef<BuildingFootprint>(
            [&](ecs::Entity, const BuildingFootprint& footprint) {
                for (std::int32_t y = 0; y < footprint.height; ++y) {
                    for (std::int32_t x = 0; x < footprint.width; ++x) {
                        const GridPoint point{
                            footprint.origin.x + x,
                            footprint.origin.y + y};
                        if (!navigation.contains(point) ||
                            !navigation.blocked(point)) {
                            valid = false;
                        }
                    }
                }
            });
        world.eachRef<OrderQueue>(
            [&](ecs::Entity, const OrderQueue& queue) {
                for (const auto& order : queue.pending) {
                    if (!navigation.contains(order.target)) valid = false;
                }
            });
        world.eachRef<MovementAgent>(
            [&](ecs::Entity, const MovementAgent& agent) {
                if (agent.nextPoint > agent.path.size()) valid = false;
                for (const auto point : agent.path) {
                    if (!navigation.contains(point)) valid = false;
                }
                if (agent.hasPathGoal &&
                    !navigation.contains(agent.pathGoal)) {
                    valid = false;
                }
            });
        return valid;
    }
};

inline std::vector<std::uint8_t> EncodeRtsSimulation(
    const RtsSimulation& simulation) {
    return RtsSimulationArchive::encode(simulation);
}

inline bool DecodeRtsSimulation(
    const std::vector<std::uint8_t>& bytes,
    RtsSimulation& simulation) {
    return RtsSimulationArchive::decode(bytes, simulation);
}

} // namespace rts::gameplay
