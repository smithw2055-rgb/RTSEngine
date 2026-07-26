#pragma once

#include <RTSEngine/Roguelite/RunSaveDiagnostics.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace rts::roguelite {

struct RunSaveSlotWriteResult final {
    RunSaveSlotError error{RunSaveSlotError::InvalidEnvelope};
    RunSaveEnvelopeError envelopeError{RunSaveEnvelopeError::None};
    RunSaveMigrationStatus migration{RunSaveMigrationStatus::Corrupt};
    std::uint64_t sequence{};
    RunSaveDiagnostic diagnostic;
};

struct RunSaveSlotLoadResult final {
    RunSaveSlotError error{RunSaveSlotError::NotFound};
    RunSaveSlotSource source{RunSaveSlotSource::None};
    bool repairedPrimary{};
    RunSaveEnvelopeError primaryError{RunSaveEnvelopeError::None};
    RunSaveEnvelopeError recoveryError{RunSaveEnvelopeError::None};
    RunSaveEnvelopeDecodeResult decoded;
    RunSaveDiagnostic diagnostic;
};

class RunSaveSlotStore final {
public:
    static constexpr std::uint64_t kMaximumEnvelopeBytes =
        static_cast<std::uint64_t>(
            RunSaveEnvelopeCodec::kMaximumPayloadBytes) + 1024u;

