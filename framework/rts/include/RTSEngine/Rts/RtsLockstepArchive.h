#pragma once

#include <RTSEngine/Rts/RtsLockstepSession.h>
#include <RTSEngine/Rts/Replay.h>
#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <vector>

namespace rts::gameplay {

inline constexpr std::uint32_t kRtsLockstepFrameMagic = 0x31464C52u;
inline constexpr std::uint32_t kRtsLockstepHashMagic = 0x31484C52u;
inline constexpr std::uint32_t kRtsReconnectMagic = 0x31524C52u;
inline constexpr std::uint16_t kRtsLockstepArchiveVersion = 1u;
inline constexpr std::uint32_t kMaximumLockstepPeers = 256u;
inline constexpr std::uint32_t kMaximumReconnectFrames = 1'000'000u;

namespace rts_lockstep_archive_detail {

inline void WriteConfig(
    foundation::BinaryWriter& writer,
    const RtsLockstepConfig& value) {
    writer.writeU64(value.sessionId);
    writer.writeU32(value.inputDelayTicks);
    writer.writeU32(value.maximumPredictionTicks);
    writer.writeU32(value.checkpointIntervalTicks);
    writer.writeU32(value.checkpointCapacity);
    writer.writeU32(value.hashExchangeIntervalTicks);
    writer.writeU32(value.maximumCommandsPerFrame);
}

inline bool ReadConfig(
    foundation::BinaryReader& reader,
    RtsLockstepConfig& value) {
    return reader.readU64(value.sessionId) &&
           reader.readU32(value.inputDelayTicks) &&
           reader.readU32(value.maximumPredictionTicks) &&
           reader.readU32(value.checkpointIntervalTicks) &&
           reader.readU32(value.checkpointCapacity) &&
           reader.readU32(value.hashExchangeIntervalTicks) &&
           reader.readU32(value.maximumCommandsPerFrame) &&
           value.sessionId != 0 &&
           value.checkpointIntervalTicks != 0 &&
           value.checkpointCapacity != 0 &&
           value.hashExchangeIntervalTicks != 0 &&
           value.maximumCommandsPerFrame != 0;
}

inline void WritePeer(
    foundation::BinaryWriter& writer,
    const sim::LockstepPeer& value) {
    writer.writeU32(value.peerId);
    writer.writeU32(value.playerSlot);
    writer.writeU32(value.issuer);
    writer.writeU8(static_cast<std::uint8_t>(value.role));
    writer.writeBool(value.active);
}

inline bool ReadPeer(
    foundation::BinaryReader& reader,
    sim::LockstepPeer& value) {
    std::uint8_t rawRole = 0;
    if (!reader.readU32(value.peerId) ||
        !reader.readU32(value.playerSlot) ||
        !reader.readU32(value.issuer) ||
        !reader.readU8(rawRole) ||
        !reader.readBool(value.active) ||
        value.peerId == 0 ||
        rawRole > static_cast<std::uint8_t>(
            sim::LockstepPeerRole::Spectator)) {
        return false;
    }
    value.role = static_cast<sim::LockstepPeerRole>(rawRole);
    return value.role == sim::LockstepPeerRole::Player
        ? value.playerSlot != 0 && value.issuer != 0
        : value.issuer == 0;
}

inline void WriteFrameBody(
    foundation::BinaryWriter& writer,
    const RtsLockstepFrame& frame) {
    writer.writeU64(frame.sessionId);
    writer.writeU64(frame.tick);
    writer.writeU32(frame.peerId);
    writer.writeU64(frame.frameSequence);
    writer.writeU32(static_cast<std::uint32_t>(frame.commands.size()));
    for (const auto& command : frame.commands) {
        WriteTickCommand(writer, command);
    }
}

inline bool ReadFrameBody(
    foundation::BinaryReader& reader,
    std::uint32_t maximumCommands,
    RtsLockstepFrame& frame) {
    std::uint32_t count = 0;
    if (!reader.readU64(frame.sessionId) ||
        !reader.readU64(frame.tick) ||
        !reader.readU32(frame.peerId) ||
        !reader.readU64(frame.frameSequence) ||
        !reader.readU32(count) ||
        frame.sessionId == 0 || frame.peerId == 0 ||
        count > maximumCommands) {
        return false;
    }
    frame.commands.resize(count);
    for (auto& command : frame.commands) {
        if (!ReadTickCommand(reader, command) ||
            command.targetTick != frame.tick ||
            command.sequence == 0) {
            return false;
        }
    }
    return true;
}

inline bool ValidatePeers(std::vector<sim::LockstepPeer>& peers) {
    std::sort(
        peers.begin(), peers.end(),
        [](const sim::LockstepPeer& first, const sim::LockstepPeer& second) {
            return first.peerId < second.peerId;
        });
    for (std::size_t index = 1; index < peers.size(); ++index) {
        if (peers[index - 1].peerId == peers[index].peerId) return false;
    }
    return !peers.empty();
}

inline bool ValidateSequences(
    std::vector<RtsLockstepPeerSequence>& sequences) {
    std::sort(
        sequences.begin(), sequences.end(),
        [](const auto& first, const auto& second) {
            return first.peerId < second.peerId;
        });
    for (std::size_t index = 0; index < sequences.size(); ++index) {
        if (sequences[index].peerId == 0 ||
            sequences[index].nextFrameSequence == 0 ||
            sequences[index].nextCommandSequence == 0 ||
            (index != 0 &&
             sequences[index - 1].peerId == sequences[index].peerId)) {
            return false;
        }
    }
    return true;
}

inline bool ValidateFrames(
    sim::LockstepSessionId sessionId,
    std::uint64_t nextTick,
    std::vector<RtsLockstepFrame>& frames) {
    std::sort(
        frames.begin(), frames.end(),
        [](const auto& first, const auto& second) {
            return std::make_tuple(first.tick, first.peerId) <
                   std::make_tuple(second.tick, second.peerId);
        });
    for (std::size_t index = 0; index < frames.size(); ++index) {
        if (frames[index].sessionId != sessionId ||
            frames[index].tick < nextTick ||
            (index != 0 &&
             frames[index - 1].tick == frames[index].tick &&
             frames[index - 1].peerId == frames[index].peerId)) {
            return false;
        }
    }
    return true;
}

} // namespace rts_lockstep_archive_detail

inline std::vector<std::uint8_t> EncodeRtsLockstepFrame(
    const RtsLockstepFrame& frame,
    std::uint32_t maximumCommands = 4096u) {
    if (frame.sessionId == 0 || frame.peerId == 0 ||
        frame.commands.size() > maximumCommands) {
        return {};
    }
    foundation::BinaryWriter writer;
    writer.writeU32(kRtsLockstepFrameMagic);
    writer.writeU16(kRtsLockstepArchiveVersion);
    rts_lockstep_archive_detail::WriteFrameBody(writer, frame);
    return writer.take();
}

inline bool DecodeRtsLockstepFrame(
    const std::vector<std::uint8_t>& bytes,
    RtsLockstepFrame& frame,
    std::uint32_t maximumCommands = 4096u) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    return reader.readU32(magic) && reader.readU16(version) &&
           magic == kRtsLockstepFrameMagic &&
           version == kRtsLockstepArchiveVersion &&
           rts_lockstep_archive_detail::ReadFrameBody(
               reader, maximumCommands, frame) &&
           reader.atEnd();
}

