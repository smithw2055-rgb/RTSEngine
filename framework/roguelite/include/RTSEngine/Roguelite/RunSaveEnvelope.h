#pragma once

#include <RTSEngine/Roguelite/RunSaveCloudRevision.h>
#include <RTSEngine/Roguelite/RunSaveSchema.h>
#include <rts/foundation/ArchiveChecksum.h>
#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/BinaryArchive.h>
#include <rts/sim/SessionSchema.h>

#include <algorithm>
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
    RunSaveCloudRevision cloud;
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
inline constexpr std::uint32_t kRunSaveFlagCloudRevision = 1u << 2u;
inline constexpr std::uint32_t kRunSaveKnownFlagsV1 =
    kRunSaveFlagAuthoritative | kRunSaveFlagLegacySummary;
inline constexpr std::uint32_t kRunSaveKnownFlags =
    kRunSaveKnownFlagsV1 | kRunSaveFlagCloudRevision;

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
    static constexpr std::uint16_t kVersion = 2u;
    static constexpr std::uint16_t kMinimumVersion = 1u;
    static constexpr std::uint16_t kKind = 1u;
    static constexpr std::uint32_t kMaximumPayloadBytes =
        RunSaveSchema::kMaximumAuthoritativeStateBytes +
        16u * 1024u * 1024u;

    static RunSaveEnvelopeBuildResult build(
        RunSaveManifestIdentity identity,
        std::uint64_t sequence,
        const std::vector<std::uint8_t>& runSaveBytes) {
        RunSaveEnvelopeBuildResult result;
        if (identity.productId == 0 || sequence == 0 ||
            !ValidateRunSaveCloudRevision(identity.cloud, true)) {
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
        manifest.identity = std::move(identity);
        manifest.sequence = sequence;
        manifest.saveTick = migration.save.tick;
        manifest.payloadSchemaVersion = RunSaveSchema::kSchemaVersion;
        if (migration.resumable) {
            manifest.flags |= kRunSaveFlagAuthoritative;
        }
        if (migration.status == RunSaveMigrationStatus::MigratedLegacySummary) {
            manifest.flags |= kRunSaveFlagLegacySummary;
        }
        if (manifest.identity.cloud.tracked()) {
            const auto expectedRevision = computeRevisionId(
                manifest, migration.currentBytes);
            if (manifest.identity.cloud.revisionId != 0 &&
                manifest.identity.cloud.revisionId != expectedRevision) {
                result.error = RunSaveEnvelopeError::InvalidManifest;
                return result;
            }
            manifest.identity.cloud.revisionId = expectedRevision;
            if (!ValidateRunSaveCloudRevision(manifest.identity.cloud)) {
                result.error = RunSaveEnvelopeError::InvalidManifest;
                return result;
            }
            manifest.flags |= kRunSaveFlagCloudRevision;
        }

        const auto protectedBytes = protectedFields(
            kVersion, manifest, migration.currentBytes);
        const auto checksum = foundation::ArchiveChecksum(protectedBytes);

        sim::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU16(kKind);
        writeManifest(writer, manifest, kVersion);
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
        if (version < kMinimumVersion || version > kVersion) {
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
        if (!readManifest(reader, manifest, version) ||
            !reader.readU32(payloadSize) ||
            payloadSize > kMaximumPayloadBytes ||
            !reader.readU64(storedChecksum)) {
            result.error = payloadSize > kMaximumPayloadBytes
                ? RunSaveEnvelopeError::TooLarge
                : RunSaveEnvelopeError::Truncated;
            return result;
        }
        if (manifest.identity.productId == 0 || manifest.sequence == 0 ||
            !ValidateRunSaveCloudRevision(manifest.identity.cloud)) {
            result.error = RunSaveEnvelopeError::InvalidManifest;
            return result;
        }
        const auto knownFlags = version == 1u
            ? kRunSaveKnownFlagsV1
            : kRunSaveKnownFlags;
        if ((manifest.flags & ~knownFlags) != 0) {
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
            protectedFields(version, manifest, payload));
        if (storedChecksum != expectedChecksum) {
            result.error = RunSaveEnvelopeError::ChecksumMismatch;
            return result;
        }

        const bool cloudFlag =
            (manifest.flags & kRunSaveFlagCloudRevision) != 0;
        if (cloudFlag != manifest.identity.cloud.tracked()) {
            result.error = RunSaveEnvelopeError::ManifestPayloadMismatch;
            return result;
        }
        if (manifest.identity.cloud.tracked() &&
            manifest.identity.cloud.revisionId !=
                computeRevisionId(manifest, payload)) {
            result.error = RunSaveEnvelopeError::ManifestPayloadMismatch;
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
        result.envelope.manifest = std::move(manifest);
        result.envelope.save = std::move(migration.save);
        result.envelope.payload = std::move(payload);
        return result;
    }

private:
    static void writeCloudRevision(
        sim::BinaryWriter& writer,
        const RunSaveCloudRevision& value) {
        writer.writeU64(value.lineageId);
        writer.writeU64(value.revisionId);
        writer.writeU64(value.deviceId);
        writer.writeU64(value.logicalClock);
        writer.writeU8(static_cast<std::uint8_t>(
            value.parentRevisionIds.size()));
        for (const auto parent : value.parentRevisionIds) {
            writer.writeU64(parent);
        }
        writer.writeU8(static_cast<std::uint8_t>(value.vectorClock.size()));
        for (const auto& entry : value.vectorClock) {
            writer.writeU64(entry.deviceId);
            writer.writeU64(entry.counter);
        }
    }

    static bool readCloudRevision(
        sim::BinaryReader& reader,
        RunSaveCloudRevision& value) {
        std::uint8_t parentCount = 0;
        std::uint8_t clockCount = 0;
        if (!reader.readU64(value.lineageId) ||
            !reader.readU64(value.revisionId) ||
            !reader.readU64(value.deviceId) ||
            !reader.readU64(value.logicalClock) ||
            !reader.readU8(parentCount) ||
            parentCount > RunSaveCloudRevision::kMaximumParents) {
            return false;
        }
        value.parentRevisionIds.resize(parentCount);
        for (auto& parent : value.parentRevisionIds) {
            if (!reader.readU64(parent)) return false;
        }
        if (!reader.readU8(clockCount) ||
            clockCount > RunSaveCloudRevision::kMaximumDevices) {
            return false;
        }
        value.vectorClock.resize(clockCount);
        for (auto& entry : value.vectorClock) {
            if (!reader.readU64(entry.deviceId) ||
                !reader.readU64(entry.counter)) {
                return false;
            }
        }
        return true;
    }

    static void writeManifest(
        sim::BinaryWriter& writer,
        const RunSaveEnvelopeManifest& manifest,
        std::uint16_t version) {
        writer.writeU64(manifest.identity.productId);
        writer.writeU64(manifest.identity.contentManifestId);
        writer.writeU64(manifest.identity.buildId);
        writer.writeU64(manifest.sequence);
        writer.writeU64(manifest.saveTick);
        writer.writeU16(manifest.payloadSchemaVersion);
        writer.writeU32(manifest.flags);
        if (version >= 2u) {
            writeCloudRevision(writer, manifest.identity.cloud);
        }
    }

    static bool readManifest(
        sim::BinaryReader& reader,
        RunSaveEnvelopeManifest& manifest,
        std::uint16_t version) {
        if (!reader.readU64(manifest.identity.productId) ||
            !reader.readU64(manifest.identity.contentManifestId) ||
            !reader.readU64(manifest.identity.buildId) ||
            !reader.readU64(manifest.sequence) ||
            !reader.readU64(manifest.saveTick) ||
            !reader.readU16(manifest.payloadSchemaVersion) ||
            !reader.readU32(manifest.flags)) {
            return false;
        }
        return version < 2u ||
               readCloudRevision(reader, manifest.identity.cloud);
    }

    static std::vector<std::uint8_t> protectedFields(
        std::uint16_t version,
        const RunSaveEnvelopeManifest& manifest,
        const std::vector<std::uint8_t>& payload) {
        sim::BinaryWriter writer;
        if (version >= 2u) writer.writeU16(version);
        writer.writeU16(kKind);
        writeManifest(writer, manifest, version);
        writer.writeU32(static_cast<std::uint32_t>(payload.size()));
        writer.writeBytes(payload);
        return writer.take();
    }

    static std::uint64_t computeRevisionId(
        const RunSaveEnvelopeManifest& manifest,
        const std::vector<std::uint8_t>& payload) noexcept {
        const auto& cloud = manifest.identity.cloud;
        if (!cloud.tracked()) return 0;
        foundation::CanonicalHash hash;
        hash.WriteString("run-save.cloud-revision.v1");
        hash.WriteU64(manifest.identity.productId);
        hash.WriteU64(manifest.identity.contentManifestId);
        hash.WriteU64(cloud.lineageId);
        hash.WriteU64(cloud.deviceId);
        hash.WriteU64(cloud.logicalClock);
        hash.WriteU64(manifest.saveTick);
        hash.WriteU32(static_cast<std::uint32_t>(
            cloud.parentRevisionIds.size()));
        for (const auto parent : cloud.parentRevisionIds) {
            hash.WriteU64(parent);
        }
        hash.WriteU32(static_cast<std::uint32_t>(cloud.vectorClock.size()));
        for (const auto& entry : cloud.vectorClock) {
            hash.WriteU64(entry.deviceId);
            hash.WriteU64(entry.counter);
        }
        hash.WriteU64(foundation::ArchiveChecksum(payload));
        const auto value = hash.Value();
        return value == 0 ? 1u : value;
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