    static RunSaveSlotWriteResult write(
        const std::filesystem::path& directory,
        const std::string& slotName,
        RunSaveManifestIdentity identity,
        std::uint64_t sequence,
        const std::vector<std::uint8_t>& runSaveBytes,
        IRunSaveDurability& durability = DefaultRunSaveDurability()) {
        RunSaveSlotWriteResult result;
        result.sequence = sequence;
        if (!validSlotName(slotName)) {
            failWrite(
                result,
                RunSaveSlotError::InvalidSlotName,
                RunSaveDiagnosticCode::InvalidSlotName,
                RunSaveDiagnosticStage::ValidateRequest);
            return result;
        }

        auto envelope = RunSaveEnvelopeCodec::build(
            identity, sequence, runSaveBytes);
        result.envelopeError = envelope.error;
        result.migration = envelope.migration;
        if (envelope.error != RunSaveEnvelopeError::None) {
            failWrite(
                result,
                RunSaveSlotError::InvalidEnvelope,
                RunSaveDiagnosticCode::InvalidEnvelope,
                RunSaveDiagnosticStage::BuildEnvelope,
                envelope.error,
                envelope.migration);
            return result;
        }

        std::error_code filesystemError;
        std::filesystem::create_directories(directory, filesystemError);
        if (filesystemError) {
            failWrite(
                result,
                RunSaveSlotError::CreateDirectoryFailed,
                RunSaveDiagnosticCode::DirectoryCreateFailed,
                RunSaveDiagnosticStage::CreateDirectory);
            result.diagnostic.nativeCode = filesystemError.value();
            return result;
        }

        const auto paths = makePaths(directory, slotName);
        const auto primary = readAndDecode(paths.primary);
        const auto recovery = readAndDecode(paths.recovery);
        if ((primary.present && primary.valid &&
             !RunSaveManifestCompatible(
                 primary.decoded.envelope.manifest, identity)) ||
            (recovery.present && recovery.valid &&
             !RunSaveManifestCompatible(
                 recovery.decoded.envelope.manifest, identity))) {
            failWrite(
                result,
                RunSaveSlotError::ManifestMismatch,
                RunSaveDiagnosticCode::ManifestMismatch,
                RunSaveDiagnosticStage::InspectExisting,
                RunSaveEnvelopeError::None,
                envelope.migration);
            return result;
        }

        std::uint64_t latestSequence = 0;
        if (primary.valid) {
            latestSequence = primary.decoded.envelope.manifest.sequence;
        }
        if (recovery.valid) {
            latestSequence = std::max(
                latestSequence,
                recovery.decoded.envelope.manifest.sequence);
        }
        if (sequence <= latestSequence) {
            failWrite(
                result,
                RunSaveSlotError::StaleSequence,
                RunSaveDiagnosticCode::StaleSequence,
                RunSaveDiagnosticStage::InspectExisting,
                RunSaveEnvelopeError::None,
                envelope.migration);
            return result;
        }

        removeNoThrow(paths.temporary);
        const auto temporaryWrite = writeFile(paths.temporary, envelope.bytes);
        if (!temporaryWrite.success) {
            failWrite(
                result,
                RunSaveSlotError::TemporaryWriteFailed,
                RunSaveDiagnosticCode::TemporaryWriteFailed,
                RunSaveDiagnosticStage::WriteTemporary,
                RunSaveEnvelopeError::None,
                envelope.migration);
            result.diagnostic.nativeCode = temporaryWrite.nativeCode;
            return result;
        }

        const auto temporarySync = durability.syncFile(paths.temporary);
        if (!temporarySync) {
            removeNoThrow(paths.temporary);
            failWrite(
                result,
                RunSaveSlotError::TemporarySyncFailed,
                RunSaveDiagnosticCode::TemporarySyncFailed,
                RunSaveDiagnosticStage::SyncTemporary,
                RunSaveEnvelopeError::None,
                envelope.migration,
                temporarySync);
            return result;
        }

        const auto temporary = readAndDecode(paths.temporary);
        if (!temporary.valid || temporary.bytes != envelope.bytes ||
            !RunSaveManifestCompatible(
                temporary.decoded.envelope.manifest, identity)) {
            removeNoThrow(paths.temporary);
            failWrite(
                result,
                RunSaveSlotError::TemporaryValidationFailed,
                RunSaveDiagnosticCode::TemporaryValidationFailed,
                RunSaveDiagnosticStage::ValidateTemporary,
                temporary.decoded.error,
                envelope.migration);
            if (temporary.ioError != 0) {
                result.diagnostic.nativeCode = temporary.ioError;
            }
            return result;
        }

        if (primary.present) {
            RunSaveDurabilityResult rotation;
            if (primary.valid) {
                rotation = durability.replaceFile(
                    paths.primary, paths.recovery);
            } else {
                rotation = durability.removeFile(paths.primary);
            }
            if (!rotation) {
                removeNoThrow(paths.temporary);
                failWrite(
                    result,
                    RunSaveSlotError::RotationFailed,
                    RunSaveDiagnosticCode::RecoveryRotationFailed,
                    RunSaveDiagnosticStage::RotateRecovery,
                    RunSaveEnvelopeError::None,
                    envelope.migration,
                    rotation);
                return result;
            }
        }

        const auto commit = durability.replaceFile(
            paths.temporary, paths.primary);
        if (!commit) {
            removeNoThrow(paths.temporary);
            failWrite(
                result,
                RunSaveSlotError::CommitFailed,
                RunSaveDiagnosticCode::PrimaryCommitFailed,
                RunSaveDiagnosticStage::CommitPrimary,
                RunSaveEnvelopeError::None,
                envelope.migration,
                commit);
            return result;
        }

        const auto committed = readAndDecode(paths.primary);
        if (!committed.valid || committed.bytes != envelope.bytes) {
            failWrite(
                result,
                RunSaveSlotError::CommitFailed,
                RunSaveDiagnosticCode::PrimaryCommitFailed,
                RunSaveDiagnosticStage::ValidateCommit,
                committed.decoded.error,
                envelope.migration);
            if (committed.ioError != 0) {
                result.diagnostic.nativeCode = committed.ioError;
            }
            return result;
        }

        result.error = RunSaveSlotError::None;
        result.envelopeError = RunSaveEnvelopeError::None;
        result.diagnostic = MakeRunSaveDiagnostic(
            RunSaveDiagnosticCode::None,
            RunSaveDiagnosticSeverity::Info,
            RunSaveDiagnosticStage::ValidateCommit,
            RunSaveSlotError::None,
            RunSaveEnvelopeError::None,
            envelope.migration,
            RunSaveSlotSource::Primary);
        return result;
    }

