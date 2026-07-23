#pragma once

#include <RTSEngine/TowerDefense/SimulationArchive.h>
#include <RTSEngine/TowerDefense/WaveLoopSimulation.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/SessionSchema.h>

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::tower_defense {

class WaveLoopSimulationArchive final {
public:
    static constexpr std::uint32_t kMagic = 0x31504c57u; // "WLP1"
    static constexpr std::uint16_t kVersion = 1u;
    static constexpr std::uint32_t kMaximumTowerBytes =
        384u * 1024u * 1024u;

    static std::vector<std::uint8_t> encode(
        const WaveLoopSimulation& simulation) {
        const auto towerBytes =
            EncodeTowerDefenseSimulation(simulation.tower_);
        if (towerBytes.empty() || towerBytes.size() > kMaximumTowerBytes) {
            return {};
        }
        const auto commandState = simulation.commands_.snapshot();
        if (commandState.pending.size() > sim::kMaximumArchiveEntries ||
            simulation.sequence_.definitions().size() >
                sim::kMaximumArchiveEntries ||
            simulation.waveDefinitions_.size() > sim::kMaximumArchiveEntries ||
            simulation.rewardDefinitions_.size() >
                sim::kMaximumArchiveEntries) {
            return {};
        }

        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU64(contentHash(simulation));
        writer.writeU32(static_cast<std::uint32_t>(towerBytes.size()));
        writer.writeBytes(towerBytes);
        writeSequenceState(
            writer,
            simulation.sequence_.state(),
            simulation.sequence_.lastFailure());

        writer.writeU64(commandState.committedThrough);
        writer.writeU32(static_cast<std::uint32_t>(commandState.pending.size()));
        for (const auto& command : commandState.pending) {
            writeCommand(writer, command);
        }

        writer.writeU64(simulation.rootSeed_);
        writer.writeU64(simulation.nextInternalSequence_);
        writer.writeU64(simulation.lastTick_);
        writer.writeBool(simulation.hasStepped_);
        writer.writeU64(simulation.hasStepped_
            ? simulation.snapshot_.worldHash
            : 0u);
        return writer.take();
    }

    static bool decode(
        const std::vector<std::uint8_t>& bytes,
        WaveLoopSimulation& simulation) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint64_t storedContentHash = 0;
        std::uint32_t towerByteCount = 0;
        std::vector<std::uint8_t> towerBytes;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU64(storedContentHash) || magic != kMagic ||
            version != kVersion ||
            storedContentHash != contentHash(simulation) ||
            !reader.readU32(towerByteCount) ||
            towerByteCount > kMaximumTowerBytes ||
            !reader.readBytes(
                towerByteCount, towerBytes, kMaximumTowerBytes)) {
            return false;
        }

        WaveSequenceState sequenceState;
        WaveSequenceFailure sequenceFailure = WaveSequenceFailure::None;
        if (!readSequenceState(reader, sequenceState, sequenceFailure)) {
            return false;
        }

        WaveLoopCommandStream::State commandState;
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
        WaveLoopCommandStream commandCandidate;
        if (!commandCandidate.restore(commandState)) return false;

        std::uint64_t rootSeed = 0;
        std::uint64_t nextInternalSequence = 0;
        std::uint64_t lastTick = 0;
        bool hasStepped = false;
        std::uint64_t storedWorldHash = 0;
        if (!reader.readU64(rootSeed) ||
            !reader.readU64(nextInternalSequence) ||
            !reader.readU64(lastTick) ||
            !reader.readBool(hasStepped) ||
            !reader.readU64(storedWorldHash) || !reader.atEnd()) {
            return false;
        }
        constexpr auto maximumNextSequence =
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max()) + 1u;
        if (rootSeed != simulation.rootSeed_ ||
            nextInternalSequence == 0 ||
            nextInternalSequence > maximumNextSequence ||
            (hasStepped &&
             (lastTick == std::numeric_limits<std::uint64_t>::max() ||
              commandCandidate.committedThrough() != lastTick + 1u)) ||
            (!hasStepped &&
             (lastTick != 0 || commandCandidate.committedThrough() != 0 ||
              storedWorldHash != 0))) {
            return false;
        }

        WaveSequenceDirector sequenceCandidate;
        for (const auto& definition : simulation.sequence_.definitions()) {
            if (!sequenceCandidate.registerSequence(definition)) return false;
        }
        if (!sequenceCandidate.restore(sequenceState, sequenceFailure)) {
            return false;
        }

        const auto backupTower =
            EncodeTowerDefenseSimulation(simulation.tower_);
        if (backupTower.empty()) return false;
        const auto backupSequence = simulation.sequence_;
        const auto backupCommands = simulation.commands_;
        const auto backupEvents = simulation.events_;
        const auto backupSnapshot = simulation.snapshot_;
        const auto backupNextInternalSequence =
            simulation.nextInternalSequence_;
        const auto backupLastTick = simulation.lastTick_;
        const auto backupHasStepped = simulation.hasStepped_;

        if (!DecodeTowerDefenseSimulation(towerBytes, simulation.tower_)) {
            return false;
        }

        auto rollback = [&]() {
            (void)DecodeTowerDefenseSimulation(
                backupTower, simulation.tower_);
            simulation.sequence_ = backupSequence;
            simulation.commands_ = backupCommands;
            simulation.events_ = backupEvents;
            simulation.snapshot_ = backupSnapshot;
            simulation.nextInternalSequence_ = backupNextInternalSequence;
            simulation.lastTick_ = backupLastTick;
            simulation.hasStepped_ = backupHasStepped;
        };

        if ((hasStepped && simulation.tower_.lastTick() != lastTick) ||
            !validateAlignment(sequenceCandidate, simulation.tower_)) {
            rollback();
            return false;
        }

        simulation.sequence_ = std::move(sequenceCandidate);
        simulation.commands_ = std::move(commandCandidate);
        simulation.nextInternalSequence_ = nextInternalSequence;
        simulation.lastTick_ = lastTick;
        simulation.hasStepped_ = hasStepped;
        simulation.events_.clear();
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
        const WaveLoopSimulation& simulation) {
        return contentHash(simulation);
    }