inline std::vector<std::uint8_t> EncodeRtsStateHashReport(
    const sim::StateHashReport& report) {
    if (report.sessionId == 0 || report.peerId == 0) return {};
    foundation::BinaryWriter writer;
    writer.writeU32(kRtsLockstepHashMagic);
    writer.writeU16(kRtsLockstepArchiveVersion);
    writer.writeU64(report.sessionId);
    writer.writeU32(report.peerId);
    writer.writeU64(report.tick);
    writer.writeU64(report.authoritativeHash);
    return writer.take();
}

inline bool DecodeRtsStateHashReport(
    const std::vector<std::uint8_t>& bytes,
    sim::StateHashReport& report) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    return reader.readU32(magic) && reader.readU16(version) &&
           magic == kRtsLockstepHashMagic &&
           version == kRtsLockstepArchiveVersion &&
           reader.readU64(report.sessionId) &&
           reader.readU32(report.peerId) &&
           reader.readU64(report.tick) &&
           reader.readU64(report.authoritativeHash) &&
           report.sessionId != 0 && report.peerId != 0 &&
           reader.atEnd();
}

inline std::vector<std::uint8_t> EncodeRtsReconnectSnapshot(
    const RtsReconnectSnapshot& snapshot) {
    if (snapshot.config.sessionId == 0 ||
        snapshot.sessionArchive.empty() ||
        snapshot.peers.size() > kMaximumLockstepPeers ||
        snapshot.sequences.size() > kMaximumLockstepPeers ||
        snapshot.futureFrames.size() > kMaximumReconnectFrames ||
        snapshot.sessionArchive.size() >
            RtsGameSessionArchive::kMaximumNestedBytes) {
        return {};
    }

    foundation::BinaryWriter writer;
    writer.writeU32(kRtsReconnectMagic);
    writer.writeU16(kRtsLockstepArchiveVersion);
    rts_lockstep_archive_detail::WriteConfig(writer, snapshot.config);
    writer.writeU64(snapshot.nextTick);
    writer.writeU64(snapshot.authoritativeHash);

    writer.writeU32(static_cast<std::uint32_t>(snapshot.peers.size()));
    for (const auto& peer : snapshot.peers) {
        rts_lockstep_archive_detail::WritePeer(writer, peer);
    }

    writer.writeU32(static_cast<std::uint32_t>(snapshot.sequences.size()));
    for (const auto& sequence : snapshot.sequences) {
        writer.writeU32(sequence.peerId);
        writer.writeU64(sequence.nextFrameSequence);
        writer.writeU32(sequence.nextCommandSequence);
    }

    writer.writeU32(static_cast<std::uint32_t>(
        snapshot.futureFrames.size()));
    for (const auto& frame : snapshot.futureFrames) {
        rts_lockstep_archive_detail::WriteFrameBody(writer, frame);
    }

    writer.writeU32(static_cast<std::uint32_t>(
        snapshot.sessionArchive.size()));
    writer.writeBytes(snapshot.sessionArchive);
    return writer.take();
}