    static RunSaveSlotLoadResult load(
        const std::filesystem::path& directory,
        const std::string& slotName,
        RunSaveManifestIdentity expected,
        bool requireExactBuild = false,
        bool repairPrimary = false,
        IRunSaveDurability& durability = DefaultRunSaveDurability()) {
        RunSaveSlotLoadResult result;
        if (!validSlotName(slotName)) {
            result.error = RunSaveSlotError::InvalidSlotName;
            result.diagnostic = MakeRunSaveDiagnostic(
                RunSaveDiagnosticCode::InvalidSlotName,
                RunSaveDiagnosticSeverity::Error,
                RunSaveDiagnosticStage::ValidateRequest,
                result.error);
            return result;
        }

        const auto paths = makePaths(directory, slotName);
        const auto primary = readAndDecode(paths.primary);
        result.primaryError = primary.decoded.error;
        if (primary.valid && RunSaveManifestCompatible(
                primary.decoded.envelope.manifest,
                expected,
                requireExactBuild)) {
            result.error = RunSaveSlotError::None;
            result.source = RunSaveSlotSource::Primary;
            result.decoded = primary.decoded;
            result.diagnostic = MakeRunSaveDiagnostic(
                RunSaveDiagnosticCode::LoadedPrimary,
                RunSaveDiagnosticSeverity::Info,
                RunSaveDiagnosticStage::LoadPrimary,
                RunSaveSlotError::None,
                RunSaveEnvelopeError::None,
                primary.decoded.migration,
                result.source);
            return result;
        }

        const auto recovery = readAndDecode(paths.recovery);
        result.recoveryError = recovery.decoded.error;
        if (recovery.valid && RunSaveManifestCompatible(
                recovery.decoded.envelope.manifest,
                expected,
                requireExactBuild)) {
            result.error = RunSaveSlotError::None;
            result.source = RunSaveSlotSource::Recovery;
            result.decoded = recovery.decoded;
            result.diagnostic = MakeRunSaveDiagnostic(
                RunSaveDiagnosticCode::LoadedRecovery,
                RunSaveDiagnosticSeverity::Warning,
                RunSaveDiagnosticStage::LoadRecovery,
                RunSaveSlotError::None,
                RunSaveEnvelopeError::None,
                recovery.decoded.migration,
                result.source);
            result.diagnostic.fallbackUsed = true;
            if (repairPrimary) {
                result.diagnostic.repairAttempted = true;
                const auto repair = promoteRecovery(
                    paths, recovery.bytes, durability);
                result.repairedPrimary = repair.success;
                result.diagnostic.repairSucceeded = repair.success;
                if (!repair.success) {
                    result.diagnostic.code =
                        RunSaveDiagnosticCode::LoadedRecoveryRepairFailed;
                    result.diagnostic.stage = repair.stage;
                    result.diagnostic.durability = repair.durability;
                    result.diagnostic.nativeCode = repair.nativeCode;
                    if (repair.envelopeError != RunSaveEnvelopeError::None) {
                        result.diagnostic.envelopeError = repair.envelopeError;
                    }
                }
            }
            return result;
        }

        if (!primary.present && !recovery.present) {
            result.error = RunSaveSlotError::NotFound;
            result.diagnostic = MakeRunSaveDiagnostic(
                RunSaveDiagnosticCode::SaveNotFound,
                RunSaveDiagnosticSeverity::Info,
                RunSaveDiagnosticStage::LoadPrimary,
                result.error);
        } else if ((primary.valid || recovery.valid) &&
                   ((!primary.valid || !RunSaveManifestCompatible(
                        primary.decoded.envelope.manifest,
                        expected,
                        requireExactBuild)) &&
                    (!recovery.valid || !RunSaveManifestCompatible(
                        recovery.decoded.envelope.manifest,
                        expected,
                        requireExactBuild)))) {
            result.error = RunSaveSlotError::ManifestMismatch;
            result.diagnostic = MakeRunSaveDiagnostic(
                RunSaveDiagnosticCode::ManifestMismatch,
                RunSaveDiagnosticSeverity::Error,
                RunSaveDiagnosticStage::LoadRecovery,
                result.error);
        } else if (primary.present) {
            result.error = RunSaveSlotError::PrimaryInvalid;
            result.diagnostic = MakeRunSaveDiagnostic(
                RunSaveDiagnosticCode::PrimaryInvalid,
                RunSaveDiagnosticSeverity::Error,
                RunSaveDiagnosticStage::LoadPrimary,
                result.error,
                primary.decoded.error,
                primary.decoded.migration,
                RunSaveSlotSource::Primary);
            result.diagnostic.nativeCode = primary.ioError;
        } else {
            result.error = RunSaveSlotError::RecoveryInvalid;
            result.diagnostic = MakeRunSaveDiagnostic(
                RunSaveDiagnosticCode::RecoveryInvalid,
                RunSaveDiagnosticSeverity::Error,
                RunSaveDiagnosticStage::LoadRecovery,
                result.error,
                recovery.decoded.error,
                recovery.decoded.migration,
                RunSaveSlotSource::Recovery);
            result.diagnostic.nativeCode = recovery.ioError;
        }
        return result;
    }

private:
    struct SlotPaths final {
        std::filesystem::path primary;
        std::filesystem::path recovery;
        std::filesystem::path temporary;
    };

    struct FileInspection final {
        bool present{};
        bool valid{};
        std::int64_t ioError{};
        std::vector<std::uint8_t> bytes;
        RunSaveEnvelopeDecodeResult decoded;
    };

    struct FileWriteResult final {
        bool success{};
        std::int64_t nativeCode{};
    };