private:
    static void writeCommand(
        foundation::BinaryWriter& writer,
        const WaveLoopCommand& command) {
        writer.writeU64(command.targetTick);
        writer.writeU32(command.issuer);
        writer.writeU32(command.sequence);
        writer.writeU8(static_cast<std::uint8_t>(command.type));
        writer.writeU32(command.objectId);
    }

    static bool readCommand(
        foundation::BinaryReader& reader,
        WaveLoopCommand& command) {
        std::uint8_t type = 0;
        if (!reader.readU64(command.targetTick) ||
            !reader.readU32(command.issuer) ||
            !reader.readU32(command.sequence) ||
            !reader.readU8(type) ||
            !reader.readU32(command.objectId) ||
            type > static_cast<std::uint8_t>(
                WaveLoopCommandType::ChooseReward)) {
            return false;
        }
        command.type = static_cast<WaveLoopCommandType>(type);
        return true;
    }

    static void writeSequenceState(
        foundation::BinaryWriter& writer,
        const WaveSequenceState& state,
        WaveSequenceFailure failure) {
        writer.writeU32(state.sequenceId);
        writer.writeU8(static_cast<std::uint8_t>(state.phase));
        writer.writeU32(state.waveIndex);
        writer.writeU32(state.completedWaves);
        writer.writeU32(state.currentWave);
        writer.writeU64(state.preparationStartedTick);
        writer.writeU64(state.scheduledStartTick);
        writer.writeU8(static_cast<std::uint8_t>(failure));
    }

    static bool readSequenceState(
        foundation::BinaryReader& reader,
        WaveSequenceState& state,
        WaveSequenceFailure& failure) {
        std::uint8_t phase = 0;
        std::uint8_t failureValue = 0;
        if (!reader.readU32(state.sequenceId) ||
            !reader.readU8(phase) ||
            phase > static_cast<std::uint8_t>(WaveSequencePhase::Failed) ||
            !reader.readU32(state.waveIndex) ||
            !reader.readU32(state.completedWaves) ||
            !reader.readU32(state.currentWave) ||
            !reader.readU64(state.preparationStartedTick) ||
            !reader.readU64(state.scheduledStartTick) ||
            !reader.readU8(failureValue) ||
            failureValue > static_cast<std::uint8_t>(
                WaveSequenceFailure::TickOverflow)) {
            return false;
        }
        state.phase = static_cast<WaveSequencePhase>(phase);
        failure = static_cast<WaveSequenceFailure>(failureValue);
        return (state.phase == WaveSequencePhase::Failed &&
                failure != WaveSequenceFailure::None) ||
               (state.phase != WaveSequencePhase::Failed &&
                failure == WaveSequenceFailure::None);
    }

    static bool validateAlignment(
        const WaveSequenceDirector& sequence,
        const TowerDefenseSimulation& tower) {
        const auto sequencePhase = sequence.state().phase;
        const auto wavePhase = tower.director().state().phase;
        switch (sequencePhase) {
        case WaveSequencePhase::Idle:
            return wavePhase == WavePhase::Idle;
        case WaveSequencePhase::Preparing:
            return wavePhase == WavePhase::Idle ||
                   wavePhase == WavePhase::Complete ||
                   wavePhase == WavePhase::Failed;
        case WaveSequencePhase::StartingWave:
            return false;
        case WaveSequencePhase::WaveActive:
            return tower.director().state().waveId ==
                       sequence.state().currentWave &&
                   (wavePhase == WavePhase::Spawning ||
                    wavePhase == WavePhase::Active);
        case WaveSequencePhase::RewardPending:
            return tower.director().state().waveId ==
                       sequence.state().currentWave &&
                   wavePhase == WavePhase::RewardPending;
        case WaveSequencePhase::Complete:
            return wavePhase == WavePhase::Complete;
        case WaveSequencePhase::Failed:
            return wavePhase == WavePhase::Idle ||
                   wavePhase == WavePhase::Complete ||
                   wavePhase == WavePhase::Failed;
        }
        return false;
    }

    static void hashWaveDefinition(
        foundation::CanonicalHash& hash,
        const WaveDefinition& value) {
        hash.WriteU32(value.id);
        hash.WriteU32(value.budget);
        hash.WriteU32(value.spawnIntervalTicks);
        hash.WriteU32(value.enemyTeamId);
        hash.WriteU32(static_cast<std::uint32_t>(value.laneIds.size()));
        for (const auto id : value.laneIds) hash.WriteU32(id);
        hash.WriteU32(static_cast<std::uint32_t>(value.enemies.size()));
        for (const auto& enemy : value.enemies) {
            hash.WriteU32(enemy.unitDefinitionId);
            hash.WriteU32(enemy.budgetCost);
            hash.WriteU32(enemy.weight);
            hash.WriteU32(enemy.maxPerWave);
        }
        hash.WriteU32(static_cast<std::uint32_t>(value.bossPool.size()));
        for (const auto id : value.bossPool) hash.WriteU32(id);
        hash.WriteU32(value.bossCount);
        hash.WriteU32(static_cast<std::uint32_t>(value.affixPool.size()));
        for (const auto id : value.affixPool) hash.WriteU32(id);
        hash.WriteU32(value.affixChoices);
        hash.WriteU32(static_cast<std::uint32_t>(value.rewardPool.size()));
        for (const auto id : value.rewardPool) hash.WriteU32(id);
        hash.WriteU32(value.rewardChoices);
    }

    static std::uint64_t contentHash(
        const WaveLoopSimulation& simulation) {
        foundation::CanonicalHash hash;
        hash.WriteU64(simulation.rootSeed_);
        hash.WriteU64(TowerDefenseSimulationArchive::configurationHash(
            simulation.tower_));
        hash.WriteU32(static_cast<std::uint32_t>(
            simulation.waveDefinitions_.size()));
        for (const auto& wave : simulation.waveDefinitions_) {
            hashWaveDefinition(hash, wave);
        }
        hash.WriteU32(static_cast<std::uint32_t>(
            simulation.rewardDefinitions_.size()));
        for (const auto& reward : simulation.rewardDefinitions_) {
            hash.WriteU32(reward.id);
            hash.WriteU32(reward.weight);
            hash.WriteI32(reward.resourceGrant);
        }
        hash.WriteU32(static_cast<std::uint32_t>(
            simulation.sequence_.definitions().size()));
        for (const auto& sequence : simulation.sequence_.definitions()) {
            hash.WriteU32(sequence.id);
            hash.WriteU32(static_cast<std::uint32_t>(sequence.waves.size()));
            for (const auto waveId : sequence.waves) hash.WriteU32(waveId);
            hash.WriteU32(sequence.initialPreparationTicks);
            hash.WriteU32(sequence.interWavePreparationTicks);
            hash.WriteBool(sequence.allowEarlyStart);
        }
        return hash.Value();
    }
};

inline std::vector<std::uint8_t> EncodeWaveLoopSimulation(
    const WaveLoopSimulation& simulation) {
    return WaveLoopSimulationArchive::encode(simulation);
}

inline bool DecodeWaveLoopSimulation(
    const std::vector<std::uint8_t>& bytes,
    WaveLoopSimulation& simulation) {
    return WaveLoopSimulationArchive::decode(bytes, simulation);
}

} // namespace rts::tower_defense
