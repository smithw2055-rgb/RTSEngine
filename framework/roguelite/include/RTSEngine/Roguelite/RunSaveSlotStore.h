#pragma once

#include <RTSEngine/Roguelite/RunSaveEnvelope.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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
    TemporaryValidationFailed,
    RotationFailed,
    CommitFailed,
    NotFound,
    PrimaryInvalid,
    RecoveryInvalid
};

struct RunSaveSlotWriteResult final {
    RunSaveSlotError error{RunSaveSlotError::InvalidEnvelope};
    RunSaveEnvelopeError envelopeError{RunSaveEnvelopeError::None};
    RunSaveMigrationStatus migration{RunSaveMigrationStatus::Corrupt};
    std::uint64_t sequence{};
};

struct RunSaveSlotLoadResult final {
    RunSaveSlotError error{RunSaveSlotError::NotFound};
    RunSaveSlotSource source{RunSaveSlotSource::None};
    bool repairedPrimary{};
    RunSaveEnvelopeError primaryError{RunSaveEnvelopeError::None};
    RunSaveEnvelopeError recoveryError{RunSaveEnvelopeError::None};
    RunSaveEnvelopeDecodeResult decoded;
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
        const std::vector<std::uint8_t>& runSaveBytes) {
        RunSaveSlotWriteResult result;
        result.sequence = sequence;
        if (!validSlotName(slotName)) {
            result.error = RunSaveSlotError::InvalidSlotName;
            return result;
        }

        auto envelope = RunSaveEnvelopeCodec::build(
            identity, sequence, runSaveBytes);
        result.envelopeError = envelope.error;
        result.migration = envelope.migration;
        if (envelope.error != RunSaveEnvelopeError::None) {
            result.error = RunSaveSlotError::InvalidEnvelope;
            return result;
        }

        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            result.error = RunSaveSlotError::CreateDirectoryFailed;
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
            result.error = RunSaveSlotError::ManifestMismatch;
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
            result.error = RunSaveSlotError::StaleSequence;
            return result;
        }

        removeNoThrow(paths.temporary);
        if (!writeFile(paths.temporary, envelope.bytes)) {
            result.error = RunSaveSlotError::TemporaryWriteFailed;
            return result;
        }
        const auto temporary = readAndDecode(paths.temporary);
        if (!temporary.valid ||
            temporary.bytes != envelope.bytes ||
            !RunSaveManifestCompatible(
                temporary.decoded.envelope.manifest, identity)) {
            removeNoThrow(paths.temporary);
            result.error = RunSaveSlotError::TemporaryValidationFailed;
            result.envelopeError = temporary.decoded.error;
            return result;
        }

        if (primary.present) {
            if (primary.valid) {
                removeNoThrow(paths.recovery);
                std::filesystem::rename(
                    paths.primary, paths.recovery, error);
                if (error) {
                    removeNoThrow(paths.temporary);
                    result.error = RunSaveSlotError::RotationFailed;
                    return result;
                }
            } else {
                std::filesystem::remove(paths.primary, error);
                if (error) {
                    removeNoThrow(paths.temporary);
                    result.error = RunSaveSlotError::RotationFailed;
                    return result;
                }
            }
        }

        error.clear();
        std::filesystem::rename(paths.temporary, paths.primary, error);
        if (error) {
            removeNoThrow(paths.temporary);
            result.error = RunSaveSlotError::CommitFailed;
            return result;
        }

        const auto committed = readAndDecode(paths.primary);
        if (!committed.valid || committed.bytes != envelope.bytes) {
            result.error = RunSaveSlotError::CommitFailed;
            result.envelopeError = committed.decoded.error;
            return result;
        }

        result.error = RunSaveSlotError::None;
        result.envelopeError = RunSaveEnvelopeError::None;
        return result;
    }

    static RunSaveSlotLoadResult load(
        const std::filesystem::path& directory,
        const std::string& slotName,
        RunSaveManifestIdentity expected,
        bool requireExactBuild = false,
        bool repairPrimary = false) {
        RunSaveSlotLoadResult result;
        if (!validSlotName(slotName)) {
            result.error = RunSaveSlotError::InvalidSlotName;
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
            if (repairPrimary) {
                result.repairedPrimary = promoteRecovery(paths, recovery.bytes);
            }
            return result;
        }

        if (!primary.present && !recovery.present) {
            result.error = RunSaveSlotError::NotFound;
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
        } else if (primary.present) {
            result.error = RunSaveSlotError::PrimaryInvalid;
        } else {
            result.error = RunSaveSlotError::RecoveryInvalid;
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
        std::vector<std::uint8_t> bytes;
        RunSaveEnvelopeDecodeResult decoded;
    };

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

    static bool writeFile(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        if (!bytes.empty()) {
            stream.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        stream.flush();
        const bool success = static_cast<bool>(stream);
        stream.close();
        return success;
    }

    static FileInspection readAndDecode(
        const std::filesystem::path& path) {
        FileInspection result;
        std::error_code error;
        result.present = std::filesystem::exists(path, error) && !error;
        if (!result.present) return result;

        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0 || size > kMaximumEnvelopeBytes) {
            result.decoded.error = RunSaveEnvelopeError::TooLarge;
            return result;
        }

        result.bytes.resize(static_cast<std::size_t>(size));
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return result;
        stream.read(
            reinterpret_cast<char*>(result.bytes.data()),
            static_cast<std::streamsize>(result.bytes.size()));
        if (!stream || stream.gcount() !=
                static_cast<std::streamsize>(result.bytes.size())) {
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

    static bool promoteRecovery(
        const SlotPaths& paths,
        const std::vector<std::uint8_t>& bytes) {
        removeNoThrow(paths.temporary);
        if (!writeFile(paths.temporary, bytes)) return false;
        const auto temporary = readAndDecode(paths.temporary);
        if (!temporary.valid || temporary.bytes != bytes) {
            removeNoThrow(paths.temporary);
            return false;
        }
        removeNoThrow(paths.primary);
        std::error_code error;
        std::filesystem::rename(paths.temporary, paths.primary, error);
        if (error) {
            removeNoThrow(paths.temporary);
            return false;
        }
        return true;
    }
};

} // namespace rts::roguelite