    struct RepairResult final {
        bool success{};
        RunSaveDiagnosticStage stage{RunSaveDiagnosticStage::RepairPrimary};
        RunSaveDurabilityResult durability;
        RunSaveEnvelopeError envelopeError{RunSaveEnvelopeError::None};
        std::int64_t nativeCode{};
    };

    static void failWrite(
        RunSaveSlotWriteResult& result,
        RunSaveSlotError error,
        RunSaveDiagnosticCode code,
        RunSaveDiagnosticStage stage,
        RunSaveEnvelopeError envelopeError = RunSaveEnvelopeError::None,
        RunSaveMigrationStatus migration = RunSaveMigrationStatus::Corrupt,
        RunSaveDurabilityResult durability = {}) noexcept {
        result.error = error;
        result.envelopeError = envelopeError;
        result.diagnostic = MakeRunSaveDiagnostic(
            code,
            RunSaveDiagnosticSeverity::Error,
            stage,
            error,
            envelopeError,
            migration,
            RunSaveSlotSource::None,
            durability);
    }

    static bool validSlotName(const std::string& value) noexcept {
        if (value.empty() || value.size() > 64) return false;
        for (const unsigned char character : value) {
            if (!std::isalnum(character) && character != '-' &&
                character != '_') {
                return false;
            }
        }
        return true;
    }

    static SlotPaths makePaths(
        const std::filesystem::path& directory,
        const std::string& slotName) {
        return {
            directory / (slotName + ".sav"),
            directory / (slotName + ".recovery.sav"),
            directory / (slotName + ".tmp")};
    }

    static FileWriteResult writeFile(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes) {
        errno = 0;
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) return {false, errno};
        if (!bytes.empty()) {
            stream.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        stream.flush();
        const bool success = static_cast<bool>(stream);
        const auto native = success ? 0 : errno;
        stream.close();
        return {success && static_cast<bool>(stream), native};
    }

    static FileInspection readAndDecode(
        const std::filesystem::path& path) {
        FileInspection result;
        std::error_code error;
        result.present = std::filesystem::exists(path, error) && !error;
        if (error) result.ioError = error.value();
        if (!result.present) return result;

        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0 || size > kMaximumEnvelopeBytes) {
            result.ioError = error.value();
            result.decoded.error = size > kMaximumEnvelopeBytes
                ? RunSaveEnvelopeError::TooLarge
                : RunSaveEnvelopeError::Truncated;
            return result;
        }

        result.bytes.resize(static_cast<std::size_t>(size));
        errno = 0;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            result.ioError = errno;
            return result;
        }
        stream.read(
            reinterpret_cast<char*>(result.bytes.data()),
            static_cast<std::streamsize>(result.bytes.size()));
        if (!stream || stream.gcount() !=
                static_cast<std::streamsize>(result.bytes.size())) {
            result.ioError = errno;
            result.bytes.clear();
            return result;
        }
        result.decoded = RunSaveEnvelopeCodec::decode(result.bytes);
        result.valid = result.decoded.error == RunSaveEnvelopeError::None;
        return result;
    }

    static void removeNoThrow(const std::filesystem::path& path) noexcept {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    static RepairResult promoteRecovery(
        const SlotPaths& paths,
        const std::vector<std::uint8_t>& bytes,
        IRunSaveDurability& durability) {
        RepairResult result;
        removeNoThrow(paths.temporary);
        const auto write = writeFile(paths.temporary, bytes);
        if (!write.success) {
            result.stage = RunSaveDiagnosticStage::WriteTemporary;
            result.nativeCode = write.nativeCode;
            return result;
        }
        const auto sync = durability.syncFile(paths.temporary);
        if (!sync) {
            removeNoThrow(paths.temporary);
            result.stage = RunSaveDiagnosticStage::SyncTemporary;
            result.durability = sync;
            result.nativeCode = sync.nativeCode;
            return result;
        }
        const auto temporary = readAndDecode(paths.temporary);
        if (!temporary.valid || temporary.bytes != bytes) {
            removeNoThrow(paths.temporary);
            result.stage = RunSaveDiagnosticStage::ValidateTemporary;
            result.envelopeError = temporary.decoded.error;
            result.nativeCode = temporary.ioError;
            return result;
        }
        const auto replace = durability.replaceFile(
            paths.temporary, paths.primary);
        if (!replace) {
            removeNoThrow(paths.temporary);
            result.stage = RunSaveDiagnosticStage::RepairPrimary;
            result.durability = replace;
            result.nativeCode = replace.nativeCode;
            return result;
        }
        result.success = true;
        result.nativeCode = 0;
        return result;
    }
};

} // namespace rts::roguelite
