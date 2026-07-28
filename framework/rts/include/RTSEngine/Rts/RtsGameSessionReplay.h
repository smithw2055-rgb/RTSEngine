#pragma once

#include <RTSEngine/Rts/Replay.h>
#include <RTSEngine/Rts/RtsGameSessionArchive.h>
#include <rts/foundation/BinaryArchive.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct RtsGameSessionReplay final {
    static constexpr std::uint16_t kSchemaVersion = 1u;

    std::vector<std::uint8_t> initialState;
    RtsReplay timeline;
};

class RtsGameSessionReplayRecorder final {
public:
    bool begin(const RtsGameSession& session) {
        replay_.initialState = RtsGameSessionArchive::encode(session);
        return !replay_.initialState.empty();
    }

    void recordCommand(TickCommand command) {
        timeline_.recordCommand(std::move(command));
    }

    void recordCheckpoint(
        std::uint64_t tick,
        const RtsGameSession& session) {
        timeline_.recordCheckpoint(
            tick,
            RtsGameSessionArchive::authoritativeHash(session));
    }

    RtsGameSessionReplay finish() {
        replay_.timeline = timeline_.finish();
        if (replay_.initialState.empty() ||
            (replay_.timeline.checkpoints.empty() &&
             replay_.timeline.commands.empty())) {
            return {};
        }
        return std::move(replay_);
    }

private:
    RtsReplayRecorder timeline_;
    RtsGameSessionReplay replay_;
};

inline std::vector<std::uint8_t> EncodeRtsGameSessionReplay(
    RtsGameSessionReplay replay) {
    const auto timelineBytes = EncodeRtsReplay(std::move(replay.timeline));
    if (replay.initialState.empty() || timelineBytes.empty() ||
        replay.initialState.size() >
            RtsGameSessionArchive::kMaximumNestedBytes ||
        timelineBytes.size() > RtsGameSessionArchive::kMaximumNestedBytes) {
        return {};
    }

    foundation::BinaryWriter writer;
    writer.writeU32(0x31524752u); // "RGR1"
    writer.writeU16(RtsGameSessionReplay::kSchemaVersion);
    writer.writeU32(static_cast<std::uint32_t>(replay.initialState.size()));
    writer.writeBytes(replay.initialState);
    writer.writeU32(static_cast<std::uint32_t>(timelineBytes.size()));
    writer.writeBytes(timelineBytes);
    return writer.take();
}

inline bool DecodeRtsGameSessionReplay(
    const std::vector<std::uint8_t>& bytes,
    RtsGameSessionReplay& replay) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint32_t initialByteCount = 0;
    std::uint32_t timelineByteCount = 0;
    std::vector<std::uint8_t> timelineBytes;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        magic != 0x31524752u ||
        version != RtsGameSessionReplay::kSchemaVersion ||
        !reader.readU32(initialByteCount) || initialByteCount == 0 ||
        initialByteCount > RtsGameSessionArchive::kMaximumNestedBytes ||
        !reader.readBytes(
            initialByteCount,
            replay.initialState,
            RtsGameSessionArchive::kMaximumNestedBytes) ||
        !reader.readU32(timelineByteCount) || timelineByteCount == 0 ||
        timelineByteCount > RtsGameSessionArchive::kMaximumNestedBytes ||
        !reader.readBytes(
            timelineByteCount,
            timelineBytes,
            RtsGameSessionArchive::kMaximumNestedBytes) ||
        !reader.atEnd() ||
        !DecodeRtsReplay(timelineBytes, replay.timeline)) {
        return false;
    }
    return true;
}

} // namespace rts::gameplay
