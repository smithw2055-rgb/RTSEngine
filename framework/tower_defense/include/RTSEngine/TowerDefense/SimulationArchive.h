#pragma once

#include <RTSEngine/Rts/SimulationArchive.h>
#include <RTSEngine/TowerDefense/Simulation.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/SessionSchema.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::tower_defense {

class TowerDefenseSimulationArchive final {
public:
    static constexpr std::uint32_t kMagic = 0x31564454u; // "TDV1"
    static constexpr std::uint16_t kVersion = 3u;
    static constexpr std::uint32_t kMaximumRtsBytes = 256u * 1024u * 1024u;

    static std::vector<std::uint8_t> encode(
        const TowerDefenseSimulation& simulation) {
        const auto rtsBytes = gameplay::EncodeRtsSimulation(simulation.rts_);
        if (rtsBytes.empty() || rtsBytes.size() > kMaximumRtsBytes) return {};

        const auto commandState = simulation.commands_.snapshot();
        if (commandState.pending.size() > sim::kMaximumArchiveEntries ||
            simulation.trackedEnemies_.size() > sim::kMaximumArchiveEntries ||
            !archiveableWavePlan(simulation.director_.plan())) {
            return {};
        }

        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU64(simulation.rootSeed_);
        writer.writeU32(static_cast<std::uint32_t>(rtsBytes.size()));
        writer.writeBytes(rtsBytes);

        const auto& state = simulation.director_.state();
        const auto* activeDefinition = state.waveId == 0
            ? nullptr
            : simulation.director_.definition(state.waveId);
        writer.writeBool(activeDefinition != nullptr);
        if (activeDefinition) writeWaveDefinition(writer, *activeDefinition);
        writeWavePlan(writer, simulation.director_.plan());
        writeWaveState(writer, state);
        writeRewardOffer(writer, simulation.director_.offer());

        writer.writeU64(commandState.committedThrough);
        writer.writeU32(static_cast<std::uint32_t>(commandState.pending.size()));
        for (const auto& command : commandState.pending) {
            writeCommand(writer, command);
        }

        writer.writeU32(static_cast<std::uint32_t>(
            simulation.trackedEnemies_.size()));
        for (const auto& enemy : simulation.trackedEnemies_) {
            writeEntity(writer, enemy.entity);
            writer.writeU32(enemy.waveId);
            writer.writeU32(enemy.laneId);
            writer.writeU32(enemy.unitDefinitionId);
            writer.writeU32(enemy.waypointIndex);
            writer.writeBool(enemy.resolved);
        }

        writeEntity(writer, simulation.core_);
        writer.writeU32(simulation.playerTeamId_);
        writer.writeU64(simulation.nextInternalRtsSequence_);
        writer.writeU64(simulation.lastTick_);
        writer.writeBool(simulation.hasStepped_);
        writer.writeBool(simulation.coreFailureReported_);
        writer.writeU64(simulation.hasStepped_
            ? simulation.snapshot_.worldHash
            : 0u);
        return writer.take();
    }

