#pragma once

#include <rts/foundation/Random.h>
#include <rts/sim/BinaryArchive.h>

#include <cstdint>

namespace rts::sim {

inline constexpr std::uint32_t kSessionArchiveMagic = 0x45535452u; // RTSE
inline constexpr std::uint32_t kMaximumArchiveEntries = 1'000'000u;

enum class SessionArchiveKind : std::uint16_t {
    RtsReplay = 1,
    RogueliteRunSave = 2
};

struct SessionArchiveHeader {
    std::uint32_t magic{kSessionArchiveMagic};
    std::uint16_t schemaVersion{1};
    SessionArchiveKind kind{SessionArchiveKind::RtsReplay};
};

struct WorldHashCheckpoint {
    std::uint64_t tick{};
    std::uint64_t worldHash{};

    friend bool operator==(
        const WorldHashCheckpoint& a,
        const WorldHashCheckpoint& b) noexcept {
        return a.tick == b.tick && a.worldHash == b.worldHash;
    }
};

inline void WriteSessionHeader(
    BinaryWriter& writer,
    SessionArchiveHeader header) {
    writer.writeU32(header.magic);
    writer.writeU16(header.schemaVersion);
    writer.writeU16(static_cast<std::uint16_t>(header.kind));
}

inline bool ReadSessionHeader(
    BinaryReader& reader,
    SessionArchiveKind expectedKind,
    std::uint16_t maximumVersion,
    SessionArchiveHeader& header) {
    std::uint16_t rawKind = 0;
    if (!reader.readU32(header.magic) ||
        !reader.readU16(header.schemaVersion) ||
        !reader.readU16(rawKind)) {
        return false;
    }
    header.kind = static_cast<SessionArchiveKind>(rawKind);
    return header.magic == kSessionArchiveMagic &&
           header.schemaVersion > 0 &&
           header.schemaVersion <= maximumVersion &&
           header.kind == expectedKind;
}

inline void WriteWorldHashCheckpoint(
    BinaryWriter& writer,
    const WorldHashCheckpoint& checkpoint) {
    writer.writeU64(checkpoint.tick);
    writer.writeU64(checkpoint.worldHash);
}

inline bool ReadWorldHashCheckpoint(
    BinaryReader& reader,
    WorldHashCheckpoint& checkpoint) {
    return reader.readU64(checkpoint.tick) &&
           reader.readU64(checkpoint.worldHash);
}

inline void WriteRandomStreamState(
    BinaryWriter& writer,
    const foundation::RandomStreamState& state) {
    writer.writeU64(state.id);
    writer.writeU64(state.state);
    writer.writeU64(state.increment);
}

inline bool ReadRandomStreamState(
    BinaryReader& reader,
    foundation::RandomStreamState& state) {
    return reader.readU64(state.id) &&
           reader.readU64(state.state) &&
           reader.readU64(state.increment) &&
           (state.increment & 1u) != 0;
}

} // namespace rts::sim
