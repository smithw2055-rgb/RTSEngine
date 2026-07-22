#pragma once

#include <RTSEngine/Ecs/WorldArchive.h>
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
    static constexpr std::uint16_t kVersion = 2u;
    static constexpr std::uint16_t kMinimumVersion = 1u;
    static constexpr std::uint32_t kMaximumWorldBytes = 128u * 1024u * 1024u;
    static constexpr std::uint32_t kMaximumModifierEntries = 4096u;

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
            simulation.modifiers_.entries().size() > kMaximumModifierEntries) {
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

        writer.writeI32(simulation.resources_.available);
        writer.writeI32(simulation.resources_.reserved);
        writer.writeI32(simulation.resources_.spent);

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

        ResourceLedger resources;
        if (!reader.readI32(resources.available) ||
            !reader.readI32(resources.reserved) ||
            !reader.readI32(resources.spent) || resources.available < 0 ||
            resources.reserved < 0 || resources.spent < 0) {
            return false;
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
            !reader.readU32(playerTeamId) ||
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

        GameplayModifierSystem modifiersCandidate;
        for (const auto& entry : modifierEntries) {
            modifiersCandidate.setProfile<MoveSpeed>(
                worldCandidate, entry.teamId, entry.profile);
        }

        if (!validateWorld(
                worldCandidate,
                navigationCandidate,
                resources,
                nextConstructionId,
                nextProductionId)) {
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
                {resources,
                 modifiersCandidate,
                 navigationCandidate,
                 commandCandidate,
                 snapshotCandidate,
                 version >= 2u ? &visionCandidate : nullptr});
            if (snapshotCandidate.worldHash != storedWorldHash) return false;
        }

        simulation.world_ = std::move(worldCandidate);
        simulation.navigation_ = std::move(navigationCandidate);
        simulation.vision_ = std::move(visionCandidate);
        simulation.resources_ = resources;
        simulation.modifiers_ = std::move(modifiersCandidate);
        simulation.commands_ = std::move(commandCandidate);
        simulation.requiredPathStart_ = requiredStart;
        simulation.requiredPathGoal_ = requiredGoal;
        simulation.building_.restoreNextConstructionId(nextConstructionId);
        simulation.nextProductionId_ = nextProductionId;
        simulation.playerTeamId_ = playerTeamId;
        simulation.lastCompletedTick_ = lastCompletedTick;
        simulation.hasStepped_ = hasStepped;
        simulation.snapshot_ = std::move(snapshotCandidate);
        simulation.structuralCommands_ = {};
        simulation.activeCommands_.clear();
        simulation.events_.clear();
        simulation.deathSideEffects_.clear();
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
        }
        return hash.Value();
    }

    static bool validateWorld(
        const ecs::World& world,
        const NavigationGrid& navigation,
        const ResourceLedger& resources,
        ConstructionId nextConstructionId,
        ProductionId nextProductionId) {
        std::int64_t reserved = 0;
        ConstructionId maximumConstructionId = 0;
        ProductionId maximumProductionId = 0;

        for (const auto entity : world.view<ConstructionSite>()) {
            const auto* site = world.try_get<ConstructionSite>(entity);
            if (!site) return false;
            reserved += site->reservedCost;
            maximumConstructionId = std::max(maximumConstructionId, site->id);
        }
        for (const auto entity : world.view<ProductionQueue>()) {
            const auto* queue = world.try_get<ProductionQueue>(entity);
            if (!queue) return false;
            for (const auto& item : queue->items) {
                reserved += item.reservedCost;
                maximumProductionId = std::max(maximumProductionId, item.id);
            }
        }
        if (reserved != resources.reserved ||
            nextConstructionId < maximumConstructionId ||
            nextProductionId < maximumProductionId ||
            reserved > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }

        for (const auto entity : world.view<BuildingFootprint>()) {
            const auto* footprint = world.try_get<BuildingFootprint>(entity);
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
