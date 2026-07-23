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
    static constexpr std::uint16_t kVersion = 3u;
    static constexpr std::uint16_t kMinimumVersion = 1u;
    static constexpr std::uint32_t kMaximumTowerBytes =
        384u * 1024u * 1024u;

    static std::vector<std::uint8_t> encode(
        const RunSimulation& simulation) {
        const auto towerBytes =
            tower_defense::EncodeTowerDefenseSimulation(simulation.tower_);
        if (towerBytes.empty() || towerBytes.size() > kMaximumTowerBytes ||
            simulation.modifiers_.stacks().size() >
                sim::kMaximumArchiveEntries ||
            !archiveableHistory(simulation.history_)) {
            return {};
        }
        const auto commandState = simulation.commands_.snapshot();
        if (commandState.pending.size() > sim::kMaximumArchiveEntries) {
            return {};
        }

        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU64(contentHash(simulation, kVersion));
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
        writer.writeU64(simulation.legacyNextWaveTick());
        writer.writeU64(simulation.lastTick_);
        writer.writeU32(simulation.nextInternalSequence_);
        writer.writeU32(simulation.playerTeamId_);
        writer.writeBool(simulation.hasStepped_);
        writeHistory(writer, simulation.history_, kVersion);
        writer.writeU64(simulation.hasStepped_
            ? simulation.snapshot_.worldHash : 0u);
        return writer.take();
    }

    static bool decode(const std::vector<std::uint8_t>& bytes,
                       RunSimulation& simulation) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint64_t storedContentHash = 0;
        std::uint32_t towerByteCount = 0;
        std::vector<std::uint8_t> towerBytes;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU64(storedContentHash) || magic != kMagic ||
            version < kMinimumVersion || version > kVersion ||
            (version < 3u && !legacyRarityCompatible(simulation)) ||
            storedContentHash != contentHash(simulation, version) ||
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
            !reader.readBool(hasStepped)) {
            return false;
        }
        state.phase = static_cast<RunPhase>(phase);

        RunHistory historyCandidate;
        if (version >= 2u &&
            !readHistory(reader, historyCandidate, version)) {
            return false;
        }
        std::uint64_t storedWorldHash = 0;
        if (!reader.readU64(storedWorldHash) || !reader.atEnd()) return false;

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

        tower_defense::WaveSequenceDirector sequenceCandidate;
        for (const auto& run : simulation.runs_) {
            if (!sequenceCandidate.registerSequence(
                    RunSimulation::makeSequenceDefinition(run))) {
                return false;
            }
        }
        tower_defense::WaveSequenceState sequenceState;
        tower_defense::WaveSequenceFailure sequenceFailure =
            tower_defense::WaveSequenceFailure::None;
        if (!legacySequenceState(
                state, nextWaveTick, sequenceState, sequenceFailure) ||
            !sequenceCandidate.restore(sequenceState, sequenceFailure) ||
            !sameState(
                RunSimulation::projectSequenceState(sequenceCandidate.state()),
                state)) {
            return false;
        }

        const auto backupTower =
            tower_defense::EncodeTowerDefenseSimulation(simulation.tower_);
        if (backupTower.empty()) return false;
        const auto backupSequenceDirector = simulation.sequence_;
        const auto backupModifiers = simulation.modifiers_;
        const auto backupCommands = simulation.commands_;
        const auto backupEvents = simulation.events_;
        const auto backupSnapshot = simulation.snapshot_;
        const auto backupState = simulation.state_;
        const auto backupHistory = simulation.history_;
        const auto backupPendingWave = simulation.pendingWave_;
        const auto backupPendingReward = simulation.pendingRewardOffer_;
        const auto backupLastTick = simulation.lastTick_;
        const auto backupInternalSequence = simulation.nextInternalSequence_;
        const auto backupPlayerTeam = simulation.playerTeamId_;
        const auto backupHasStepped = simulation.hasStepped_;

        if (!tower_defense::DecodeTowerDefenseSimulation(
                towerBytes, simulation.tower_)) {
            return false;
        }

        auto rollback = [&]() {
            (void)tower_defense::DecodeTowerDefenseSimulation(
                backupTower, simulation.tower_);
            simulation.sequence_ = backupSequenceDirector;
            simulation.modifiers_ = backupModifiers;
            simulation.commands_ = backupCommands;
            simulation.events_ = backupEvents;
            simulation.snapshot_ = backupSnapshot;
            simulation.state_ = backupState;
            simulation.history_ = backupHistory;
            simulation.pendingWave_ = backupPendingWave;
            simulation.pendingRewardOffer_ = backupPendingReward;
            simulation.lastTick_ = backupLastTick;
            simulation.nextInternalSequence_ = backupInternalSequence;
            simulation.playerTeamId_ = backupPlayerTeam;
            simulation.hasStepped_ = backupHasStepped;
        };

        if ((hasStepped && simulation.tower_.lastTick() != lastTick) ||
            ResolveGameplayProfile(modifierCandidate) !=
                simulation.tower_.rts().teamModifierProfile(playerTeamId) ||
            !validateTowerAlignment(state, simulation.tower_.director()) ||
            legacyNextWaveTick(sequenceCandidate, simulation.tower_) !=
                nextWaveTick) {
            rollback();
            return false;
        }

        if (version == 1u) {
            historyCandidate = migrateLegacyHistory(
                state, lastTick, simulation.tower_);
        }
        if (!validateHistory(
                historyCandidate, state, simulation,
                simulation.tower_, version)) {
            rollback();
            return false;
        }

        simulation.sequence_ = std::move(sequenceCandidate);
        simulation.modifiers_ = std::move(modifierCandidate);
        simulation.commands_ = std::move(commandCandidate);
        simulation.history_ = std::move(historyCandidate);
        simulation.pendingWave_ = {};
        simulation.pendingRewardOffer_ = {};
        simulation.synchronizeRunState();
        if (!sameState(simulation.state_, state)) {
            rollback();
            return false;
        }
        simulation.lastTick_ = lastTick;
        simulation.nextInternalSequence_ = nextInternalSequence;
        simulation.playerTeamId_ = playerTeamId;
        simulation.hasStepped_ = hasStepped;
        simulation.events_.clear();
        simulation.snapshot_ = {};

        if (hasStepped) {
            simulation.buildSnapshot(lastTick, version);
            if (simulation.snapshot_.worldHash != storedWorldHash) {
                rollback();
                return false;
            }
            if (version < 3u) {
                migrateRarityHistory(simulation.history_, simulation.modifiers_);
                simulation.buildSnapshot(lastTick, kVersion);
            }
        } else if (simulation.snapshot_.worldHash != 0) {
            rollback();
            return false;
        }
        return true;
    }

