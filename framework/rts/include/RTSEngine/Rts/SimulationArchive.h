#pragma once

#include <RTSEngine/Ecs/WorldArchive.h>
#include <RTSEngine/Rts/AuthoritativeStateHash.h>
#include <RTSEngine/Rts/Replay.h>
#include <RTSEngine/Rts/ResearchStateHash.h>
#include <RTSEngine/Rts/RtsComponentSchemas.h>
#include <RTSEngine/Rts/Simulation.h>
#include <RTSEngine/Rts/TeamEconomyComponentSchemas.h>
#include <RTSEngine/Rts/TechTreeComponentSchemas.h>
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
    static constexpr std::uint32_t kMagic = 0x31535452u;
    static constexpr std::uint16_t kVersion = 5u;
    static constexpr std::uint16_t kMinimumVersion = 1u;
    static constexpr std::uint32_t kMaximumWorldBytes =
        128u * 1024u * 1024u;
    static constexpr std::uint32_t kMaximumModifierEntries = 4096u;
    static constexpr std::uint32_t kMaximumEconomyEntries = 65536u;
    static constexpr std::uint32_t kMaximumTechTeams = 4096u;
    static constexpr std::uint32_t kMaximumCompletedResearch = 65536u;

    static std::uint64_t authoritativeHash(
        const RtsSimulation& simulation) {
        return currentWorldHash(simulation);
    }

    static std::vector<std::uint8_t> encode(
        const RtsSimulation& simulation) {
        if (!simulation.structuralCommands_.empty()) return {};

        ecs::ComponentSchemaRegistry schemas;
        if (!registerSchemas(schemas)) return {};
        foundation::BinaryWriter worldWriter;
        if (!ecs::WorldArchive::write(
                worldWriter, simulation.world_, schemas) ||
            worldWriter.bytes().size() > kMaximumWorldBytes) {
            return {};
        }

        const auto commandState = simulation.commands_.snapshot();
        if (commandState.pending.size() > sim::kMaximumArchiveEntries ||
            simulation.modifiers_.entries().size() >
                kMaximumModifierEntries ||
            simulation.economy_.entries().size() >
                kMaximumEconomyEntries ||
            simulation.tech_.states().size() > kMaximumTechTeams) {
            return {};
        }

        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU64(contentHash(simulation, kVersion));
        writer.writeU32(static_cast<std::uint32_t>(
            worldWriter.bytes().size()));
        writer.writeBytes(worldWriter.bytes());
        if (!simulation.navigation_.writeState(writer) ||
            !simulation.vision_.writeExploredState(writer)) {
            return {};
        }

        writer.writeU32(static_cast<std::uint32_t>(
            simulation.economy_.entries().size()));
        for (const auto& account : simulation.economy_.entries()) {
            writer.writeU32(account.teamId);
            writer.writeU32(account.resourceType);
            writer.writeU64(static_cast<std::uint64_t>(account.available));
            writer.writeU64(static_cast<std::uint64_t>(account.reserved));
            writer.writeU64(static_cast<std::uint64_t>(account.spent));
        }

        writer.writeU32(static_cast<std::uint32_t>(
            simulation.tech_.states().size()));
        for (const auto& state : simulation.tech_.states()) {
            if (state.completed.size() > kMaximumCompletedResearch) return {};
            writer.writeU32(state.teamId);
            writer.writeU32(static_cast<std::uint32_t>(
                state.completed.size()));
            for (const auto id : state.completed) writer.writeU32(id);
        }

        writer.writeU32(static_cast<std::uint32_t>(
            simulation.modifiers_.entries().size()));
        for (const auto& entry : simulation.modifiers_.entries()) {
            writer.writeU32(entry.teamId);
            writeModifierProfile(writer, entry.profile);
        }

        writer.writeU64(commandState.committedThrough);
        writer.writeU32(static_cast<std::uint32_t>(
            commandState.pending.size()));
        for (const auto& command : commandState.pending) {
            WriteTickCommand(writer, command);
        }

        writeGridPoint(writer, simulation.requiredPathStart_);
        writeGridPoint(writer, simulation.requiredPathGoal_);
        writer.writeU32(simulation.building_.nextConstructionId());
        writer.writeU32(simulation.nextProductionId_);
        writer.writeU32(simulation.nextResourceNodeId_);
        writer.writeU32(simulation.nextResearchId_);
        writer.writeU32(simulation.playerTeamId_);
        writer.writeU64(simulation.lastCompletedTick_);
        writer.writeBool(simulation.hasStepped_);
        writer.writeU64(currentWorldHash(simulation));
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
            !reader.readBytes(
                worldByteCount, worldBytes, kMaximumWorldBytes)) {
            return false;
        }

        ecs::ComponentSchemaRegistry schemas;
        if (!registerSchemas(schemas)) return false;
        ecs::World worldCandidate;
        foundation::BinaryReader worldReader(worldBytes);
        if (!ecs::WorldArchive::read(
                worldReader, schemas, worldCandidate)) {
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

        TeamEconomyRuntime economyCandidate;
        ResourceLedger legacyResources;
        if (version >= 4u) {
            std::uint32_t economyCount = 0;
            std::vector<TeamResourceAccount> accounts;
            if (!reader.readU32(economyCount) ||
                economyCount > kMaximumEconomyEntries) {
                return false;
            }
            accounts.resize(economyCount);
            for (auto& account : accounts) {
                std::uint64_t available = 0;
                std::uint64_t reserved = 0;
                std::uint64_t spent = 0;
                if (!reader.readU32(account.teamId) ||
                    !reader.readU32(account.resourceType) ||
                    !reader.readU64(available) ||
                    !reader.readU64(reserved) ||
                    !reader.readU64(spent) ||
                    available > static_cast<std::uint64_t>(
                        std::numeric_limits<ResourceAmount>::max()) ||
                    reserved > static_cast<std::uint64_t>(
                        std::numeric_limits<ResourceAmount>::max()) ||
                    spent > static_cast<std::uint64_t>(
                        std::numeric_limits<ResourceAmount>::max())) {
                    return false;
                }
                account.available = static_cast<ResourceAmount>(available);
                account.reserved = static_cast<ResourceAmount>(reserved);
                account.spent = static_cast<ResourceAmount>(spent);
            }
            if (!economyCandidate.restore(std::move(accounts))) return false;
        } else if (!reader.readI32(legacyResources.available) ||
                   !reader.readI32(legacyResources.reserved) ||
                   !reader.readI32(legacyResources.spent) ||
                   legacyResources.available < 0 ||
                   legacyResources.reserved < 0 ||
                   legacyResources.spent < 0) {
            return false;
        }

        TechTreeRuntime techCandidate;
        if (version >= 5u) {
            std::uint32_t teamCount = 0;
            std::vector<TeamTechState> states;
            if (!reader.readU32(teamCount) || teamCount > kMaximumTechTeams) {
                return false;
            }
            states.resize(teamCount);
            for (auto& state : states) {
                std::uint32_t completedCount = 0;
                if (!reader.readU32(state.teamId) ||
                    !reader.readU32(completedCount) ||
                    completedCount > kMaximumCompletedResearch) {
                    return false;
                }
                state.completed.resize(completedCount);
                for (auto& id : state.completed) {
                    if (!reader.readU32(id) ||
                        !simulation.researchDefinitions_.find(id)) {
                        return false;
                    }
                }
            }
            if (!techCandidate.restore(std::move(states))) return false;
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
        std::uint32_t nextResourceNodeId = 0;
        ResearchQueueId nextResearchId = 0;
        std::uint32_t playerTeamId = 0;
        std::uint64_t lastCompletedTick = 0;
        bool hasStepped = false;
        std::uint64_t storedWorldHash = 0;
        if (!readGridPoint(reader, requiredStart) ||
            !readGridPoint(reader, requiredGoal) ||
            !reader.readU32(nextConstructionId) ||
            !reader.readU32(nextProductionId) ||
            (version >= 4u && !reader.readU32(nextResourceNodeId)) ||
            (version >= 5u && !reader.readU32(nextResearchId)) ||
            !reader.readU32(playerTeamId) ||
            !reader.readU64(lastCompletedTick) ||
            !reader.readBool(hasStepped) ||
            !reader.readU64(storedWorldHash) || !reader.atEnd()) {
            return false;
        }

        if (version < 4u &&
            !economyCandidate.importLegacy(
                playerTeamId,
                kPrimaryResourceType,
                legacyResources)) {
            return false;
        }

        if (!navigationCandidate.contains(requiredStart) ||
            !navigationCandidate.contains(requiredGoal) ||
            playerTeamId == 0 ||
            (hasStepped &&
             (lastCompletedTick ==
                  std::numeric_limits<std::uint64_t>::max() ||
              commandCandidate.committedThrough() !=
                  lastCompletedTick + 1u)) ||
            (!hasStepped &&
             (lastCompletedTick != 0 ||
              commandCandidate.committedThrough() != 0 ||
              storedWorldHash != 0 ||
              visionCandidate.layerCount() != 0u))) {
            return false;
        }

        GameplayModifierSystem modifiersCandidate;
        for (const auto& entry : modifierEntries) {
            modifiersCandidate.setProfile<MoveSpeed>(
                worldCandidate, entry.teamId, entry.profile);
        }

        if (!validateWorld(
                worldCandidate,
                navigationCandidate,
                economyCandidate,
                techCandidate,
                simulation.researchDefinitions_,
                nextConstructionId,
                nextProductionId,
                nextResourceNodeId,
                nextResearchId,
                version)) {
            return false;
        }

        foundation::BinaryWriter canonicalWorld;
        if (!ecs::WorldArchive::write(
                canonicalWorld, worldCandidate, schemas) ||
            canonicalWorld.bytes() != worldBytes) {
            return false;
        }

        WorldSnapshot snapshotCandidate;
        if (hasStepped) {
            VisionSystem::run(
                worldCandidate, navigationCandidate, visionCandidate);
            SnapshotBuilder::build(
                worldCandidate,
                lastCompletedTick,
                {economyCandidate,
                 playerTeamId,
                 modifiersCandidate,
                 navigationCandidate,
                 commandCandidate,
                 snapshotCandidate,
                 version >= 2u ? &visionCandidate : nullptr,
                 version});
            snapshotCandidate.teamTech = techCandidate.states();
            snapshotCandidate.worldHash = FinalizeRtsAuthoritativeWorldHash(
                snapshotCandidate.worldHash,
                worldCandidate.entityRegistryHash(),
                requiredStart,
                requiredGoal,
                nextConstructionId,
                nextProductionId,
                playerTeamId,
                version,
                nextResourceNodeId,
                nextResearchId,
                version >= 5u ? HashTechTreeState(techCandidate) : 0u,
                version >= 5u ? HashResearchQueueState(worldCandidate) : 0u);
            if (snapshotCandidate.worldHash != storedWorldHash) return false;
        }

        simulation.world_ = std::move(worldCandidate);
        simulation.navigation_ = std::move(navigationCandidate);
        simulation.vision_ = std::move(visionCandidate);
        simulation.economy_ = std::move(economyCandidate);
        simulation.tech_ = std::move(techCandidate);
        simulation.modifiers_ = std::move(modifiersCandidate);
        simulation.commands_ = std::move(commandCandidate);
        simulation.requiredPathStart_ = requiredStart;
        simulation.requiredPathGoal_ = requiredGoal;
        simulation.building_.restoreNextConstructionId(nextConstructionId);
        simulation.nextProductionId_ = nextProductionId;
        simulation.nextResourceNodeId_ = nextResourceNodeId;
        simulation.nextResearchId_ = nextResearchId;
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
    struct ReservedAmount final {
        std::uint32_t teamId{};
        ResourceTypeId resourceType{};
        ResourceAmount amount{};
    };

    static bool registerSchemas(
        ecs::ComponentSchemaRegistry& schemas) {
        return RegisterTeamEconomyComponentSchemas(schemas) &&
               RegisterTechTreeComponentSchemas(schemas) &&
               RegisterVisionComponentSchema(schemas) &&
               RegisterRtsComponentSchemas(schemas);
    }

    static std::uint64_t currentWorldHash(
        const RtsSimulation& simulation) {
        if (!simulation.hasStepped_) return 0u;
        WorldSnapshot current;
        SnapshotBuilder::build(
            simulation.world_,
            simulation.lastCompletedTick_,
            {simulation.economy_,
             simulation.playerTeamId_,
             simulation.modifiers_,
             simulation.navigation_,
             simulation.commands_,
             current,
             &simulation.vision_});
        return FinalizeRtsAuthoritativeWorldHash(
            current.worldHash,
            simulation.world_.entityRegistryHash(),
            simulation.requiredPathStart_,
            simulation.requiredPathGoal_,
            simulation.building_.nextConstructionId(),
            simulation.nextProductionId_,
            simulation.playerTeamId_,
            kVersion,
            simulation.nextResourceNodeId_,
            simulation.nextResearchId_,
            HashTechTreeState(simulation.tech_),
            HashResearchQueueState(simulation.world_));
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

    static void hashDelta(
        foundation::CanonicalHash& hash,
        const TeamModifierDelta& value) {
        hash.WriteI32(value.unitHealth);
        hash.WriteI32(value.unitDamage);
        hash.WriteI32(value.unitArmorAdd);
        hash.WriteI32(value.unitMoveSpeed);
        hash.WriteI32(value.buildingHealth);
        hash.WriteI32(value.buildingDamage);
        hash.WriteI32(value.constructionSpeed);
        hash.WriteI32(value.productionSpeed);
        hash.WriteI32(value.bountyMultiplier);
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
                hash.WriteU32(definition.dropOffResourceType);
                hash.WriteU32(definition.supplyProvided);
            }
        }
        hash.WriteU32(static_cast<std::uint32_t>(
            simulation.unitDefinitions_.values().size()));
        for (const auto& definition :
             simulation.unitDefinitions_.values()) {
            hash.WriteU32(definition.id);
            hash.WriteI32(definition.cost);
            hash.WriteU32(definition.trainTicks);
            hash.WriteI32(definition.cellsPerTick);
            hashCombatStats(hash, definition.combat);
            if (version >= 2u) hash.WriteI32(definition.visionRange);
            if (version >= 4u) {
                hash.WriteBool(definition.worker);
                hash.WriteU64(static_cast<std::uint64_t>(
                    definition.cargoCapacity));
                hash.WriteU64(static_cast<std::uint64_t>(
                    definition.harvestAmount));
                hash.WriteU32(definition.harvestTicks);
            }
        }
        if (version >= 5u) {
            hash.WriteU32(static_cast<std::uint32_t>(
                simulation.researchDefinitions_.values().size()));
            for (const auto& definition :
                 simulation.researchDefinitions_.values()) {
                hash.WriteU32(definition.id);
                hash.WriteU32(definition.researchTicks);
                hash.WriteU32(static_cast<std::uint32_t>(
                    definition.costs.size()));
                for (const auto& cost : definition.costs) {
                    hash.WriteU32(cost.resourceType);
                    hash.WriteU64(static_cast<std::uint64_t>(cost.amount));
                }
                PrerequisiteCatalog::appendPrerequisites(
                    hash, definition.prerequisites);
                hashDelta(hash, definition.modifiers);
            }
            simulation.buildingPrerequisites_.appendHash(hash);
            simulation.unitPrerequisites_.appendHash(hash);
        }
        return hash.Value();
    }

    static void addReserved(
        std::vector<ReservedAmount>& values,
        std::uint32_t teamId,
        ResourceTypeId resourceType,
        ResourceAmount amount) {
        const auto found = std::lower_bound(
            values.begin(), values.end(),
            ReservedAmount{teamId, resourceType, 0},
            [](const ReservedAmount& first, const ReservedAmount& second) {
                return first.teamId < second.teamId ||
                       (first.teamId == second.teamId &&
                        first.resourceType < second.resourceType);
            });
        if (found != values.end() && found->teamId == teamId &&
            found->resourceType == resourceType) {
            found->amount += amount;
        } else {
            values.insert(found, {teamId, resourceType, amount});
        }
    }

    static ResourceAmount expectedReserved(
        const std::vector<ReservedAmount>& values,
        std::uint32_t teamId,
        ResourceTypeId resourceType) {
        const auto found = std::lower_bound(
            values.begin(), values.end(),
            ReservedAmount{teamId, resourceType, 0},
            [](const ReservedAmount& first, const ReservedAmount& second) {
                return first.teamId < second.teamId ||
                       (first.teamId == second.teamId &&
                        first.resourceType < second.resourceType);
            });
        return found != values.end() && found->teamId == teamId &&
               found->resourceType == resourceType
            ? found->amount
            : 0;
    }

    static bool validateWorld(
        const ecs::World& world,
        const NavigationGrid& navigation,
        const TeamEconomyRuntime& economy,
        const TechTreeRuntime& tech,
        const DefinitionCatalog<ResearchDefinition>& researchDefinitions,
        ConstructionId nextConstructionId,
        ProductionId nextProductionId,
        std::uint32_t nextResourceNodeId,
        ResearchQueueId nextResearchId,
        std::uint16_t compatibilityVersion) {
        ResourceAmount totalReserved = 0;
        ConstructionId maximumConstructionId = 0;
        ProductionId maximumProductionId = 0;
        std::uint32_t maximumResourceNodeId = 0;
        ResearchQueueId maximumResearchId = 0;
        std::vector<ReservedAmount> reserved;
        std::vector<std::uint32_t> resourceNodeIds;

        for (const auto entity : world.view<ConstructionSite>()) {
            const auto* site = world.try_get<ConstructionSite>(entity);
            if (!site || site->ownerTeam == 0 || site->reservedCost < 0) {
                return false;
            }
            totalReserved += site->reservedCost;
            addReserved(
                reserved,
                site->ownerTeam,
                kPrimaryResourceType,
                site->reservedCost);
            maximumConstructionId = std::max(
                maximumConstructionId, site->id);
        }
        for (const auto entity : world.view<ProductionQueue>()) {
            const auto* queue = world.try_get<ProductionQueue>(entity);
            const auto* team = world.try_get<Team>(entity);
            if (!queue || (!queue->items.empty() && !team)) return false;
            for (const auto& item : queue->items) {
                if (item.reservedCost < 0 || !team || team->id == 0) {
                    return false;
                }
                totalReserved += item.reservedCost;
                addReserved(
                    reserved,
                    team->id,
                    kPrimaryResourceType,
                    item.reservedCost);
                maximumProductionId = std::max(
                    maximumProductionId, item.id);
            }
        }
        if (compatibilityVersion >= 5u) {
            for (const auto entity : world.view<ResearchQueue>()) {
                const auto* queue = world.try_get<ResearchQueue>(entity);
                const auto* team = world.try_get<Team>(entity);
                if (!queue || (!queue->items.empty() && !team)) return false;
                for (const auto& item : queue->items) {
                    if (!team || team->id == 0 || item.id == 0 ||
                        item.researchDefinitionId == 0 ||
                        !researchDefinitions.find(
                            item.researchDefinitionId) ||
                        tech.completed(team->id, item.researchDefinitionId) ||
                        !ValidateResourceCosts(item.reservedCosts)) {
                        return false;
                    }
                    maximumResearchId = std::max(
                        maximumResearchId, item.id);
                    for (const auto& cost : item.reservedCosts) {
                        totalReserved += cost.amount;
                        addReserved(
                            reserved,
                            team->id,
                            cost.resourceType,
                            cost.amount);
                    }
                }
            }
        }

        if (nextConstructionId < maximumConstructionId ||
            nextProductionId < maximumProductionId ||
            nextResearchId < maximumResearchId) {
            return false;
        }

        ResourceAmount economyReserved = 0;
        for (const auto& account : economy.entries()) {
            economyReserved += account.reserved;
            if (compatibilityVersion >= 5u &&
                account.reserved != expectedReserved(
                    reserved, account.teamId, account.resourceType)) {
                return false;
            }
            if (compatibilityVersion == 4u &&
                account.resourceType == kPrimaryResourceType &&
                account.reserved != expectedReserved(
                    reserved, account.teamId, kPrimaryResourceType)) {
                return false;
            }
            if (compatibilityVersion == 4u &&
                account.resourceType != kPrimaryResourceType &&
                account.reserved != 0) {
                return false;
            }
        }
        if (economyReserved != totalReserved) return false;
        if (compatibilityVersion >= 4u) {
            for (const auto& value : reserved) {
                if (economy.reserved(
                        value.teamId, value.resourceType) != value.amount) {
                    return false;
                }
            }
        }

        for (const auto entity : world.view<BuildingFootprint>()) {
            const auto* footprint =
                world.try_get<BuildingFootprint>(entity);
            if (!footprint) return false;
            for (std::int32_t y = 0; y < footprint->height; ++y) {
                for (std::int32_t x = 0; x < footprint->width; ++x) {
                    const GridPoint point{
                        footprint->origin.x + x,
                        footprint->origin.y + y};
                    if (!navigation.contains(point) ||
                        !navigation.blocked(point)) {
                        return false;
                    }
                }
            }
        }

        for (const auto entity : world.view<ResourceDropOff>()) {
            const auto* dropOff = world.try_get<ResourceDropOff>(entity);
            const auto* team = world.try_get<Team>(entity);
            if (!dropOff || !team || team->id == 0 ||
                !navigation.contains({dropOff->accessX, dropOff->accessY}) ||
                navigation.blocked({dropOff->accessX, dropOff->accessY})) {
                return false;
            }
        }
        for (const auto entity : world.view<ConstructionEconomyFeatures>()) {
            const auto* features =
                world.try_get<ConstructionEconomyFeatures>(entity);
            if (!features) return false;
            if (features->dropOffResourceType != 0 &&
                (!navigation.contains(
                    {features->dropOffAccessX,
                     features->dropOffAccessY}) ||
                 navigation.blocked(
                    {features->dropOffAccessX,
                     features->dropOffAccessY}))) {
                return false;
            }
        }

        for (const auto entity : world.view<ResourceNode>()) {
            const auto* node = world.try_get<ResourceNode>(entity);
            const auto* position = world.try_get<Position>(entity);
            if (!node || !position || node->id == 0 ||
                node->resourceType == 0 || node->remaining < 0 ||
                !navigation.contains({position->x, position->y})) {
                return false;
            }
            maximumResourceNodeId = std::max(
                maximumResourceNodeId, node->id);
            resourceNodeIds.push_back(node->id);
        }
        std::sort(resourceNodeIds.begin(), resourceNodeIds.end());
        if (std::adjacent_find(
                resourceNodeIds.begin(), resourceNodeIds.end()) !=
                resourceNodeIds.end() ||
            nextResourceNodeId < maximumResourceNodeId) {
            return false;
        }

        for (const auto entity : world.view<WorkerHarvester>()) {
            const auto* worker = world.try_get<WorkerHarvester>(entity);
            if (!worker) return false;
            if (worker->targetNode.valid() &&
                (!world.alive(worker->targetNode) ||
                 !world.try_get<ResourceNode>(worker->targetNode))) {
                return false;
            }
        }
        for (const auto entity : world.view<OrderQueue>()) {
            const auto* queue = world.try_get<OrderQueue>(entity);
            if (!queue) return false;
            for (const auto& order : queue->pending) {
                if (!navigation.contains(order.target)) return false;
            }
        }
        for (const auto entity : world.view<MovementAgent>()) {
            const auto* agent = world.try_get<MovementAgent>(entity);
            if (!agent || agent->nextPoint > agent->path.size()) return false;
            for (const auto point : agent->path) {
                if (!navigation.contains(point)) return false;
            }
            if (agent->hasPathGoal &&
                !navigation.contains(agent->pathGoal)) {
                return false;
            }
        }
        return true;
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
