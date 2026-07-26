#pragma once

#include <RTSEngine/Rts/SimulationTypes.h>
#include <rts/foundation/Random.h>
#include <rts/sim/BinaryArchive.h>
#include <rts/sim/SessionSchema.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::gameplay {

struct RtsReplay {
    static constexpr std::uint16_t kSchemaVersion = 1;

    std::uint64_t firstTick{};
    std::uint64_t lastTick{};
    std::vector<TickCommand> commands;
    TickCommandStream::State commandStream;
    std::vector<foundation::RandomStreamState> randomStreams;
    std::vector<sim::WorldHashCheckpoint> checkpoints;
};

class RtsReplayRecorder final {
public:
    void recordCommand(TickCommand command) {
        replay_.commands.push_back(std::move(command));
    }

    void recordCheckpoint(std::uint64_t tick, std::uint64_t worldHash) {
        replay_.checkpoints.push_back({tick, worldHash});
        if (replay_.checkpoints.size() == 1) {
            replay_.firstTick = tick;
        }
        replay_.lastTick = tick;
    }

    void setCommandStreamState(TickCommandStream::State state) {
        replay_.commandStream = std::move(state);
    }

    void setRandomStreams(
        std::vector<foundation::RandomStreamState> states) {
        replay_.randomStreams = std::move(states);
    }

    RtsReplay finish() {
        normalize(replay_);
        return std::move(replay_);
    }