private:
    static bool sameState(const RunState& a, const RunState& b) noexcept {
        return a.runId == b.runId && a.phase == b.phase &&
               a.waveIndex == b.waveIndex &&
               a.completedWaves == b.completedWaves &&
               a.currentWave == b.currentWave;
    }

    static void writeCommand(foundation::BinaryWriter& writer,
                             const TickCommand& command) {
        writer.writeU64(command.targetTick);
        writer.writeU32(command.issuer);
        writer.writeU32(command.sequence);
        writer.writeU8(static_cast<std::uint8_t>(command.type));
        writer.writeU32(command.objectId);
    }

    static bool readCommand(foundation::BinaryReader& reader,
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

    static bool archiveableHistory(const RunHistory& history) noexcept {
        if (history.waves.size() > sim::kMaximumArchiveEntries) return false;
        for (const auto& wave : history.waves) {
            if (wave.affixes.size() > sim::kMaximumArchiveEntries ||
                wave.bosses.size() > sim::kMaximumArchiveEntries ||
                wave.rewardChoices.size() > sim::kMaximumArchiveEntries ||
                wave.rewardRarities.size() > sim::kMaximumArchiveEntries) {
                return false;
            }
        }
        return true;
    }

    static void writeHistory(foundation::BinaryWriter& writer,
                             const RunHistory& history,
                             std::uint16_t version) {
        writer.writeU32(history.runId);
        writer.writeU64(history.startedTick);
        writer.writeU64(history.finishedTick);
        writer.writeU8(static_cast<std::uint8_t>(history.phase));
        writer.writeBool(history.legacyImported);
        if (version >= 3u) writer.writeU32(history.rewardPityMisses);
        writer.writeU32(static_cast<std::uint32_t>(history.waves.size()));
        for (const auto& wave : history.waves) {
            writer.writeU32(wave.waveId);
            writer.writeU32(wave.waveIndex);
            writer.writeU64(wave.startedTick);
            writer.writeU64(wave.completedTick);
            writer.writeU8(static_cast<std::uint8_t>(wave.phase));
            writer.writeU32(wave.plannedEnemies);
            writer.writeU32(wave.plannedBosses);
            writer.writeU32(wave.enemiesDefeated);
            writer.writeU32(wave.bossesDefeated);
            writer.writeI32(wave.coreHealthStart);
            writer.writeI32(wave.coreHealthEnd);
            writer.writeI32(wave.coreHealthMaximum);
            writer.writeI32(wave.resourcesStart);
            writer.writeI32(wave.resourcesEnd);
            writer.writeI32(wave.resourceDelta);
            writer.writeI32(wave.resourceBonus);
            writer.writeU32(static_cast<std::uint32_t>(wave.affixes.size()));
            for (const auto id : wave.affixes) writer.writeU32(id);
            writer.writeU32(static_cast<std::uint32_t>(wave.bosses.size()));
            for (const auto id : wave.bosses) writer.writeU32(id);
            writer.writeU32(static_cast<std::uint32_t>(
                wave.rewardChoices.size()));
            for (const auto id : wave.rewardChoices) writer.writeU32(id);
            writer.writeU32(wave.selectedModifier);
            writer.writeBool(wave.modifierApplied);
            if (version >= 3u) {
                writer.writeU32(wave.rewardRarityBudget);
                writer.writeU32(wave.rewardRaritySpent);
                writer.writeU8(static_cast<std::uint8_t>(
                    wave.guaranteedRarity));
                writer.writeU8(static_cast<std::uint8_t>(
                    wave.effectiveGuaranteedRarity));
                writer.writeU32(wave.pityBefore);
                writer.writeU32(wave.pityAfter);
                writer.writeBool(wave.pityTriggered);
                writer.writeU32(static_cast<std::uint32_t>(
                    wave.rewardRarities.size()));
                for (const auto rarity : wave.rewardRarities) {
                    writer.writeU8(static_cast<std::uint8_t>(rarity));
                }
            }
        }
    }

    static bool readHistory(foundation::BinaryReader& reader,
                            RunHistory& history,
                            std::uint16_t version) {
        std::uint8_t phase = 0;
        std::uint32_t waveCount = 0;
        if (!reader.readU32(history.runId) ||
            !reader.readU64(history.startedTick) ||
            !reader.readU64(history.finishedTick) ||
            !reader.readU8(phase) ||
            phase > static_cast<std::uint8_t>(RunHistoryPhase::Failed) ||
            !reader.readBool(history.legacyImported) ||
            (version >= 3u &&
             !reader.readU32(history.rewardPityMisses)) ||
            !reader.readU32(waveCount) ||
            waveCount > sim::kMaximumArchiveEntries) {
            return false;
        }
        history.phase = static_cast<RunHistoryPhase>(phase);
        history.waves.resize(waveCount);
        for (auto& wave : history.waves) {
            std::uint8_t wavePhase = 0;
            std::uint32_t count = 0;
            if (!reader.readU32(wave.waveId) ||
                !reader.readU32(wave.waveIndex) ||
                !reader.readU64(wave.startedTick) ||
                !reader.readU64(wave.completedTick) ||
                !reader.readU8(wavePhase) ||
                wavePhase > static_cast<std::uint8_t>(
                    WaveResultPhase::Failed) ||
                !reader.readU32(wave.plannedEnemies) ||
                !reader.readU32(wave.plannedBosses) ||
                !reader.readU32(wave.enemiesDefeated) ||
                !reader.readU32(wave.bossesDefeated) ||
                !reader.readI32(wave.coreHealthStart) ||
                !reader.readI32(wave.coreHealthEnd) ||
                !reader.readI32(wave.coreHealthMaximum) ||
                !reader.readI32(wave.resourcesStart) ||
                !reader.readI32(wave.resourcesEnd) ||
                !reader.readI32(wave.resourceDelta) ||
                !reader.readI32(wave.resourceBonus)) {
                return false;
            }
            wave.phase = static_cast<WaveResultPhase>(wavePhase);
            if (!readIds(reader, wave.affixes) ||
                !readIds(reader, wave.bosses) ||
                !readIds(reader, wave.rewardChoices) ||
                !reader.readU32(wave.selectedModifier) ||
                !reader.readBool(wave.modifierApplied)) {
                return false;
            }
            if (version >= 3u) {
                std::uint8_t guaranteed = 0;
                std::uint8_t effective = 0;
                if (!reader.readU32(wave.rewardRarityBudget) ||
                    !reader.readU32(wave.rewardRaritySpent) ||
                    !reader.readU8(guaranteed) ||
                    guaranteed > static_cast<std::uint8_t>(
                        RewardRarity::Legendary) ||
                    !reader.readU8(effective) ||
                    effective > static_cast<std::uint8_t>(
                        RewardRarity::Legendary) ||
                    !reader.readU32(wave.pityBefore) ||
                    !reader.readU32(wave.pityAfter) ||
                    !reader.readBool(wave.pityTriggered) ||
                    !reader.readU32(count) ||
                    count > sim::kMaximumArchiveEntries) {
                    return false;
                }
                wave.guaranteedRarity =
                    static_cast<RewardRarity>(guaranteed);
                wave.effectiveGuaranteedRarity =
                    static_cast<RewardRarity>(effective);
                wave.rewardRarities.resize(count);
                for (auto& rarity : wave.rewardRarities) {
                    std::uint8_t raw = 0;
                    if (!reader.readU8(raw) ||
                        raw > static_cast<std::uint8_t>(
                            RewardRarity::Legendary)) {
                        return false;
                    }
                    rarity = static_cast<RewardRarity>(raw);
                }
            }
        }
        return true;
    }

    template<class T>
    static bool readIds(foundation::BinaryReader& reader,
                        std::vector<T>& values) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) ||
            count > sim::kMaximumArchiveEntries) return false;
        values.resize(count);
        for (auto& id : values) {
            std::uint32_t raw = 0;
            if (!reader.readU32(raw) || raw == 0) return false;
            id = static_cast<T>(raw);
        }
        return true;
    }

    static bool restoreStacks(ModifierRuntime& runtime,
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

    static bool validateRunState(const RunSimulation& simulation,
                                 const RunState& state,
                                 std::uint64_t nextWaveTick) {
        if (state.phase == RunPhase::Idle) {
            return state.runId == 0 && state.waveIndex == 0 &&
                   state.completedWaves == 0 && state.currentWave == 0 &&
                   nextWaveTick == 0;
        }
        const auto* run = RunSimulation::findById(
            simulation.runs_, state.runId);
        if (!run || state.completedWaves != state.waveIndex ||
            state.waveIndex > run->waves.size()) return false;
        if (state.phase == RunPhase::Complete) {
            return state.waveIndex == run->waves.size() &&
                   state.currentWave == 0 &&
                   nextWaveTick ==
                       tower_defense::WaveSequenceDirector::noScheduledTick();
        }
        if (state.waveIndex >= run->waves.size() ||
            state.currentWave != run->waves[state.waveIndex]) return false;
        if (state.phase == RunPhase::BetweenWaves) {
            return nextWaveTick !=
                tower_defense::WaveSequenceDirector::noScheduledTick();
        }
        if (state.phase == RunPhase::WaveActive ||
            state.phase == RunPhase::RewardPending) {
            return nextWaveTick ==
                tower_defense::WaveSequenceDirector::noScheduledTick();
        }
        return true;
    }

    static bool legacySequenceState(
        const RunState& legacy,
        std::uint64_t nextWaveTick,
        tower_defense::WaveSequenceState& state,
        tower_defense::WaveSequenceFailure& failure) noexcept {
        state = {};
        failure = tower_defense::WaveSequenceFailure::None;
        if (legacy.phase == RunPhase::Idle) return nextWaveTick == 0;
        state.sequenceId = legacy.runId;
        state.waveIndex = legacy.waveIndex;
        state.completedWaves = legacy.completedWaves;
        state.currentWave = legacy.currentWave;
        const auto noTick =
            tower_defense::WaveSequenceDirector::noScheduledTick();
        switch (legacy.phase) {
        case RunPhase::Idle:
            return false;
        case RunPhase::BetweenWaves:
            if (nextWaveTick == noTick) return false;
            state.phase = tower_defense::WaveSequencePhase::Preparing;
            state.scheduledStartTick = nextWaveTick;
            state.preparationStartedTick = legacy.waveIndex == 0
                ? nextWaveTick
                : (nextWaveTick == 0 ? 0 : nextWaveTick - 1u);
            break;
        case RunPhase::WaveActive:
            state.phase = tower_defense::WaveSequencePhase::WaveActive;
            state.scheduledStartTick = noTick;
            break;
        case RunPhase::RewardPending:
            state.phase = tower_defense::WaveSequencePhase::RewardPending;
            state.scheduledStartTick = noTick;
            break;
        case RunPhase::Complete:
            state.phase = tower_defense::WaveSequencePhase::Complete;
            state.scheduledStartTick = noTick;
            break;
        case RunPhase::Failed:
            state.phase = tower_defense::WaveSequencePhase::Failed;
            state.scheduledStartTick = noTick;
            state.preparationStartedTick = nextWaveTick == noTick
                ? noTick
                : (legacy.waveIndex == 0
                       ? nextWaveTick
                       : (nextWaveTick == 0 ? 0 : nextWaveTick - 1u));
            failure = tower_defense::WaveSequenceFailure::WaveRejected;
            break;
        }
        return true;
    }

    static std::uint64_t legacyNextWaveTick(
        const tower_defense::WaveSequenceDirector& sequence,
        const tower_defense::TowerDefenseSimulation& tower) noexcept {
        const auto& value = sequence.state();
        const auto noTick =
            tower_defense::WaveSequenceDirector::noScheduledTick();
        switch (value.phase) {
        case tower_defense::WaveSequencePhase::Idle:
            return 0;
        case tower_defense::WaveSequencePhase::Preparing:
            return value.scheduledStartTick;
        case tower_defense::WaveSequencePhase::Failed:
            if (tower.director().state().phase ==
                    tower_defense::WavePhase::Failed ||
                value.preparationStartedTick == noTick) return noTick;
            return value.waveIndex == 0
                ? value.preparationStartedTick
                : value.preparationStartedTick + 1u;
        default:
            return noTick;
        }
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

    static RunHistory migrateLegacyHistory(
        const RunState& state,
        std::uint64_t lastTick,
        const tower_defense::TowerDefenseSimulation& tower) {
        RunHistory history;
        if (state.phase == RunPhase::Idle) return history;
        history.runId = state.runId;
        history.startedTick = 0;
        history.legacyImported = true;
        history.phase = state.phase == RunPhase::Complete
            ? RunHistoryPhase::Complete
            : (state.phase == RunPhase::Failed
                   ? RunHistoryPhase::Failed
                   : RunHistoryPhase::Active);
        if (history.phase != RunHistoryPhase::Active) {
            history.finishedTick = lastTick;
        }
        if (state.phase != RunPhase::WaveActive &&
            state.phase != RunPhase::RewardPending &&
            state.phase != RunPhase::Failed) return history;
        const auto& plan = tower.director().plan();
        if (state.currentWave == 0 || plan.waveId != state.currentWave) {
            return history;
        }

        WaveResult wave;
        wave.waveId = state.currentWave;
        wave.waveIndex = state.waveIndex;
        wave.startedTick = tower.director().state().startedTick;
        wave.completedTick = state.phase == RunPhase::RewardPending ||
                                     state.phase == RunPhase::Failed
            ? lastTick : 0;
        wave.phase = state.phase == RunPhase::RewardPending
            ? WaveResultPhase::RewardPending
            : (state.phase == RunPhase::Failed
                   ? WaveResultPhase::Failed
                   : WaveResultPhase::Active);
        wave.plannedEnemies = static_cast<std::uint32_t>(plan.spawns.size());
        wave.enemiesDefeated = tower.director().state().resolved;
        wave.coreHealthStart = tower.snapshot().coreHealthCurrent;
        wave.coreHealthEnd = tower.snapshot().coreHealthCurrent;
        wave.coreHealthMaximum = tower.snapshot().coreHealthMaximum;
        wave.resourcesStart = tower.resources().available;
        wave.resourcesEnd = tower.resources().available;
        for (const auto& affix : plan.affixes) wave.affixes.push_back(affix.id);
        for (const auto& spawn : plan.spawns) {
            if (spawn.bossId != 0) wave.bosses.push_back(spawn.bossId);
        }
        wave.plannedBosses = static_cast<std::uint32_t>(wave.bosses.size());
        for (const auto& enemy : tower.snapshot().enemies) {
            if (!enemy.alive && enemy.bossId != 0) ++wave.bossesDefeated;
        }
        wave.rewardChoices = tower.director().offer().choices;
        if (tower.director().offer().chosen) {
            wave.selectedModifier = tower.director().offer().selected;
            wave.modifierApplied = true;
            wave.phase = WaveResultPhase::Complete;
        }
        history.waves.push_back(std::move(wave));
        return history;
    }

    static void migrateRarityHistory(
        RunHistory& history,
        const ModifierRuntime& modifiers) {
        history.legacyImported = true;
        history.rewardPityMisses = 0;
        for (auto& wave : history.waves) {
            wave.rewardRarityBudget = 0;
            wave.rewardRaritySpent = 0;
            wave.guaranteedRarity = RewardRarity::Common;
            wave.effectiveGuaranteedRarity = RewardRarity::Common;
            wave.pityBefore = 0;
            wave.pityAfter = 0;
            wave.pityTriggered = false;
            wave.rewardRarities.clear();
            for (const auto id : wave.rewardChoices) {
                const auto* definition = modifiers.definition(id);
                wave.rewardRarities.push_back(
                    definition ? definition->rarity : RewardRarity::Common);
            }
        }
    }

    static bool validateHistory(
        const RunHistory& history,
        const RunState& state,
        const RunSimulation& simulation,
        const tower_defense::TowerDefenseSimulation& tower,
        std::uint16_t version) {
        if (!archiveableHistory(history)) return false;
        if (state.phase == RunPhase::Idle) {
            return history.runId == 0 && history.startedTick == 0 &&
                   history.finishedTick == 0 &&
                   history.phase == RunHistoryPhase::Idle &&
                   history.rewardPityMisses == 0 && history.waves.empty();
        }
        if (history.runId != state.runId) return false;
        const auto expectedPhase = state.phase == RunPhase::Complete
            ? RunHistoryPhase::Complete
            : (state.phase == RunPhase::Failed
                   ? RunHistoryPhase::Failed
                   : RunHistoryPhase::Active);
        if (history.phase != expectedPhase ||
            ((expectedPhase == RunHistoryPhase::Active) !=
             (history.finishedTick == 0))) return false;
        const auto* run = RunSimulation::findById(
            simulation.runs_, state.runId);
        if (!run) return false;

        std::uint32_t previousIndex = 0;
        bool hasPrevious = false;
        for (const auto& wave : history.waves) {
            if (wave.waveId == 0 || wave.waveIndex >= run->waves.size() ||
                run->waves[wave.waveIndex] != wave.waveId ||
                (hasPrevious && wave.waveIndex <= previousIndex) ||
                wave.plannedBosses != wave.bosses.size() ||
                wave.enemiesDefeated > wave.plannedEnemies ||
                wave.bossesDefeated > wave.plannedBosses ||
                wave.coreHealthStart < 0 || wave.coreHealthEnd < 0 ||
                wave.coreHealthMaximum < 0 || wave.resourcesStart < 0 ||
                wave.resourcesEnd < 0) return false;
            if (wave.phase == WaveResultPhase::Active) {
                if (wave.completedTick != 0 || wave.resourceDelta != 0 ||
                    wave.resourcesEnd != wave.resourcesStart) return false;
            } else {
                const auto delta = static_cast<std::int64_t>(
                    wave.resourcesEnd) - wave.resourcesStart;
                if (wave.completedTick < wave.startedTick ||
                    delta != wave.resourceDelta) return false;
            }
            if (wave.selectedModifier != 0 &&
                std::find(wave.rewardChoices.begin(),
                          wave.rewardChoices.end(),
                          wave.selectedModifier) ==
                    wave.rewardChoices.end()) return false;
            if (version >= 3u) {
                if (wave.rewardRarities.size() !=
                    wave.rewardChoices.size()) return false;
                std::uint64_t spent = 0;
                for (const auto rarity : wave.rewardRarities) {
                    if (!ValidRewardRarity(rarity)) return false;
                    spent += RewardRarityCost(rarity);
                }
                if (wave.rewardRarityBudget == 0) {
                    if (wave.rewardRaritySpent != 0 ||
                        wave.pityBefore != 0 || wave.pityAfter != 0 ||
                        wave.pityTriggered ||
                        wave.guaranteedRarity != RewardRarity::Common ||
                        wave.effectiveGuaranteedRarity !=
                            RewardRarity::Common) return false;
                } else if (spent != wave.rewardRaritySpent ||
                           spent > wave.rewardRarityBudget ||
                           !RewardRarityAtLeast(
                               wave.effectiveGuaranteedRarity,
                               wave.guaranteedRarity)) {
                    return false;
                }
            }
            previousIndex = wave.waveIndex;
            hasPrevious = true;
        }

        if (history.waves.empty()) {
            return history.legacyImported ||
                   state.phase == RunPhase::BetweenWaves;
        }
        const auto& last = history.waves.back();
        switch (state.phase) {
        case RunPhase::Idle:
            return false;
        case RunPhase::BetweenWaves:
            return last.phase == WaveResultPhase::Complete &&
                   last.waveIndex < state.waveIndex;
        case RunPhase::WaveActive:
            if (last.phase != WaveResultPhase::Active ||
                last.waveIndex != state.waveIndex ||
                last.waveId != state.currentWave) return false;
            break;
        case RunPhase::RewardPending:
            if (last.phase != WaveResultPhase::RewardPending ||
                last.waveIndex != state.waveIndex ||
                last.waveId != state.currentWave) return false;
            break;
        case RunPhase::Complete:
            return last.phase == WaveResultPhase::Complete;
        case RunPhase::Failed:
            if (last.phase != WaveResultPhase::Failed &&
                !history.legacyImported) return false;
            break;
        }

        if ((state.phase == RunPhase::WaveActive ||
             state.phase == RunPhase::RewardPending) &&
            tower.director().plan().waveId == state.currentWave) {
            const auto& plan = tower.director().plan();
            if (last.plannedEnemies != plan.spawns.size()) return false;
            std::vector<tower_defense::WaveAffixId> affixes;
            for (const auto& affix : plan.affixes) affixes.push_back(affix.id);
            std::vector<tower_defense::BossId> bosses;
            for (const auto& spawn : plan.spawns) {
                if (spawn.bossId != 0) bosses.push_back(spawn.bossId);
            }
            if (last.affixes != affixes || last.bosses != bosses) return false;
        }
        return true;
    }

    static bool legacyRarityCompatible(
        const RunSimulation& simulation) noexcept {
        for (const auto& definition : simulation.modifiers_.definitions()) {
            if (definition.rarity != RewardRarity::Common) return false;
        }
        return std::all_of(
            simulation.runs_.begin(), simulation.runs_.end(),
            [](const RunDefinition& run) {
                return run.rewardRules.empty();
            });
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

    static void hashModifierDefinition(
        foundation::CanonicalHash& hash,
        const ModifierDefinition& value,
        std::uint16_t version) {
        hash.WriteU32(value.id);
        hash.WriteU32(value.weight);
        hash.WriteU32(value.maxStacks);
        if (version >= 3u) {
            hash.WriteU8(static_cast<std::uint8_t>(value.rarity));
        }
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

    static std::uint64_t contentHash(
        const RunSimulation& simulation,
        std::uint16_t version) {
        foundation::CanonicalHash hash;
        hash.WriteU64(simulation.rootSeed_);
        hash.WriteU64(tower_defense::TowerDefenseSimulationArchive::
            configurationHash(simulation.tower_));
        hash.WriteU32(static_cast<std::uint32_t>(simulation.runs_.size()));
        for (const auto& run : simulation.runs_) {
            hash.WriteU32(run.id);
            hash.WriteU32(static_cast<std::uint32_t>(run.waves.size()));
            for (const auto wave : run.waves) hash.WriteU32(wave);
            if (version >= 3u) {
                hash.WriteU32(static_cast<std::uint32_t>(
                    run.rewardRules.size()));
                for (const auto& rule : run.rewardRules) {
                    hash.WriteU32(rule.rarityBudget);
                    hash.WriteU8(static_cast<std::uint8_t>(
                        rule.guaranteedRarity));
                    hash.WriteU32(rule.pityAfterMisses);
                    hash.WriteU8(static_cast<std::uint8_t>(
                        rule.pityRarity));
                }
            }
        }
        hash.WriteU32(static_cast<std::uint32_t>(simulation.waves_.size()));
        for (const auto& wave : simulation.waves_) {
            hashWaveDefinition(hash, wave);
        }
        hash.WriteU32(static_cast<std::uint32_t>(
            simulation.modifiers_.definitions().size()));
        for (const auto& definition : simulation.modifiers_.definitions()) {
            hashModifierDefinition(hash, definition, version);
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
