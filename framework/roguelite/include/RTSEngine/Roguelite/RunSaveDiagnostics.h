#pragma once

#include <RTSEngine/Roguelite/RunSaveDurability.h>
#include <RTSEngine/Roguelite/RunSaveEnvelope.h>

#include <cstdint>
#include <string_view>

namespace rts::roguelite {

enum class RunSaveSlotSource : std::uint8_t {
    None,
    Primary,
    Recovery
};

enum class RunSaveSlotError : std::uint8_t {
    None,
    InvalidSlotName,
    InvalidEnvelope,
    ManifestMismatch,
    StaleSequence,
    CreateDirectoryFailed,
    TemporaryWriteFailed,
    TemporarySyncFailed,
    TemporaryValidationFailed,
    RotationFailed,
    CommitFailed,
    NotFound,
    PrimaryInvalid,
    RecoveryInvalid
};

enum class RunSaveDiagnosticSeverity : std::uint8_t {
    Info,
    Warning,
    Error
};

enum class RunSaveDiagnosticStage : std::uint8_t {
    None,
    ValidateRequest,
    BuildEnvelope,
    InspectExisting,
    CreateDirectory,
    WriteTemporary,
    SyncTemporary,
    ValidateTemporary,
    RotateRecovery,
    CommitPrimary,
    ValidateCommit,
    LoadPrimary,
    LoadRecovery,
    RepairPrimary
};

enum class RunSaveDiagnosticCode : std::uint8_t {
    None,
    InvalidSlotName,
    InvalidEnvelope,
    ManifestMismatch,
    StaleSequence,
    DirectoryCreateFailed,
    TemporaryWriteFailed,
    TemporarySyncFailed,
    TemporaryValidationFailed,
    RecoveryRotationFailed,
    PrimaryCommitFailed,
    SaveNotFound,
    PrimaryInvalid,
    RecoveryInvalid,
    LoadedPrimary,
    LoadedRecovery,
    LoadedRecoveryRepairFailed
};

struct RunSaveDiagnostic final {
    RunSaveDiagnosticCode code{RunSaveDiagnosticCode::None};
    RunSaveDiagnosticSeverity severity{RunSaveDiagnosticSeverity::Info};
    RunSaveDiagnosticStage stage{RunSaveDiagnosticStage::None};
    RunSaveSlotSource source{RunSaveSlotSource::None};
    RunSaveSlotError slotError{RunSaveSlotError::None};
    RunSaveEnvelopeError envelopeError{RunSaveEnvelopeError::None};
    RunSaveMigrationStatus migration{RunSaveMigrationStatus::Corrupt};
    RunSaveDurabilityResult durability;
    std::int64_t nativeCode{};
    bool fallbackUsed{};
    bool repairAttempted{};
    bool repairSucceeded{};
};

inline constexpr std::string_view RunSaveDiagnosticCodeName(
    RunSaveDiagnosticCode value) noexcept {
    switch (value) {
    case RunSaveDiagnosticCode::None: return "save.none";
    case RunSaveDiagnosticCode::InvalidSlotName: return "save.invalid_slot_name";
    case RunSaveDiagnosticCode::InvalidEnvelope: return "save.invalid_envelope";
    case RunSaveDiagnosticCode::ManifestMismatch: return "save.manifest_mismatch";
    case RunSaveDiagnosticCode::StaleSequence: return "save.stale_sequence";
    case RunSaveDiagnosticCode::DirectoryCreateFailed: return "save.directory_create_failed";
    case RunSaveDiagnosticCode::TemporaryWriteFailed: return "save.temporary_write_failed";
    case RunSaveDiagnosticCode::TemporarySyncFailed: return "save.temporary_sync_failed";
    case RunSaveDiagnosticCode::TemporaryValidationFailed: return "save.temporary_validation_failed";
    case RunSaveDiagnosticCode::RecoveryRotationFailed: return "save.recovery_rotation_failed";
    case RunSaveDiagnosticCode::PrimaryCommitFailed: return "save.primary_commit_failed";
    case RunSaveDiagnosticCode::SaveNotFound: return "save.not_found";
    case RunSaveDiagnosticCode::PrimaryInvalid: return "save.primary_invalid";
    case RunSaveDiagnosticCode::RecoveryInvalid: return "save.recovery_invalid";
    case RunSaveDiagnosticCode::LoadedPrimary: return "save.loaded_primary";
    case RunSaveDiagnosticCode::LoadedRecovery: return "save.loaded_recovery";
    case RunSaveDiagnosticCode::LoadedRecoveryRepairFailed:
        return "save.loaded_recovery_repair_failed";
    }
    return "save.unknown";
}

inline constexpr std::string_view RunSaveEnvelopeErrorName(
    RunSaveEnvelopeError value) noexcept {
    switch (value) {
    case RunSaveEnvelopeError::None: return "none";
    case RunSaveEnvelopeError::InvalidManifest: return "invalid_manifest";
    case RunSaveEnvelopeError::InvalidPayload: return "invalid_payload";
    case RunSaveEnvelopeError::UnsupportedPayload: return "unsupported_payload";
    case RunSaveEnvelopeError::TooLarge: return "too_large";
    case RunSaveEnvelopeError::BadMagic: return "bad_magic";
    case RunSaveEnvelopeError::UnsupportedEnvelopeVersion:
        return "unsupported_envelope_version";
    case RunSaveEnvelopeError::WrongKind: return "wrong_kind";
    case RunSaveEnvelopeError::Truncated: return "truncated";
    case RunSaveEnvelopeError::InvalidFlags: return "invalid_flags";
    case RunSaveEnvelopeError::ChecksumMismatch: return "checksum_mismatch";
    case RunSaveEnvelopeError::ManifestPayloadMismatch:
        return "manifest_payload_mismatch";
    case RunSaveEnvelopeError::TrailingData: return "trailing_data";
    }
    return "unknown";
}

inline constexpr std::string_view RunSaveMigrationStatusName(
    RunSaveMigrationStatus value) noexcept {
    switch (value) {
    case RunSaveMigrationStatus::CurrentAuthoritative:
        return "current_authoritative";
    case RunSaveMigrationStatus::CurrentSummaryOnly:
        return "current_summary_only";
    case RunSaveMigrationStatus::MigratedLegacySummary:
        return "migrated_legacy_summary";
    case RunSaveMigrationStatus::UnsupportedFuture:
        return "unsupported_future";
    case RunSaveMigrationStatus::Corrupt: return "corrupt";
    }
    return "unknown";
}

inline constexpr std::string_view RunSaveDurabilityErrorName(
    RunSaveDurabilityError value) noexcept {
    switch (value) {
    case RunSaveDurabilityError::None: return "none";
    case RunSaveDurabilityError::OpenFailed: return "open_failed";
    case RunSaveDurabilityError::SyncFailed: return "sync_failed";
    case RunSaveDurabilityError::ReplaceFailed: return "replace_failed";
    case RunSaveDurabilityError::RemoveFailed: return "remove_failed";
    case RunSaveDurabilityError::DirectoryOpenFailed:
        return "directory_open_failed";
    case RunSaveDurabilityError::DirectorySyncFailed:
        return "directory_sync_failed";
    }
    return "unknown";
}

inline RunSaveDiagnostic MakeRunSaveDiagnostic(
    RunSaveDiagnosticCode code,
    RunSaveDiagnosticSeverity severity,
    RunSaveDiagnosticStage stage,
    RunSaveSlotError slotError = RunSaveSlotError::None,
    RunSaveEnvelopeError envelopeError = RunSaveEnvelopeError::None,
    RunSaveMigrationStatus migration = RunSaveMigrationStatus::Corrupt,
    RunSaveSlotSource source = RunSaveSlotSource::None,
    RunSaveDurabilityResult durability = {}) noexcept {
    RunSaveDiagnostic result;
    result.code = code;
    result.severity = severity;
    result.stage = stage;
    result.source = source;
    result.slotError = slotError;
    result.envelopeError = envelopeError;
    result.migration = migration;
    result.durability = durability;
    result.nativeCode = durability.nativeCode;
    return result;
}

} // namespace rts::roguelite