    static void normalize(RtsReplay& replay) {
        sortCommands(replay.commands);
        sortCommands(replay.commandStream.pending);
        std::sort(
            replay.randomStreams.begin(),
            replay.randomStreams.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
        replay.randomStreams.erase(
            std::unique(
                replay.randomStreams.begin(),
                replay.randomStreams.end(),
                [](const auto& a, const auto& b) {
                    return a.id == b.id;
                }),
            replay.randomStreams.end());
        std::sort(
            replay.checkpoints.begin(),
            replay.checkpoints.end(),
            [](const auto& a, const auto& b) {
                return a.tick < b.tick;
            });
        replay.checkpoints.erase(
            std::unique(
                replay.checkpoints.begin(),
                replay.checkpoints.end(),
                [](const auto& a, const auto& b) {
                    return a.tick == b.tick;
                }),
            replay.checkpoints.end());
    }

private:
    static void sortCommands(std::vector<TickCommand>& commands) {
        std::stable_sort(
            commands.begin(),
            commands.end(),
            [](const TickCommand& a, const TickCommand& b) {
                if (a.targetTick != b.targetTick) {
                    return a.targetTick < b.targetTick;
                }
                if (a.issuer != b.issuer) return a.issuer < b.issuer;
                return a.sequence < b.sequence;
            });
        commands.erase(
            std::unique(
                commands.begin(),
                commands.end(),
                [](const TickCommand& a, const TickCommand& b) {
                    return a.targetTick == b.targetTick &&
                           a.issuer == b.issuer &&
                           a.sequence == b.sequence;
                }),
            commands.end());
    }

    RtsReplay replay_;
};

inline void WriteTickCommand(
    sim::BinaryWriter& writer,
    const TickCommand& command) {
    writer.writeU64(command.targetTick);
    writer.writeU32(command.issuer);
    writer.writeU32(command.sequence);
    writer.writeU8(static_cast<std::uint8_t>(command.type));
    writer.writeU32(command.subject.index);
    writer.writeU32(command.subject.generation);
    writer.writeI32(command.targetX);
    writer.writeI32(command.targetY);
    writer.writeBool(command.append);
    writer.writeU32(command.definitionId);
    writer.writeU32(command.objectId);
    writer.writeU32(command.targetEntity.index);
    writer.writeU32(command.targetEntity.generation);
}

inline bool ReadTickCommand(
    sim::BinaryReader& reader,
    TickCommand& command) {
    std::uint8_t rawType = 0;
    if (!reader.readU64(command.targetTick) ||
        !reader.readU32(command.issuer) ||
        !reader.readU32(command.sequence) ||
        !reader.readU8(rawType) ||
        !reader.readU32(command.subject.index) ||
        !reader.readU32(command.subject.generation) ||
        !reader.readI32(command.targetX) ||
        !reader.readI32(command.targetY) ||
        !reader.readBool(command.append) ||
        !reader.readU32(command.definitionId) ||
        !reader.readU32(command.objectId) ||
        !reader.readU32(command.targetEntity.index) ||
        !reader.readU32(command.targetEntity.generation)) {
        return false;
    }
    if (rawType > static_cast<std::uint8_t>(CommandType::HoldPosition)) {
        return false;
    }
    command.type = static_cast<CommandType>(rawType);
    return true;
}

inline std::vector<std::uint8_t> EncodeRtsReplay(RtsReplay replay) {
    RtsReplayRecorder::normalize(replay);
    sim::BinaryWriter writer;
    sim::WriteSessionHeader(
        writer,
        {sim::kSessionArchiveMagic,
         RtsReplay::kSchemaVersion,
         sim::SessionArchiveKind::RtsReplay});
    writer.writeU64(replay.firstTick);
    writer.writeU64(replay.lastTick);

    writer.writeU32(static_cast<std::uint32_t>(replay.commands.size()));
    for (const auto& command : replay.commands) {
        WriteTickCommand(writer, command);
    }

    writer.writeU64(replay.commandStream.committedThrough);
    writer.writeU32(static_cast<std::uint32_t>(
        replay.commandStream.pending.size()));
    for (const auto& command : replay.commandStream.pending) {
        WriteTickCommand(writer, command);
    }

    writer.writeU32(static_cast<std::uint32_t>(
        replay.randomStreams.size()));
    for (const auto& state : replay.randomStreams) {
        sim::WriteRandomStreamState(writer, state);
    }

    writer.writeU32(static_cast<std::uint32_t>(
        replay.checkpoints.size()));
    for (const auto& checkpoint : replay.checkpoints) {
        sim::WriteWorldHashCheckpoint(writer, checkpoint);
    }
    return writer.take();
}

inline bool DecodeRtsReplay(
    const std::vector<std::uint8_t>& bytes,
    RtsReplay& replay) {
    sim::BinaryReader reader(bytes);
    sim::SessionArchiveHeader header;
    if (!sim::ReadSessionHeader(
            reader,
            sim::SessionArchiveKind::RtsReplay,
            RtsReplay::kSchemaVersion,
            header) ||
        !reader.readU64(replay.firstTick) ||
        !reader.readU64(replay.lastTick)) {
        return false;
    }

    std::uint32_t count = 0;
    if (!reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
        return false;
    }
    replay.commands.clear();
    replay.commands.resize(count);
    for (auto& command : replay.commands) {
        if (!ReadTickCommand(reader, command)) return false;
    }

    if (!reader.readU64(replay.commandStream.committedThrough) ||
        !reader.readU32(count) ||
        count > sim::kMaximumArchiveEntries) {
        return false;
    }
    replay.commandStream.pending.clear();
    replay.commandStream.pending.resize(count);
    for (auto& command : replay.commandStream.pending) {
        if (!ReadTickCommand(reader, command) ||
            command.targetTick < replay.commandStream.committedThrough) {
            return false;
        }
    }

    if (!reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
        return false;
    }
    replay.randomStreams.clear();
    replay.randomStreams.resize(count);
    for (auto& state : replay.randomStreams) {
        if (!sim::ReadRandomStreamState(reader, state)) return false;
    }

    if (!reader.readU32(count) || count > sim::kMaximumArchiveEntries) {
        return false;
    }
    replay.checkpoints.clear();
    replay.checkpoints.resize(count);
    for (auto& checkpoint : replay.checkpoints) {
        if (!sim::ReadWorldHashCheckpoint(reader, checkpoint)) {
            return false;
        }
    }
    if (!reader.atEnd()) return false;
    RtsReplayRecorder::normalize(replay);
    return replay.firstTick <= replay.lastTick ||
           replay.checkpoints.empty();
}

} // namespace rts::gameplay
