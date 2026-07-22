#pragma once

#include <RTSEngine/Roguelite/RunSimulation.h>
#include <RTSEngine/TowerDefense/SimulationArchive.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/SessionSchema.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::roguelite {

class RunSimulationArchive final {
public:
    static constexpr std::uint32_t kMagic = 0x314e5552u; // "RUN1"
    static constexpr std::uint16_t kVersion = 1u;
    static constexpr std::uint32_t kMaximumTowerBytes = 384u * 1024u * 1024u;

    static std::vector<std::uint8_t> encode(
        const RunSimulation& simulation) {
        const auto towerBytes =
            tower_defense::EncodeTowerDefenseSimulation(simulation.tower_);
        if (towerBytes.empty() || towerBytes.size() > kMaximumTowerBytes ||
            simulation.modifiers_.stacks().size() > sim::kMaximumArchiveEntries) {
            return {};
        }
        const auto commandState = simulation.commands_.snapshot();
        if (commandState.pending.size() > sim::kMaximumArchiveEntries) return {};

        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU64(contentHash(simulation));
        writer.writeU32(static_cast<std::uint32_t>(towerBytes.size()));
        writer.writeBytes(towerBytes);

        writer.writeU32(static_cast<std::uint32_t>(
            simulation.modifiers_.stacks().size()));
        for (const auto& stack : simulation.modifiers_.stacks()) {
            writer.writeU32(stack.id);
            writer.writeU32(stack.stacks);
        }

        writer.writeU64(commandState.committedThrough);
        writer.writeU32(static_cast<std::uint32_t>(commandState.pending.size()));
        for (const auto& command : commandState.pending) {
            writeCommand(writer, command);
        }

        writer.writeU32(simulation.state_.runId);
        writer.writeU8(static_cast<std::uint8_t>(simulation.state_.phase));
        writer.writeU32(simulation.state_.waveIndex);
        writer.writeU32(simulation.state_.completedWaves);
        writer.writeU32(simulation.state_.currentWave);
        writer.writeU64(simulation.rootSeed_);
        writer.writeU64(simulation.nextWaveTick_);
        writer.writeU64(simulation.lastTick_);
        writer.writeU32(simulation.nextInternalSequence_);
        writer.writeU32(simulation.playerTeamId_);
        writer.writeBool(simulation.hasStepped_);
        writer.writeU64(simulation.hasStepped_
            ? simulation.snapshot_.worldHash
            : 0u);
        return writer.take();
    }

