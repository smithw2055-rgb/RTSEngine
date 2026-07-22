#pragma once

#include <RTSEngine/Roguelite/RunSimulationArchive.h>
#include <RTSEngine/Rts/Replay.h>
#include <RTSEngine/TowerDefense/Simulation.h>
#include <rts/foundation/Random.h>
#include <rts/sim/BinaryArchive.h>
#include <rts/sim/SessionSchema.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace rts::roguelite {

struct RunSaveSchema {
    static constexpr std::uint16_t kSchemaVersion = 2;
    static constexpr std::uint32_t kMaximumAuthoritativeStateBytes =
        512u * 1024u * 1024u;

    std::uint64_t tick{};
    std::uint64_t rootSeed{};
    RunState run{};
    gameplay::ResourceLedger resources{};
    gameplay::TeamModifierProfile gameplayProfile{};
    std::vector<ModifierStack> modifierStacks;
    std::vector<foundation::RandomStreamState> randomStreams;
    std::vector<sim::WorldHashCheckpoint> checkpoints;
    TickCommandStream::State runCommands;
    tower_defense::TickCommandStream::State towerCommands;
    gameplay::TickCommandStream::State rtsCommands;
    std::vector<std::uint8_t> authoritativeState;
};

inline void WriteRunCommand(
    sim::BinaryWriter& writer,
    const TickCommand& command) {
    writer.writeU64(command.targetTick);
    writer.writeU32(command.issuer);
    writer.writeU32(command.sequence);
    writer.writeU8(static_cast<std::uint8_t>(command.type));
    writer.writeU32(command.objectId);
}