    static bool decode(
        const std::vector<std::uint8_t>& bytes,
        TowerDefenseSimulation& simulation) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint64_t rootSeed = 0;
        std::uint32_t rtsByteCount = 0;
        std::vector<std::uint8_t> rtsBytes;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU64(rootSeed) || magic != kMagic ||
            version != kVersion || rootSeed != simulation.rootSeed_ ||
            !reader.readU32(rtsByteCount) ||
            rtsByteCount > kMaximumRtsBytes ||
            !reader.readBytes(rtsByteCount, rtsBytes, kMaximumRtsBytes)) {
            return false;
        }

        bool hasActiveDefinition = false;
        WaveDefinition activeDefinition;
        if (!reader.readBool(hasActiveDefinition) ||
            (hasActiveDefinition &&
             !readWaveDefinition(reader, activeDefinition))) {
            return false;
        }

        WavePlan plan;
        WaveState state;
        RewardOffer offer;
        if (!readWavePlan(reader, plan) ||
            !readWaveState(reader, state) ||
            !readRewardOffer(reader, offer)) {
            return false;
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
            if (!readCommand(reader, command) ||
                command.targetTick < commandState.committedThrough) {
                return false;
            }
        }
        TickCommandStream commandCandidate;
        if (!commandCandidate.restore(commandState)) return false;

        std::uint32_t trackedCount = 0;
        if (!reader.readU32(trackedCount) ||
            trackedCount > sim::kMaximumArchiveEntries) {
            return false;
        }
        std::vector<TowerDefenseSimulation::TrackedEnemy> trackedCandidate;
        trackedCandidate.resize(trackedCount);
        for (auto& enemy : trackedCandidate) {
            if (!readEntity(reader, enemy.entity) ||
                !reader.readU32(enemy.waveId) ||
                !reader.readU32(enemy.laneId) ||
                !reader.readU32(enemy.unitDefinitionId) ||
                !reader.readU32(enemy.waypointIndex) ||
                !reader.readBool(enemy.resolved) ||
                !enemy.entity.valid() || enemy.waveId == 0 ||
                enemy.laneId == 0 || enemy.unitDefinitionId == 0 ||
                enemy.waypointIndex == 0) {
                return false;
            }
        }

        ecs::Entity core;
        std::uint32_t playerTeamId = 0;
        std::uint64_t nextInternalRtsSequence = 0;
        std::uint64_t lastTick = 0;
        bool hasStepped = false;
        bool coreFailureReported = false;
        std::uint64_t storedWorldHash = 0;
        if (!readEntity(reader, core) ||
            !reader.readU32(playerTeamId) ||
            !reader.readU64(nextInternalRtsSequence) ||
            !reader.readU64(lastTick) ||
            !reader.readBool(hasStepped) ||
            !reader.readBool(coreFailureReported) ||
            !reader.readU64(storedWorldHash) || !reader.atEnd()) {
            return false;
        }
        constexpr auto maximumNextSequence =
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max()) + 1u;
        if (nextInternalRtsSequence == 0 ||
            nextInternalRtsSequence > maximumNextSequence) {
            return false;
        }

        WaveDirector directorCandidate(rootSeed);
        directorCandidate.replaceLaneGraph(simulation.director_.laneGraph());
        for (const auto& lane : simulation.laneDefinitions_) {
            if (!directorCandidate.registerLane(lane)) return false;
        }
        for (const auto& wave : simulation.waveDefinitions_) {
            if (!directorCandidate.registerWave(wave)) return false;
        }
        for (const auto& reward : simulation.rewardDefinitions_) {
            if (!directorCandidate.registerReward(reward)) return false;
        }
        if (hasActiveDefinition &&
            !directorCandidate.registerWave(activeDefinition)) {
            return false;
        }
        if (!rebuildDirector(directorCandidate, plan, state, offer)) {
            return false;
        }

        if ((state.waveId == 0) != !hasActiveDefinition ||
            (hasStepped &&
             (lastTick == std::numeric_limits<std::uint64_t>::max() ||
              commandCandidate.committedThrough() != lastTick + 1u)) ||
            (!hasStepped &&
             (lastTick != 0 || commandCandidate.committedThrough() != 0 ||
              storedWorldHash != 0))) {
            return false;
        }

        const auto backupRts = gameplay::EncodeRtsSimulation(simulation.rts_);
        if (backupRts.empty()) return false;
        const auto backupDirector = simulation.director_;
        const auto backupCommands = simulation.commands_;
        const auto backupTracked = simulation.trackedEnemies_;
        const auto backupEvents = simulation.events_;
        const auto backupSnapshot = simulation.snapshot_;
        const auto backupCore = simulation.core_;
        const auto backupPlayerTeam = simulation.playerTeamId_;
        const auto backupNextInternalRtsSequence =
            simulation.nextInternalRtsSequence_;
        const auto backupLastTick = simulation.lastTick_;
        const auto backupHasStepped = simulation.hasStepped_;
        const auto backupCoreFailure = simulation.coreFailureReported_;
        const auto backupWaves = simulation.waveDefinitions_;

        if (!gameplay::DecodeRtsSimulation(rtsBytes, simulation.rts_)) {
            return false;
        }

        auto rollback = [&]() {
            (void)gameplay::DecodeRtsSimulation(backupRts, simulation.rts_);
            simulation.director_ = backupDirector;
            simulation.commands_ = backupCommands;
            simulation.trackedEnemies_ = backupTracked;
            simulation.events_ = backupEvents;
            simulation.snapshot_ = backupSnapshot;
            simulation.core_ = backupCore;
            simulation.playerTeamId_ = backupPlayerTeam;
            simulation.nextInternalRtsSequence_ =
                backupNextInternalRtsSequence;
            simulation.lastTick_ = backupLastTick;
            simulation.hasStepped_ = backupHasStepped;
            simulation.coreFailureReported_ = backupCoreFailure;
            simulation.waveDefinitions_ = backupWaves;
        };

        if (!validateRestoredState(
                simulation.rts_, directorCandidate, trackedCandidate,
                core, playerTeamId, lastTick, hasStepped,
                coreFailureReported)) {
            rollback();
            return false;
        }

        simulation.director_ = std::move(directorCandidate);
        simulation.commands_ = std::move(commandCandidate);
        simulation.trackedEnemies_ = std::move(trackedCandidate);
        simulation.core_ = core;
        simulation.playerTeamId_ = playerTeamId;
        simulation.nextInternalRtsSequence_ = nextInternalRtsSequence;
        simulation.lastTick_ = lastTick;
        simulation.hasStepped_ = hasStepped;
        simulation.coreFailureReported_ = coreFailureReported;
        simulation.events_.clear();
        if (hasActiveDefinition) {
            TowerDefenseSimulation::replaceById(
                simulation.waveDefinitions_, activeDefinition);
        }
        simulation.snapshot_ = {};
        if (hasStepped) simulation.buildSnapshot(lastTick);
        if ((hasStepped && simulation.snapshot_.worldHash != storedWorldHash) ||
            (!hasStepped && simulation.snapshot_.worldHash != 0)) {
            rollback();
            return false;
        }
        return true;
    }

    static std::uint64_t configurationHash(
        const TowerDefenseSimulation& simulation) {
        foundation::CanonicalHash hash;
        hash.WriteU64(simulation.rootSeed_);
        simulation.director_.laneGraph().appendHash(hash);
        hash.WriteU32(static_cast<std::uint32_t>(
            simulation.laneDefinitions_.size()));
        for (const auto& lane : simulation.laneDefinitions_) {
            hash.WriteU32(lane.id);
            hash.WriteI32(lane.spawn.x);
            hash.WriteI32(lane.spawn.y);
            hash.WriteI32(lane.goal.x);
            hash.WriteI32(lane.goal.y);
            hash.WriteU32(lane.weight);
            hash.WriteU32(lane.startNodeId);
            hash.WriteU32(lane.goalNodeId);
        }
        return hash.Value();
    }