    static bool decode(
        const std::vector<std::uint8_t>& bytes,
        RunSimulation& simulation) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint64_t storedContentHash = 0;
        std::uint32_t towerByteCount = 0;
        std::vector<std::uint8_t> towerBytes;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU64(storedContentHash) || magic != kMagic ||
            version != kVersion || storedContentHash != contentHash(simulation) ||
            !reader.readU32(towerByteCount) ||
            towerByteCount > kMaximumTowerBytes ||
            !reader.readBytes(
                towerByteCount, towerBytes, kMaximumTowerBytes)) {
            return false;
        }

        std::uint32_t stackCount = 0;
        if (!reader.readU32(stackCount) ||
            stackCount > sim::kMaximumArchiveEntries) {
            return false;
        }
        std::vector<ModifierStack> stacks(stackCount);
        ModifierId previousModifier = 0;
        for (auto& stack : stacks) {
            if (!reader.readU32(stack.id) ||
                !reader.readU32(stack.stacks) || stack.id == 0 ||
                stack.stacks == 0 || stack.id <= previousModifier) {
                return false;
            }
            previousModifier = stack.id;
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

        RunState state;
        std::uint8_t phase = 0;
        std::uint64_t rootSeed = 0;
        std::uint64_t nextWaveTick = 0;
        std::uint64_t lastTick = 0;
        std::uint32_t nextInternalSequence = 0;
        std::uint32_t playerTeamId = 0;
        bool hasStepped = false;
        std::uint64_t storedWorldHash = 0;
        if (!reader.readU32(state.runId) || !reader.readU8(phase) ||
            phase > static_cast<std::uint8_t>(RunPhase::Failed) ||
            !reader.readU32(state.waveIndex) ||
            !reader.readU32(state.completedWaves) ||
            !reader.readU32(state.currentWave) ||
            !reader.readU64(rootSeed) ||
            !reader.readU64(nextWaveTick) ||
            !reader.readU64(lastTick) ||
            !reader.readU32(nextInternalSequence) ||
            !reader.readU32(playerTeamId) ||
            !reader.readBool(hasStepped) ||
            !reader.readU64(storedWorldHash) || !reader.atEnd()) {
            return false;
        }
        state.phase = static_cast<RunPhase>(phase);

        if (rootSeed != simulation.rootSeed_ || nextInternalSequence == 0 ||
            (hasStepped &&
             (lastTick == std::numeric_limits<std::uint64_t>::max() ||
              commandCandidate.committedThrough() != lastTick + 1u)) ||
            (!hasStepped &&
             (lastTick != 0 || commandCandidate.committedThrough() != 0 ||
              storedWorldHash != 0)) ||
            !validateRunState(simulation, state, nextWaveTick)) {
            return false;
        }

        ModifierRuntime modifierCandidate;
        for (const auto& definition : simulation.modifiers_.definitions()) {
            if (!modifierCandidate.registerDefinition(definition)) return false;
        }
        if (!restoreStacks(modifierCandidate, stacks)) return false;

        const auto backupTower =
            tower_defense::EncodeTowerDefenseSimulation(simulation.tower_);
        if (backupTower.empty()) return false;
        const auto backupModifiers = simulation.modifiers_;
        const auto backupCommands = simulation.commands_;
        const auto backupEvents = simulation.events_;
        const auto backupSnapshot = simulation.snapshot_;
        const auto backupState = simulation.state_;
        const auto backupNextWaveTick = simulation.nextWaveTick_;
        const auto backupLastTick = simulation.lastTick_;
        const auto backupSequence = simulation.nextInternalSequence_;
        const auto backupPlayerTeam = simulation.playerTeamId_;
        const auto backupHasStepped = simulation.hasStepped_;

        if (!tower_defense::DecodeTowerDefenseSimulation(
                towerBytes, simulation.tower_)) {
            return false;
        }

        auto rollback = [&]() {
            (void)tower_defense::DecodeTowerDefenseSimulation(
                backupTower, simulation.tower_);
            simulation.modifiers_ = backupModifiers;
            simulation.commands_ = backupCommands;
            simulation.events_ = backupEvents;
            simulation.snapshot_ = backupSnapshot;
            simulation.state_ = backupState;
            simulation.nextWaveTick_ = backupNextWaveTick;
            simulation.lastTick_ = backupLastTick;
            simulation.nextInternalSequence_ = backupSequence;
            simulation.playerTeamId_ = backupPlayerTeam;
            simulation.hasStepped_ = backupHasStepped;
        };

        if ((hasStepped && simulation.tower_.lastTick() != lastTick) ||
            ResolveGameplayProfile(modifierCandidate) !=
                simulation.tower_.rts().teamModifierProfile(playerTeamId) ||
            !validateTowerAlignment(state, simulation.tower_.director())) {
            rollback();
            return false;
        }

        simulation.modifiers_ = std::move(modifierCandidate);
        simulation.commands_ = std::move(commandCandidate);
        simulation.state_ = state;
        simulation.nextWaveTick_ = nextWaveTick;
        simulation.lastTick_ = lastTick;
        simulation.nextInternalSequence_ = nextInternalSequence;
        simulation.playerTeamId_ = playerTeamId;
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

private:
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
            type > static_cast<std::uint8_t>(CommandType::ChooseModifier)) {
            return false;
        }
        command.type = static_cast<CommandType>(type);
        return true;
    }

    static bool restoreStacks(
        ModifierRuntime& runtime,
        const std::vector<ModifierStack>& stacks) {
        std::vector<std::uint32_t> applied(stacks.size(), 0);
        std::uint64_t remaining = 0;
        for (const auto& stack : stacks) {
            const auto* definition = runtime.definition(stack.id);
            if (!definition || stack.stacks > definition->maxStacks) {
                return false;
            }
            remaining += stack.stacks;
        }

        while (remaining > 0) {
            bool progressed = false;
            for (std::size_t index = 0; index < stacks.size(); ++index) {
                if (applied[index] >= stacks[index].stacks) continue;
                if (runtime.canApply(stacks[index].id) == ApplyFailure::None) {
                    const auto result = runtime.apply(stacks[index].id);
                    if (!result.accepted) return false;
                    ++applied[index];
                    --remaining;
                    progressed = true;
                }
            }
            if (!progressed) return false;
        }
        return runtime.stacks() == stacks;
    }

    static bool validateRunState(
        const RunSimulation& simulation,
        const RunState& state,
        std::uint64_t nextWaveTick) {
        if (state.phase == RunPhase::Idle) {
            return state.runId == 0 && state.waveIndex == 0 &&
                   state.completedWaves == 0 && state.currentWave == 0;
        }
        const auto* run = RunSimulation::findById(simulation.runs_, state.runId);
        if (!run || state.completedWaves != state.waveIndex ||
            state.waveIndex > run->waves.size()) {
            return false;
        }
        if (state.phase == RunPhase::Complete) {
            return state.waveIndex == run->waves.size() &&
                   state.currentWave == 0;
        }
        if (state.waveIndex >= run->waves.size() ||
            state.currentWave != run->waves[state.waveIndex]) {
            return false;
        }
        if (state.phase == RunPhase::BetweenWaves) {
            return nextWaveTick != std::numeric_limits<std::uint64_t>::max();
        }
        if (state.phase == RunPhase::WaveActive ||
            state.phase == RunPhase::RewardPending) {
            return nextWaveTick == std::numeric_limits<std::uint64_t>::max();
        }
        return true;
    }

    static bool validateTowerAlignment(
        const RunState& state,
        const tower_defense::WaveDirector& director) {
        const auto phase = director.state().phase;
        switch (state.phase) {
        case RunPhase::Idle:
            return phase == tower_defense::WavePhase::Idle;
        case RunPhase::BetweenWaves:
            return phase == tower_defense::WavePhase::Idle ||
                   phase == tower_defense::WavePhase::Complete;
        case RunPhase::WaveActive:
            return director.state().waveId == state.currentWave &&
                   (phase == tower_defense::WavePhase::Spawning ||
                    phase == tower_defense::WavePhase::Active);
        case RunPhase::RewardPending:
            return director.state().waveId == state.currentWave &&
                   phase == tower_defense::WavePhase::RewardPending;
        case RunPhase::Complete:
            return phase == tower_defense::WavePhase::Complete;
        case RunPhase::Failed:
            return phase == tower_defense::WavePhase::Failed ||
                   phase == tower_defense::WavePhase::Idle;
        }
        return false;
    }

    static void hashCombatStats(
        foundation::CanonicalHash& hash,
        const gameplay::CombatStats& value) {
        hash.WriteI32(value.maximumHealth);
        hash.WriteI32(value.armor);
        hash.WriteI32(value.weaponDamage);
        hash.WriteI32(value.weaponRange);
        hash.WriteU32(value.cooldownTicks);
        hash.WriteI32(value.bounty);
    }

    static void hashWaveDefinition(
        foundation::CanonicalHash& hash,
        const tower_defense::WaveDefinition& value) {
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
        hash.WriteU32(static_cast<std::uint32_t>(value.rewardPool.size()));
        for (const auto id : value.rewardPool) hash.WriteU32(id);
        hash.WriteU32(value.rewardChoices);
    }

    static void hashModifierDefinition(
        foundation::CanonicalHash& hash,
        const ModifierDefinition& value) {
        hash.WriteU32(value.id);
        hash.WriteU32(value.weight);
        hash.WriteU32(value.maxStacks);
        const auto hashVector = [&hash](const auto& values) {
            hash.WriteU32(static_cast<std::uint32_t>(values.size()));
            for (const auto item : values) {
                if constexpr (sizeof(item) <= sizeof(std::uint32_t)) {
                    hash.WriteU32(static_cast<std::uint32_t>(item));
                } else {
                    hash.WriteU64(static_cast<std::uint64_t>(item));
                }
            }
        };
        hashVector(value.tags);
        hashVector(value.requiredModifiers);
        hashVector(value.requiredTags);
        hashVector(value.excludedModifiers);
        hashVector(value.excludedTags);
        hash.WriteU32(static_cast<std::uint32_t>(value.effects.size()));
        for (const auto& effect : value.effects) {
            hash.WriteU64(effect.stat);
            hash.WriteU8(static_cast<std::uint8_t>(effect.operation));
            hash.WriteI32(effect.value);
        }
    }

    static std::uint64_t contentHash(const RunSimulation& simulation) {
        foundation::CanonicalHash hash;
        hash.WriteU64(simulation.rootSeed_);
        hash.WriteU64(tower_defense::TowerDefenseSimulationArchive::
            configurationHash(simulation.tower_));
        hash.WriteU32(static_cast<std::uint32_t>(simulation.runs_.size()));
        for (const auto& run : simulation.runs_) {
            hash.WriteU32(run.id);
            hash.WriteU32(static_cast<std::uint32_t>(run.waves.size()));
            for (const auto wave : run.waves) hash.WriteU32(wave);
        }
        hash.WriteU32(static_cast<std::uint32_t>(simulation.waves_.size()));
        for (const auto& wave : simulation.waves_) hashWaveDefinition(hash, wave);
        hash.WriteU32(static_cast<std::uint32_t>(
            simulation.modifiers_.definitions().size()));
        for (const auto& definition : simulation.modifiers_.definitions()) {
            hashModifierDefinition(hash, definition);
        }
        return hash.Value();
    }
};

inline std::vector<std::uint8_t> EncodeRunSimulation(
    const RunSimulation& simulation) {
    return RunSimulationArchive::encode(simulation);
}

inline bool DecodeRunSimulation(
    const std::vector<std::uint8_t>& bytes,
    RunSimulation& simulation) {
    return RunSimulationArchive::decode(bytes, simulation);
}

} // namespace rts::roguelite