inline bool DecodeRtsReconnectSnapshot(
    const std::vector<std::uint8_t>& bytes,
    RtsReconnectSnapshot& snapshot) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint32_t count = 0;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        magic != kRtsReconnectMagic ||
        version != kRtsLockstepArchiveVersion ||
        !rts_lockstep_archive_detail::ReadConfig(reader, snapshot.config) ||
        !reader.readU64(snapshot.nextTick) ||
        !reader.readU64(snapshot.authoritativeHash) ||
        !reader.readU32(count) || count > kMaximumLockstepPeers) {
        return false;
    }

    snapshot.peers.resize(count);
    for (auto& peer : snapshot.peers) {
        if (!rts_lockstep_archive_detail::ReadPeer(reader, peer)) {
            return false;
        }
    }
    if (!rts_lockstep_archive_detail::ValidatePeers(snapshot.peers) ||
        !reader.readU32(count) || count > kMaximumLockstepPeers) {
        return false;
    }

    snapshot.sequences.resize(count);
    for (auto& sequence : snapshot.sequences) {
        if (!reader.readU32(sequence.peerId) ||
            !reader.readU64(sequence.nextFrameSequence) ||
            !reader.readU32(sequence.nextCommandSequence)) {
            return false;
        }
    }
    if (!rts_lockstep_archive_detail::ValidateSequences(
            snapshot.sequences) ||
        !reader.readU32(count) || count > kMaximumReconnectFrames) {
        return false;
    }

    snapshot.futureFrames.resize(count);
    for (auto& frame : snapshot.futureFrames) {
        if (!rts_lockstep_archive_detail::ReadFrameBody(
                reader,
                snapshot.config.maximumCommandsPerFrame,
                frame)) {
            return false;
        }
    }
    if (!rts_lockstep_archive_detail::ValidateFrames(
            snapshot.config.sessionId,
            snapshot.nextTick,
            snapshot.futureFrames)) {
        return false;
    }

    std::uint32_t archiveSize = 0;
    if (!reader.readU32(archiveSize) || archiveSize == 0 ||
        archiveSize > RtsGameSessionArchive::kMaximumNestedBytes ||
        !reader.readBytes(
            archiveSize,
            snapshot.sessionArchive,
            RtsGameSessionArchive::kMaximumNestedBytes) ||
        !reader.atEnd()) {
        return false;
    }
    return true;
}

} // namespace rts::gameplay
