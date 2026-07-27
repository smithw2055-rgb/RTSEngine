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
        if (!normalize(replay_)) return {};
        return std::move(replay_);
    }

    static bool normalize(RtsReplay& replay) {
        if (!sortCommands(replay.commands) ||
            !sortCommands(replay.commandStream.pending) ||
            !sortRandomStreams(replay.randomStreams) ||
            !sortCheckpoints(replay.checkpoints)) {
            return false;
        }
        return true;
    }

private:
    static bool sameIdentity(
        const TickCommand& a,
        const TickCommand& b) noexcept {
        return a.targetTick == b.targetTick &&
               a.issuer == b.issuer &&
               a.sequence == b.sequence;
    }

    static bool samePayload(
        const TickCommand& a,
        const TickCommand& b) noexcept {
        return a.type == b.type &&
               a.subject == b.subject &&
               a.targetX == b.targetX &&
               a.targetY == b.targetY &&
               a.append == b.append &&
               a.definitionId == b.definitionId &&
               a.objectId == b.objectId &&
               a.targetEntity == b.targetEntity;
    }

    static bool sortCommands(std::vector<TickCommand>& commands) {
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

        auto output = commands.begin();
        for (auto current = commands.begin(); current != commands.end();
             ++current) {
            if (output != commands.begin() &&
                sameIdentity(*(output - 1), *current)) {
                if (!samePayload(*(output - 1), *current)) return false;
                continue;
            }
            if (output != current) *output = std::move(*current);
            ++output;
        }
        commands.erase(output, commands.end());
        return true;
    }

    static bool sortRandomStreams(
        std::vector<foundation::RandomStreamState>& streams) {
        std::sort(
            streams.begin(), streams.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
        auto output = streams.begin();
        for (auto current = streams.begin(); current != streams.end();
             ++current) {
            if (output != streams.begin() &&
                (output - 1)->id == current->id) {
                if (!(*(output - 1) == *current)) return false;
                continue;
            }
            if (output != current) *output = *current;
            ++output;
        }
        streams.erase(output, streams.end());
        return true;
    }

    static bool sortCheckpoints(
        std::vector<sim::WorldHashCheckpoint>& checkpoints) {
        std::sort(
            checkpoints.begin(), checkpoints.end(),
            [](const auto& a, const auto& b) {
                return a.tick < b.tick;
            });
        auto output = checkpoints.begin();
        for (auto current = checkpoints.begin(); current != checkpoints.end();
             ++current) {
            if (output != checkpoints.begin() &&
                (output - 1)->tick == current->tick) {
                if ((output - 1)->worldHash != current->worldHash) {
                    return false;
                }
                continue;
            }
            if (output != current) *output = *current;
            ++output;
        }
        checkpoints.erase(output, checkpoints.end());
        return true;
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
    if (rawType > static_cast<std::uint8_t>(CommandType::Gather)) {
        return false;
    }
    command.type = static_cast<CommandType>(rawType);
    return true;
}

inline std::vector<std::uint8_t> EncodeRtsReplay(RtsReplay replay) {
    if (!RtsReplayRecorder::normalize(replay)) return {};
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
    if (!reader.atEnd() || !RtsReplayRecorder::normalize(replay)) {
        return false;
    }
    return replay.firstTick <= replay.lastTick ||
           replay.checkpoints.empty();
}

} // namespace rts::gameplay