private:
    static bool archiveableWavePlan(const WavePlan& plan) noexcept {
        if (plan.rewards.size() > sim::kMaximumArchiveEntries ||
            plan.routes.size() > sim::kMaximumArchiveEntries ||
            plan.spawns.size() > sim::kMaximumArchiveEntries) {
            return false;
        }
        for (const auto& route : plan.routes) {
            if (route.nodeIds.size() > sim::kMaximumArchiveEntries ||
                route.points.empty() ||
                route.points.size() > sim::kMaximumArchiveEntries ||
                (!route.nodeIds.empty() &&
                 route.nodeIds.size() != route.points.size())) {
                return false;
            }
        }
        return true;
    }

    static void writeEntity(
        foundation::BinaryWriter& writer,
        ecs::Entity entity) {
        writer.writeU32(entity.index);
        writer.writeU32(entity.generation);
    }

    static bool readEntity(
        foundation::BinaryReader& reader,
        ecs::Entity& entity) {
        if (!reader.readU32(entity.index) ||
            !reader.readU32(entity.generation)) {
            return false;
        }
        return (entity.index == 0 && entity.generation == 0) ||
               (entity.index != 0 && entity.generation != 0);
    }

    static void writeGridPoint(
        foundation::BinaryWriter& writer,
        gameplay::GridPoint value) {
        writer.writeI32(value.x);
        writer.writeI32(value.y);
    }

    static bool readGridPoint(
        foundation::BinaryReader& reader,
        gameplay::GridPoint& value) {
        return reader.readI32(value.x) && reader.readI32(value.y);
    }

    static void writeCommand(
        foundation::BinaryWriter& writer,
        const TickCommand& command) {
        writer.writeU64(command.targetTick);
        writer.writeU32(command.issuer);
        writer.writeU32(command.sequence);
        writer.writeU8(static_cast<std::uint8_t>(command.type));
        writer.writeU32(command.objectId);
    }

    static bool readCommand(
        foundation::BinaryReader& reader,
        TickCommand& command) {
        std::uint8_t type = 0;
        if (!reader.readU64(command.targetTick) ||
            !reader.readU32(command.issuer) ||
            !reader.readU32(command.sequence) ||
            !reader.readU8(type) ||
            !reader.readU32(command.objectId) ||
            type > static_cast<std::uint8_t>(CommandType::ChooseReward)) {
            return false;
        }
        command.type = static_cast<CommandType>(type);
        return true;
    }

    static void writeWaveDefinition(
        foundation::BinaryWriter& writer,
        const WaveDefinition& value) {
        writer.writeU32(value.id);
        writer.writeU32(value.budget);
        writer.writeU32(value.spawnIntervalTicks);
        writer.writeU32(value.enemyTeamId);
        writer.writeU32(static_cast<std::uint32_t>(value.laneIds.size()));
        for (const auto id : value.laneIds) writer.writeU32(id);
        writer.writeU32(static_cast<std::uint32_t>(value.enemies.size()));
        for (const auto& enemy : value.enemies) {
            writer.writeU32(enemy.unitDefinitionId);
            writer.writeU32(enemy.budgetCost);
            writer.writeU32(enemy.weight);
            writer.writeU32(enemy.maxPerWave);
        }
        writer.writeU32(static_cast<std::uint32_t>(value.rewardPool.size()));
        for (const auto id : value.rewardPool) writer.writeU32(id);
        writer.writeU32(value.rewardChoices);
    }

    static bool readWaveDefinition(
        foundation::BinaryReader& reader,
        WaveDefinition& value) {
        std::uint32_t count = 0;
        if (!reader.readU32(value.id) || !reader.readU32(value.budget) ||
            !reader.readU32(value.spawnIntervalTicks) ||
            !reader.readU32(value.enemyTeamId) || value.id == 0 ||
            !reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
            return false;
        }
        value.laneIds.resize(count);
        for (auto& id : value.laneIds) {
            if (!reader.readU32(id) || id == 0) return false;
        }
        if (!reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
            return false;
        }
        value.enemies.resize(count);
        for (auto& enemy : value.enemies) {
            if (!reader.readU32(enemy.unitDefinitionId) ||
                !reader.readU32(enemy.budgetCost) ||
                !reader.readU32(enemy.weight) ||
                !reader.readU32(enemy.maxPerWave) ||
                enemy.unitDefinitionId == 0 || enemy.budgetCost == 0 ||
                enemy.weight == 0) {
                return false;
            }
        }
        if (!reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
            return false;
        }
        value.rewardPool.resize(count);
        for (auto& id : value.rewardPool) {
            if (!reader.readU32(id) || id == 0) return false;
        }
        return reader.readU32(value.rewardChoices);
    }

    static void writeWavePlan(
        foundation::BinaryWriter& writer,
        const WavePlan& value) {
        writer.writeU32(value.waveId);
        writer.writeU32(value.enemyTeamId);
        writer.writeU32(value.unusedBudget);
        writer.writeU32(value.rewardChoices);
        writer.writeU32(static_cast<std::uint32_t>(value.rewards.size()));
        for (const auto& reward : value.rewards) {
            writer.writeU32(reward.id);
            writer.writeU32(reward.weight);
            writer.writeI32(reward.resourceGrant);
        }
        writer.writeU32(static_cast<std::uint32_t>(value.routes.size()));
        for (const auto& route : value.routes) {
            writer.writeU32(route.id);
            writer.writeU32(route.totalCost);
            writer.writeU32(static_cast<std::uint32_t>(route.nodeIds.size()));
            for (const auto nodeId : route.nodeIds) writer.writeU32(nodeId);
            writer.writeU32(static_cast<std::uint32_t>(route.points.size()));
            for (const auto point : route.points) writeGridPoint(writer, point);
        }
        writer.writeU32(static_cast<std::uint32_t>(value.spawns.size()));
        for (const auto& spawn : value.spawns) {
            writer.writeU32(spawn.sequence);
            writer.writeU64(spawn.tickOffset);
            writer.writeU32(spawn.laneId);
            writeGridPoint(writer, spawn.spawn);
            writeGridPoint(writer, spawn.goal);
            writer.writeU32(spawn.unitDefinitionId);
        }
    }

    static bool readWavePlan(
        foundation::BinaryReader& reader,
        WavePlan& value) {
        std::uint32_t count = 0;
        if (!reader.readU32(value.waveId) ||
            !reader.readU32(value.enemyTeamId) ||
            !reader.readU32(value.unusedBudget) ||
            !reader.readU32(value.rewardChoices) ||
            !reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
            return false;
        }
        value.rewards.resize(count);
        RewardId previousReward = 0;
        for (auto& reward : value.rewards) {
            if (!reader.readU32(reward.id) ||
                !reader.readU32(reward.weight) ||
                !reader.readI32(reward.resourceGrant) || reward.id == 0 ||
                reward.weight == 0 || reward.id <= previousReward) {
                return false;
            }
            previousReward = reward.id;
        }

        if (!reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
            return false;
        }
        value.routes.resize(count);
        LaneId previousLane = 0;
        for (auto& route : value.routes) {
            std::uint32_t nodeCount = 0;
            std::uint32_t pointCount = 0;
            if (!reader.readU32(route.id) ||
                !reader.readU32(route.totalCost) ||
                route.id == 0 || route.id <= previousLane ||
                !reader.readU32(nodeCount) ||
                nodeCount > sim::kMaximumArchiveEntries) {
                return false;
            }
            previousLane = route.id;
            route.nodeIds.resize(nodeCount);
            for (auto& nodeId : route.nodeIds) {
                if (!reader.readU32(nodeId) || nodeId == 0) return false;
            }
            if (!reader.readU32(pointCount) || pointCount == 0 ||
                pointCount > sim::kMaximumArchiveEntries) {
                return false;
            }
            route.points.resize(pointCount);
            for (auto& point : route.points) {
                if (!readGridPoint(reader, point)) return false;
            }
            if (!route.nodeIds.empty() &&
                route.nodeIds.size() != route.points.size()) {
                return false;
            }
        }

        if (!reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
            return false;
        }
        value.spawns.resize(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            auto& spawn = value.spawns[index];
            if (!reader.readU32(spawn.sequence) ||
                !reader.readU64(spawn.tickOffset) ||
                !reader.readU32(spawn.laneId) ||
                !readGridPoint(reader, spawn.spawn) ||
                !readGridPoint(reader, spawn.goal) ||
                !reader.readU32(spawn.unitDefinitionId) ||
                spawn.sequence != index || spawn.laneId == 0 ||
                spawn.unitDefinitionId == 0 ||
                (index > 0 &&
                 spawn.tickOffset <= value.spawns[index - 1].tickOffset)) {
                return false;
            }
            const auto route = std::lower_bound(
                value.routes.begin(), value.routes.end(), spawn.laneId,
                [](const PlannedLaneRoute& current, LaneId id) {
                    return current.id < id;
                });
            if (route == value.routes.end() || route->id != spawn.laneId ||
                route->points.empty() ||
                route->points.front() != spawn.spawn ||
                route->points.back() != spawn.goal) {
                return false;
            }
        }
        return true;
    }

    static void writeWaveState(
        foundation::BinaryWriter& writer,
        const WaveState& value) {
        writer.writeU32(value.waveId);
        writer.writeU8(static_cast<std::uint8_t>(value.phase));
        writer.writeU64(value.startedTick);
        writer.writeU32(value.nextSpawn);
        writer.writeU32(value.spawned);
        writer.writeU32(value.resolved);
    }

    static bool readWaveState(
        foundation::BinaryReader& reader,
        WaveState& value) {
        std::uint8_t phase = 0;
        if (!reader.readU32(value.waveId) || !reader.readU8(phase) ||
            phase > static_cast<std::uint8_t>(WavePhase::Failed) ||
            !reader.readU64(value.startedTick) ||
            !reader.readU32(value.nextSpawn) ||
            !reader.readU32(value.spawned) ||
            !reader.readU32(value.resolved)) {
            return false;
        }
        value.phase = static_cast<WavePhase>(phase);
        return value.resolved <= value.spawned &&
               value.spawned == value.nextSpawn;
    }

    static void writeRewardOffer(
        foundation::BinaryWriter& writer,
        const RewardOffer& value) {
        writer.writeU32(value.waveId);
        writer.writeU32(static_cast<std::uint32_t>(value.choices.size()));
        for (const auto id : value.choices) writer.writeU32(id);
        writer.writeU32(value.selected);
        writer.writeBool(value.chosen);
    }

    static bool readRewardOffer(
        foundation::BinaryReader& reader,
        RewardOffer& value) {
        std::uint32_t count = 0;
        if (!reader.readU32(value.waveId) || !reader.readU32(count) ||
            count > sim::kMaximumArchiveEntries) {
            return false;
        }
        value.choices.resize(count);
        for (auto& id : value.choices) {
            if (!reader.readU32(id) || id == 0) return false;
        }
        if (!reader.readU32(value.selected) ||
            !reader.readBool(value.chosen)) {
            return false;
        }
        auto sorted = value.choices;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            return false;
        }
        return (!value.chosen && value.selected == 0) ||
               (value.chosen && value.selected != 0 &&
                std::find(value.choices.begin(), value.choices.end(),
                          value.selected) != value.choices.end());
    }

    static bool samePlan(const WavePlan& a, const WavePlan& b) {
        if (a.waveId != b.waveId || a.enemyTeamId != b.enemyTeamId ||
            a.unusedBudget != b.unusedBudget ||
            a.rewardChoices != b.rewardChoices ||
            a.rewards.size() != b.rewards.size() ||
            a.routes != b.routes || a.spawns != b.spawns) {
            return false;
        }
        for (std::size_t index = 0; index < a.rewards.size(); ++index) {
            const auto& left = a.rewards[index];
            const auto& right = b.rewards[index];
            if (left.id != right.id || left.weight != right.weight ||
                left.resourceGrant != right.resourceGrant) {
                return false;
            }
        }
        return true;
    }

    static bool sameState(const WaveState& a, const WaveState& b) {
        return a.waveId == b.waveId && a.phase == b.phase &&
               a.startedTick == b.startedTick &&
               a.nextSpawn == b.nextSpawn && a.spawned == b.spawned &&
               a.resolved == b.resolved;
    }

    static bool sameOffer(const RewardOffer& a, const RewardOffer& b) {
        return a.waveId == b.waveId && a.choices == b.choices &&
               a.selected == b.selected && a.chosen == b.chosen;
    }

    static bool rebuildDirector(
        WaveDirector& director,
        const WavePlan& plan,
        const WaveState& state,
        const RewardOffer& offer) {
        if (state.phase == WavePhase::Idle) {
            return state.waveId == 0 && plan.waveId == 0 &&
                   plan.routes.empty() && plan.spawns.empty() &&
                   offer.waveId == 0 && offer.choices.empty() && !offer.chosen;
        }
        if (state.waveId == 0 || plan.waveId != state.waveId ||
            state.nextSpawn > plan.spawns.size()) {
            return false;
        }
        const auto result = director.begin(state.waveId, state.startedTick);
        if (!result.accepted || !samePlan(director.plan(), plan)) {
            return false;
        }
        if (state.nextSpawn > 0) {
            const auto tick = state.startedTick +
                plan.spawns[state.nextSpawn - 1].tickOffset;
            const auto due = director.dueSpawns(tick);
            if (due.size() != state.nextSpawn ||
                !std::equal(due.begin(), due.end(), plan.spawns.begin())) {
                return false;
            }
        }
        for (std::uint32_t index = 0; index < state.resolved; ++index) {
            if (director.state().resolved >= director.state().spawned) {
                return false;
            }
            (void)director.markEnemyResolved();
        }
        if (state.phase == WavePhase::Failed) {
            if (!director.fail()) return false;
        } else if (state.phase == WavePhase::Complete && offer.chosen) {
            if (!director.chooseReward(offer.selected)) return false;
        }
        return samePlan(director.plan(), plan) &&
               sameState(director.state(), state) &&
               sameOffer(director.offer(), offer);
    }

    static bool validateRestoredState(
        const gameplay::RtsSimulation& rts,
        const WaveDirector& director,
        const std::vector<TowerDefenseSimulation::TrackedEnemy>& tracked,
        ecs::Entity core,
        std::uint32_t playerTeamId,
        std::uint64_t lastTick,
        bool hasStepped,
        bool coreFailureReported) {
        if (hasStepped && rts.lastCompletedTick() != lastTick) return false;
        if (core.valid()) {
            const auto* team = rts.world().try_get<gameplay::Team>(core);
            if (rts.world().alive(core)) {
                if (!team || team->id != playerTeamId ||
                    !rts.world().try_get<gameplay::Health>(core)) {
                    return false;
                }
                if (coreFailureReported) return false;
            } else if (!coreFailureReported &&
                       director.state().phase == WavePhase::Failed) {
                return false;
            }
        } else if (director.state().phase == WavePhase::Spawning ||
                   director.state().phase == WavePhase::Active ||
                   director.state().phase == WavePhase::RewardPending) {
            return false;
        }

        auto ordered = tracked;
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& a, const auto& b) {
                      return a.entity < b.entity;
                  });
        if (std::adjacent_find(
                ordered.begin(), ordered.end(),
                [](const auto& a, const auto& b) {
                    return a.entity == b.entity;
                }) != ordered.end()) {
            return false;
        }

        std::uint32_t resolved = 0;
        for (const auto& enemy : tracked) {
            const auto* route = director.plannedRoute(enemy.laneId);
            if (!route || route->points.empty() ||
                enemy.waypointIndex == 0 ||
                enemy.waypointIndex > route->points.size()) {
                return false;
            }
            if (!enemy.resolved && !rts.world().alive(enemy.entity)) {
                return false;
            }
            if (enemy.resolved && rts.world().alive(enemy.entity)) {
                return false;
            }
            if (enemy.resolved) {
                ++resolved;
                continue;
            }

            const auto* queue =
                rts.world().try_get<gameplay::OrderQueue>(enemy.entity);
            if (!queue) return false;
            const auto expectedCount =
                route->points.size() - enemy.waypointIndex;
            if (queue->pending.size() != expectedCount) return false;
            for (std::size_t index = 0; index < expectedCount; ++index) {
                const auto& order = queue->pending[index];
                const auto target = route->points[enemy.waypointIndex + index];
                if (order.type != gameplay::OrderType::AttackMove ||
                    order.target != target) {
                    return false;
                }
            }
        }
        if (director.state().phase != WavePhase::Failed &&
            (tracked.size() != director.state().spawned ||
             resolved != director.state().resolved)) {
            return false;
        }
        return true;
    }
};

inline std::vector<std::uint8_t> EncodeTowerDefenseSimulation(
    const TowerDefenseSimulation& simulation) {
    return TowerDefenseSimulationArchive::encode(simulation);
}

inline bool DecodeTowerDefenseSimulation(
    const std::vector<std::uint8_t>& bytes,
    TowerDefenseSimulation& simulation) {
    return TowerDefenseSimulationArchive::decode(bytes, simulation);
}

} // namespace rts::tower_defense
