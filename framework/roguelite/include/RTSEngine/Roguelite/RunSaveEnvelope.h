#pragma once

#include <RTSEngine/Roguelite/RunSaveSchema.h>
#include <rts/foundation/ArchiveChecksum.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/BinaryArchive.h>
#include <rts/sim/SessionSchema.h>

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace rts::roguelite {

inline std::uint64_t MakeSaveIdentifier(std::string_view value) noexcept {
    foundation::CanonicalHash hash;
    hash.WriteString(value);
    return hash.Value();
}

enum class RunSaveMigrationStatus : std::uint8_t {
    CurrentAuthoritative,
    CurrentSummaryOnly,
    MigratedLegacySummary,
    UnsupportedFuture,
    Corrupt
};

struct RunSaveMigrationResult final {
    RunSaveMigrationStatus status{RunSaveMigrationStatus::Corrupt};
    std::uint16_t sourceSchemaVersion{};
    bool resumable{};
    RunSaveSchema save;
    std::vector<std::uint8_t> currentBytes;
};

inline RunSaveMigrationResult MigrateRunSaveToCurrent(
    const std::vector<std::uint8_t>& bytes) {
    RunSaveMigrationResult result;
    sim::BinaryReader headerReader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t kind = 0;
    if (!headerReader.readU32(magic) ||
        !headerReader.readU16(version) ||
        !headerReader.readU16(kind) ||
        magic != sim::kSessionArchiveMagic || version == 0 ||
        kind != static_cast<std::uint16_t>(
            sim::SessionArchiveKind::RogueliteRunSave)) {
        return result;
    }

    result.sourceSchemaVersion = version;
    if (version > RunSaveSchema::kSchemaVersion) {
        result.status = RunSaveMigrationStatus::UnsupportedFuture;
        return result;
    }
    if (!DecodeRunSave(bytes, result.save)) {
        result.status = RunSaveMigrationStatus::Corrupt;
        return result;
    }

    result.resumable = !result.save.authoritativeState.empty();
    result.currentBytes = version == RunSaveSchema::kSchemaVersion
        ? bytes
        : EncodeRunSave(result.save);
    if (result.currentBytes.empty()) {
        result.status = RunSaveMigrationStatus::Corrupt;
        return result;
    }

    if (version < RunSaveSchema::kSchemaVersion) {
        // Version 1 never contained a complete authoritative world. Its
        // progression summary is preserved in a current-version container,
        // but it intentionally remains non-resumable.
        result.status = RunSaveMigrationStatus::MigratedLegacySummary;
        result.resumable = false;
    } else {
        result.status = result.resumable
            ? RunSaveMigrationStatus::CurrentAuthoritative
            : RunSaveMigrationStatus::CurrentSummaryOnly;
    }
    return result;
}

struct RunSaveManifestIdentity final {
    std::uint64_t productId{};
    std::uint64_t contentManifestId{};
    std::uint64_t buildId{};
};

struct RunSaveEnvelopeManifest final {
    RunSaveManifestIdentity identity;
    std::uint64_t sequence{};
    std::uint64_t saveTick{};
    std::uint16_t payloadSchemaVersion{};
    std::uint32_t flags{};
};

inline constexpr std::uint32_t kRunSaveFlagAuthoritative = 1u << 0u;
inline constexpr std::uint32_t kRunSaveFlagLegacySummary = 1u << 1u;
inline constexpr std::uint32_t kRunSaveKnownFlags =
    kRunSaveFlagAuthoritative | kRunSaveFlagLegacySummary;

struct RunSaveEnvelope final {
    RunSaveEnvelopeManifest manifest;
    RunSaveSchema save;
    std::vector<std::uint8_t> payload;
};

enum class RunSaveEnvelopeError : std::uint8_t {
    None,
    InvalidManifest,
    InvalidPayload,
    UnsupportedPayload,
    TooLarge,
    BadMagic,
    UnsupportedEnvelopeVersion,
    WrongKind,
    Truncated,
    InvalidFlags,
    ChecksumMismatch,
    ManifestPayloadMismatch,
    TrailingData
};

struct RunSaveEnvelopeBuildResult final {
    RunSaveEnvelopeError error{RunSaveEnvelopeError::InvalidPayload};
    RunSaveMigrationStatus migration{RunSaveMigrationStatus::Corrupt};
    bool resumable{};
    std::vector<std::uint8_t> bytes;
};

struct RunSaveEnvelopeDecodeResult final {
    RunSaveEnvelopeError error{RunSaveEnvelopeError::Truncated};
    RunSaveMigrationStatus migration{RunSaveMigrationStatus::Corrupt};
    bool resumable{};
    RunSaveEnvelope envelope;
};

class RunSaveEnvelopeCodec final {
public:
    static constexpr std::uint32_t kMagic = 0x45565352u; // "RSVE"
    static constexpr std::uint16_t kVersion = 1u;
    static constexpr std::uint16_t kKind = 1u;
    static constexpr std::uint32_t kMaximumPayloadBytes =
        RunSaveSchema::kMaximumAuthoritativeStateBytes + 16u * 1024u * 1024u;

    static RunSaveEnvelopeBuildResult build(
        RunSaveManifestIdentity identity,
        std::uint64_t sequence,
        const std::vector<std::uint8_t>& runSaveBytes) {
        RunSaveEnvelopeBuildResult result;
        if (identity.productId == 0 || sequence == 0) {
            result.error = RunSaveEnvelopeError::InvalidManifest;
            return result;
        }

        auto migration = MigrateRunSaveToCurrent(runSaveBytes);
        result.migration = migration.status;
        result.resumable = migration.resumable;
        if (migration.status == RunSaveMigrationStatus::UnsupportedFuture) {
            result.error = RunSaveEnvelopeError::UnsupportedPayload;
            return result;
        }
        if (migration.status == RunSaveMigrationStatus::Corrupt ||
            migration.currentBytes.empty()) {
            result.error = RunSaveEnvelopeError::InvalidPayload;
            return result;
        }
        if (migration.currentBytes.size() > kMaximumPayloadBytes) {
            result.error = RunSaveEnvelopeError::TooLarge;
            return result;
        }

        RunSaveEnvelopeManifest manifest;
        manifest.identity = identity;
        manifest.sequence = sequence;
        manifest.saveTick = migration.save.tick;
        manifest.payloadSchemaVersion = RunSaveSchema::kSchemaVersion;
        if (migration.resumable) {
            manifest.flags |= kRunSaveFlagAuthoritative;
        }
        if (migration.status == RunSaveMigrationStatus::MigratedLegacySummary) {
            manifest.flags |= kRunSaveFlagLegacySummary;
        }

        const auto protectedBytes = protectedFields(
            manifest, migration.currentBytes);
        const auto checksum = foundation::ArchiveChecksum(protectedBytes);

        sim::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU16(kKind);
        writeManifest(writer, manifest);
        writer.writeU32(static_cast<std::uint32_t>(
            migration.currentBytes.size()));
        writer.writeU64(checksum);
        writer.writeBytes(migration.currentBytes);

        result.error = RunSaveEnvelopeError::None;
        result.bytes = writer.take();
        return result;
    }

    static RunSaveEnvelopeDecodeResult decode(
        const std::vector<std::uint8_t>& bytes) {
        RunSaveEnvelopeDecodeResult result;
        sim::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint16_t kind = 0;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU16(kind)) {
            return result;
        }
        if (magic != kMagic) {
            result.error = RunSaveEnvelopeError::BadMagic;
            return result;
        }
        if (version == 0 || version > kVersion) {
            result.error = RunSaveEnvelopeError::UnsupportedEnvelopeVersion;
            return result;
        }
        if (kind != kKind) {
            result.error = RunSaveEnvelopeError::WrongKind;
            return result;
        }

        RunSaveEnvelopeManifest manifest;
        std::uint32_t payloadSize = 0;
        std::uint64_t storedChecksum = 0;
        if (!readManifest(reader, manifest) ||
            !reader.readU32(payloadSize) ||
            payloadSize > kMaximumPayloadBytes ||
            !reader.readU64(storedChecksum)) {
            result.error = payloadSize > kMaximumPayloadBytes
                ? RunSaveEnvelopeError::TooLarge
                : RunSaveEnvelopeError::Truncated;
            return result;
        }
        if (manifest.identity.productId == 0 || manifest.sequence == 0) {
            result.error = RunSaveEnvelopeError::InvalidManifest;
            return result;
        }
        if ((manifest.flags & ~kRunSaveKnownFlags) != 0) {
            result.error = RunSaveEnvelopeError::InvalidFlags;
            return result;
        }

        std::vector<std::uint8_t> payload;
        if (!reader.readBytes(payloadSize, payload, kMaximumPayloadBytes)) {
            result.error = RunSaveEnvelopeError::Truncated;
            return result;
        }
        if (!reader.atEnd()) {
            result.error = RunSaveEnvelopeError::TrailingData;
            return result;
        }

        const auto expectedChecksum = foundation::ArchiveChecksum(
            protectedFields(manifest, payload));
        if (storedChecksum != expectedChecksum) {
            result.error = RunSaveEnvelopeError::ChecksumMismatch;
            return result;
        }

        auto migration = MigrateRunSaveToCurrent(payload);
        result.migration = migration.status;
        result.resumable = migration.resumable;
        if (migration.status == RunSaveMigrationStatus::UnsupportedFuture) {
            result.error = RunSaveEnvelopeError::UnsupportedPayload;
            return result;
        }
        if (migration.status == RunSaveMigrationStatus::Corrupt) {
            result.error = RunSaveEnvelopeError::InvalidPayload;
            return result;
        }

        const bool authoritativeFlag =
            (manifest.flags & kRunSaveFlagAuthoritative) != 0;
        const bool legacyFlag =
            (manifest.flags & kRunSaveFlagLegacySummary) != 0;
        if (manifest.payloadSchemaVersion != migration.sourceSchemaVersion ||
            manifest.saveTick != migration.save.tick ||
            authoritativeFlag != migration.resumable ||
            (legacyFlag && migration.resumable)) {
            result.error = RunSaveEnvelopeError::ManifestPayloadMismatch;
            return result;
        }

        result.error = RunSaveEnvelopeError::None;
        result.envelope.manifest = manifest;
        result.envelope.save = std::move(migration.save);
        result.envelope.payload = std::move(payload);
        return result;
    }

private:
    static void writeManifest(
        sim::BinaryWriter& writer,
        const RunSaveEnvelopeManifest& manifest) {
        writer.writeU64(manifest.identity.productId);
        writer.writeU64(manifest.identity.contentManifestId);
        writer.writeU64(manifest.identity.buildId);
        writer.writeU64(manifest.sequence);
        writer.writeU64(manifest.saveTick);
        writer.writeU16(manifest.payloadSchemaVersion);
        writer.writeU32(manifest.flags);
    }

    static bool readManifest(
        sim::BinaryReader& reader,
        RunSaveEnvelopeManifest& manifest) {
        return reader.readU64(manifest.identity.productId) &&
               reader.readU64(manifest.identity.contentManifestId) &&
               reader.readU64(manifest.identity.buildId) &&
               reader.readU64(manifest.sequence) &&
               reader.readU64(manifest.saveTick) &&
               reader.readU16(manifest.payloadSchemaVersion) &&
               reader.readU32(manifest.flags);
    }

    static std::vector<std::uint8_t> protectedFields(
        const RunSaveEnvelopeManifest& manifest,
        const std::vector<std::uint8_t>& payload) {
        sim::BinaryWriter writer;
        writer.writeU16(kKind);
        writeManifest(writer, manifest);
        writer.writeU32(static_cast<std::uint32_t>(payload.size()));
        writer.writeBytes(payload);
        return writer.take();
    }
};

inline bool RunSaveManifestCompatible(
    const RunSaveEnvelopeManifest& manifest,
    RunSaveManifestIdentity expected,
    bool requireExactBuild = false) noexcept {
    return manifest.identity.productId == expected.productId &&
           manifest.identity.contentManifestId == expected.contentManifestId &&
           (!requireExactBuild ||
            manifest.identity.buildId == expected.buildId);
}

} // namespace rts::roguelite
