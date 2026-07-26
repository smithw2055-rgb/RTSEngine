#pragma once

#include <RTSEngine/Roguelite/RunSaveCloudConflict.h>
#include <RTSEngine/Roguelite/RunSaveDurability.h>
#include <rts/foundation/ArchiveChecksum.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace rts::roguelite {

enum class RunSaveCloudBranchError : std::uint8_t {
    None,
    InvalidBaseSlot,
    InvalidEnvelope,
    CreateDirectoryFailed,
    WriteFailed,
    SyncFailed,
    CommitFailed,
    ValidationFailed
};

struct RunSaveCloudBranchResult final {
    RunSaveCloudBranchError error{RunSaveCloudBranchError::InvalidEnvelope};
    RunSaveEnvelopeError envelopeError{RunSaveEnvelopeError::None};
    RunSaveDurabilityResult durability;
    std::int64_t nativeCode{};
    bool alreadyPresent{};
    std::string branchName;
    std::filesystem::path path;
};

inline bool ValidateRunSaveBaseSlotName(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64u) return false;
    for (const unsigned char character : value) {
        if (!std::isalnum(character) && character != '-' &&
            character != '_') {
            return false;
        }
    }
    return true;
}

inline std::string NormalizeRunSaveCloudBranchLabel(
    std::string_view value,
    RunSaveCloudSide side) {
    std::string result;
    result.reserve(std::min<std::size_t>(value.size(), 24u));
    bool separator = false;
    for (const unsigned char character : value) {
        if (std::isalnum(character)) {
            if (separator && !result.empty() && result.size() < 24u) {
                result.push_back('-');
            }
            separator = false;
            if (result.size() < 24u) {
                result.push_back(static_cast<char>(std::tolower(character)));
            }
        } else {
            separator = true;
        }
        if (result.size() >= 24u) break;
    }
    while (!result.empty() && result.back() == '-') result.pop_back();
    if (result.empty()) {
        result = side == RunSaveCloudSide::Cloud ? "cloud" : "local";
    }
    return result;
}

inline std::string RunSaveCloudHex(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(16u, '0');
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto shift = static_cast<unsigned>((15u - index) * 4u);
        result[index] = digits[(value >> shift) & 0x0fu];
    }
    return result;
}

inline std::string MakeRunSaveCloudBranchName(
    std::string_view baseSlot,
    std::string_view requestedLabel,
    RunSaveCloudSide side,
    const RunSaveEnvelopeDecodeResult& decoded,
    const std::vector<std::uint8_t>& envelopeBytes) {
    std::string base(baseSlot.substr(0, std::min<std::size_t>(40u, baseSlot.size())));
    const auto label = NormalizeRunSaveCloudBranchLabel(requestedLabel, side);
    const auto revision =
        decoded.envelope.manifest.identity.cloud.tracked()
        ? decoded.envelope.manifest.identity.cloud.revisionId
        : foundation::ArchiveChecksum(envelopeBytes);
    return base + "--branch--" + label + "--" +
           RunSaveCloudHex(revision) + ".branch.sav";
}

inline RunSaveCloudBranchResult PreserveRunSaveCloudBranch(
    const std::filesystem::path& directory,
    std::string_view baseSlot,
    std::string_view requestedLabel,
    RunSaveCloudSide side,
    const std::vector<std::uint8_t>& envelopeBytes,
    IRunSaveDurability& durability = DefaultRunSaveDurability()) {
    RunSaveCloudBranchResult result;
    if (!ValidateRunSaveBaseSlotName(baseSlot)) {
        result.error = RunSaveCloudBranchError::InvalidBaseSlot;
        return result;
    }

    const auto decoded = RunSaveEnvelopeCodec::decode(envelopeBytes);
    result.envelopeError = decoded.error;
    if (decoded.error != RunSaveEnvelopeError::None) {
        result.error = RunSaveCloudBranchError::InvalidEnvelope;
        return result;
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(directory, filesystemError);
    if (filesystemError) {
        result.error = RunSaveCloudBranchError::CreateDirectoryFailed;
        result.nativeCode = filesystemError.value();
        return result;
    }

    result.branchName = MakeRunSaveCloudBranchName(
        baseSlot, requestedLabel, side, decoded, envelopeBytes);
    result.path = directory / result.branchName;
    const auto temporary = directory / (result.branchName + ".tmp");

    if (std::filesystem::exists(result.path, filesystemError) &&
        !filesystemError) {
        std::ifstream existing(result.path, std::ios::binary | std::ios::ate);
        if (existing) {
            const auto size = existing.tellg();
            if (size >= 0 && static_cast<std::uint64_t>(size) ==
                    envelopeBytes.size()) {
                existing.seekg(0);
                std::vector<std::uint8_t> bytes(envelopeBytes.size());
                existing.read(
                    reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
                if (existing && bytes == envelopeBytes) {
                    result.error = RunSaveCloudBranchError::None;
                    result.alreadyPresent = true;
                    return result;
                }
            }
        }
    }

    std::filesystem::remove(temporary, filesystemError);
    errno = 0;
    std::ofstream stream(
        temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        result.error = RunSaveCloudBranchError::WriteFailed;
        result.nativeCode = errno;
        return result;
    }
    if (!envelopeBytes.empty()) {
        stream.write(
            reinterpret_cast<const char*>(envelopeBytes.data()),
            static_cast<std::streamsize>(envelopeBytes.size()));
    }
    stream.flush();
    const bool writeSucceeded = static_cast<bool>(stream);
    result.nativeCode = writeSucceeded ? 0 : errno;
    stream.close();
    if (!writeSucceeded || !stream) {
        std::filesystem::remove(temporary, filesystemError);
        result.error = RunSaveCloudBranchError::WriteFailed;
        return result;
    }

    result.durability = durability.syncFile(temporary);
    if (!result.durability) {
        std::filesystem::remove(temporary, filesystemError);
        result.error = RunSaveCloudBranchError::SyncFailed;
        result.nativeCode = result.durability.nativeCode;
        return result;
    }

    result.durability = durability.replaceFile(temporary, result.path);
    if (!result.durability) {
        std::filesystem::remove(temporary, filesystemError);
        result.error = RunSaveCloudBranchError::CommitFailed;
        result.nativeCode = result.durability.nativeCode;
        return result;
    }

    std::ifstream committed(result.path, std::ios::binary | std::ios::ate);
    if (!committed || committed.tellg() < 0 ||
        static_cast<std::uint64_t>(committed.tellg()) !=
            envelopeBytes.size()) {
        result.error = RunSaveCloudBranchError::ValidationFailed;
        return result;
    }
    committed.seekg(0);
    std::vector<std::uint8_t> readBack(envelopeBytes.size());
    committed.read(
        reinterpret_cast<char*>(readBack.data()),
        static_cast<std::streamsize>(readBack.size()));
    if (!committed || readBack != envelopeBytes ||
        RunSaveEnvelopeCodec::decode(readBack).error !=
            RunSaveEnvelopeError::None) {
        result.error = RunSaveCloudBranchError::ValidationFailed;
        return result;
    }

    result.error = RunSaveCloudBranchError::None;
    result.envelopeError = RunSaveEnvelopeError::None;
    return result;
}

} // namespace rts::roguelite