inline bool ReadRunCommand(
    sim::BinaryReader& reader,
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

inline void WriteTowerDefenseCommand(
    sim::BinaryWriter& writer,
    const tower_defense::TickCommand& command) {
    writer.writeU64(command.targetTick);
    writer.writeU32(command.issuer);
    writer.writeU32(command.sequence);
    writer.writeU8(static_cast<std::uint8_t>(command.type));
    writer.writeU32(command.objectId);
}

inline bool ReadTowerDefenseCommand(
    sim::BinaryReader& reader,
    tower_defense::TickCommand& command) {
    std::uint8_t type = 0;
    if (!reader.readU64(command.targetTick) ||
        !reader.readU32(command.issuer) ||
        !reader.readU32(command.sequence) ||
        !reader.readU8(type) ||
        !reader.readU32(command.objectId) ||
        type > static_cast<std::uint8_t>(
            tower_defense::CommandType::ChooseReward)) {
        return false;
    }
    command.type = static_cast<tower_defense::CommandType>(type);
    return true;
}

inline void WriteTeamModifierProfile(
    sim::BinaryWriter& writer,
    const gameplay::TeamModifierProfile& profile) {
    writer.writeI32(profile.unitHealth);
    writer.writeI32(profile.unitDamage);
    writer.writeI32(profile.unitArmorAdd);
    writer.writeI32(profile.unitMoveSpeed);
    writer.writeI32(profile.buildingHealth);
    writer.writeI32(profile.buildingDamage);
    writer.writeI32(profile.constructionSpeed);
    writer.writeI32(profile.productionSpeed);
    writer.writeI32(profile.bountyMultiplier);
}

inline bool ReadTeamModifierProfile(
    sim::BinaryReader& reader,
    gameplay::TeamModifierProfile& profile) {
    if (!reader.readI32(profile.unitHealth) ||
        !reader.readI32(profile.unitDamage) ||
        !reader.readI32(profile.unitArmorAdd) ||
        !reader.readI32(profile.unitMoveSpeed) ||
        !reader.readI32(profile.buildingHealth) ||
        !reader.readI32(profile.buildingDamage) ||
        !reader.readI32(profile.constructionSpeed) ||
        !reader.readI32(profile.productionSpeed) ||
        !reader.readI32(profile.bountyMultiplier)) {
        return false;
    }
    return gameplay::SanitizeTeamModifierProfile(profile) == profile;
}

template<class State, class WriteCommand>
inline void WriteCommandStreamState(
    sim::BinaryWriter& writer,
    const State& state,
    WriteCommand writeCommand) {
    writer.writeU64(state.committedThrough);
    writer.writeU32(static_cast<std::uint32_t>(state.pending.size()));
    for (const auto& command : state.pending) writeCommand(writer, command);
}

template<class State, class ReadCommand>
inline bool ReadCommandStreamState(
    sim::BinaryReader& reader,
    State& state,
    ReadCommand readCommand) {
    std::uint32_t count = 0;
    if (!reader.readU64(state.committedThrough) ||
        !reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
        return false;
    }
    state.pending.resize(count);
    for (auto& command : state.pending) {
        if (!readCommand(reader, command) ||
            command.targetTick < state.committedThrough) {
            return false;
        }
    }
    return true;
}

inline std::vector<std::uint8_t> EncodeRunSave(
    const RunSaveSchema& save) {
    if (save.modifierStacks.size() > sim::kMaximumArchiveEntries ||
        save.randomStreams.size() > sim::kMaximumArchiveEntries ||
        save.checkpoints.size() > sim::kMaximumArchiveEntries ||
        save.runCommands.pending.size() > sim::kMaximumArchiveEntries ||
        save.towerCommands.pending.size() > sim::kMaximumArchiveEntries ||
        save.rtsCommands.pending.size() > sim::kMaximumArchiveEntries ||
        save.authoritativeState.size() >
            RunSaveSchema::kMaximumAuthoritativeStateBytes) {
        return {};
    }

    sim::BinaryWriter writer;
    sim::WriteSessionHeader(
        writer,
        {sim::kSessionArchiveMagic,
         RunSaveSchema::kSchemaVersion,
         sim::SessionArchiveKind::RogueliteRunSave});
    writer.writeU64(save.tick);
    writer.writeU64(save.rootSeed);
    writer.writeU32(save.run.runId);
    writer.writeU8(static_cast<std::uint8_t>(save.run.phase));
    writer.writeU32(save.run.waveIndex);
    writer.writeU32(save.run.completedWaves);
    writer.writeU32(save.run.currentWave);
    writer.writeI32(save.resources.available);
    writer.writeI32(save.resources.reserved);
    writer.writeI32(save.resources.spent);
    WriteTeamModifierProfile(writer, save.gameplayProfile);

    writer.writeU32(static_cast<std::uint32_t>(save.modifierStacks.size()));
    for (const auto& stack : save.modifierStacks) {
        writer.writeU32(stack.id);
        writer.writeU32(stack.stacks);
    }

    writer.writeU32(static_cast<std::uint32_t>(save.randomStreams.size()));
    for (const auto& state : save.randomStreams) {
        sim::WriteRandomStreamState(writer, state);
    }

    writer.writeU32(static_cast<std::uint32_t>(save.checkpoints.size()));
    for (const auto& checkpoint : save.checkpoints) {
        sim::WriteWorldHashCheckpoint(writer, checkpoint);
    }

    WriteCommandStreamState(writer, save.runCommands, WriteRunCommand);
    WriteCommandStreamState(
        writer, save.towerCommands, WriteTowerDefenseCommand);
    WriteCommandStreamState(
        writer, save.rtsCommands, gameplay::WriteTickCommand);

    writer.writeU32(static_cast<std::uint32_t>(save.authoritativeState.size()));
    writer.writeBytes(save.authoritativeState);
    return writer.take();
}

inline bool DecodeRunSave(
    const std::vector<std::uint8_t>& bytes,
    RunSaveSchema& save) {
    sim::BinaryReader reader(bytes);
    sim::SessionArchiveHeader header;
    std::uint8_t phase = 0;
    if (!sim::ReadSessionHeader(
            reader,
            sim::SessionArchiveKind::RogueliteRunSave,
            RunSaveSchema::kSchemaVersion,
            header) ||
        !reader.readU64(save.tick) ||
        !reader.readU64(save.rootSeed) ||
        !reader.readU32(save.run.runId) ||
        !reader.readU8(phase) ||
        phase > static_cast<std::uint8_t>(RunPhase::Failed) ||
        !reader.readU32(save.run.waveIndex) ||
        !reader.readU32(save.run.completedWaves) ||
        !reader.readU32(save.run.currentWave) ||
        !reader.readI32(save.resources.available) ||
        !reader.readI32(save.resources.reserved) ||
        !reader.readI32(save.resources.spent) ||
        save.resources.available < 0 || save.resources.reserved < 0 ||
        save.resources.spent < 0 ||
        !ReadTeamModifierProfile(reader, save.gameplayProfile)) {
        return false;
    }
    save.run.phase = static_cast<RunPhase>(phase);

    std::uint32_t count = 0;
    if (!reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
        return false;
    }
    save.modifierStacks.resize(count);
    ModifierId previousModifier = 0;
    for (auto& stack : save.modifierStacks) {
        if (!reader.readU32(stack.id) || !reader.readU32(stack.stacks) ||
            stack.id == 0 || stack.stacks == 0 ||
            stack.id <= previousModifier) {
            return false;
        }
        previousModifier = stack.id;
    }

    if (!reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
        return false;
    }
    save.randomStreams.resize(count);
    for (auto& state : save.randomStreams) {
        if (!sim::ReadRandomStreamState(reader, state)) return false;
    }

    if (!reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
        return false;
    }
    save.checkpoints.resize(count);
    for (auto& checkpoint : save.checkpoints) {
        if (!sim::ReadWorldHashCheckpoint(reader, checkpoint)) return false;
    }

    if (!ReadCommandStreamState(reader, save.runCommands, ReadRunCommand) ||
        !ReadCommandStreamState(
            reader, save.towerCommands, ReadTowerDefenseCommand) ||
        !ReadCommandStreamState(
            reader, save.rtsCommands, gameplay::ReadTickCommand)) {
        return false;
    }

    save.authoritativeState.clear();
    if (header.schemaVersion >= 2) {
        if (!reader.readU32(count) ||
            count > RunSaveSchema::kMaximumAuthoritativeStateBytes ||
            !reader.readBytes(
                count,
                save.authoritativeState,
                RunSaveSchema::kMaximumAuthoritativeStateBytes)) {
            return false;
        }
    }
    return reader.atEnd();
}

inline RunSaveSchema CaptureRunSave(
    const RunSimulation& simulation,
    std::vector<foundation::RandomStreamState> randomStreams = {},
    std::vector<sim::WorldHashCheckpoint> checkpoints = {}) {
    RunSaveSchema save;
    save.tick = simulation.lastTick();
    save.rootSeed = simulation.rootSeed();
    save.run = simulation.state();
    save.resources = simulation.tower().resources();
    save.gameplayProfile = ResolveGameplayProfile(simulation.modifiers());
    save.modifierStacks = simulation.modifiers().stacks();
    save.randomStreams = std::move(randomStreams);
    save.checkpoints = std::move(checkpoints);
    save.runCommands = simulation.commandStreamState();
    save.towerCommands = simulation.tower().commandStreamState();
    save.rtsCommands = simulation.tower().rts().commandStreamState();
    save.authoritativeState = EncodeRunSimulation(simulation);
    return save;
}

inline bool RestoreRunSave(
    const RunSaveSchema& save,
    RunSimulation& simulation) {
    return !save.authoritativeState.empty() &&
           DecodeRunSimulation(save.authoritativeState, simulation);
}

} // namespace rts::roguelite
