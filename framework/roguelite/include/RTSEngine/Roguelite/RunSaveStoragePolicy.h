#pragma once

#include <RTSEngine/Roguelite/RunSaveDurability.h>
#include <RTSEngine/Roguelite/RunSaveEnvelope.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace rts::roguelite {

enum class RunSaveStorageKind : std::uint8_t {
    Primary,
    Recovery,
    ConflictBranch,
    Temporary
};

struct RunSaveStorageEntry final {
    std::filesystem::path path;
    std::string filename;
    std::string baseSlot;
    RunSaveStorageKind kind{RunSaveStorageKind::Primary};
    std::uint64_t size{};
    bool valid{};
    RunSaveEnvelopeError envelopeError{RunSaveEnvelopeError::None};
    std::uint64_t sequence{};
    std::uint64_t saveTick{};
    std::uint64_t revisionId{};
};

struct RunSaveStorageInventory final {
    std::vector<RunSaveStorageEntry> entries;
    std::uint64_t totalBytes{};
    std::uint64_t primaryBytes{};
    std::uint64_t recoveryBytes{};
    std::uint64_t conflictBranchBytes{};
    std::uint64_t temporaryBytes{};
    std::uint32_t primaryCount{};
    std::uint32_t recoveryCount{};
    std::uint32_t conflictBranchCount{};
    std::uint32_t temporaryCount{};
    std::uint32_t invalidCount{};
};

struct RunSaveStoragePolicy final {
    std::uint64_t maximumTotalBytes{
        std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t maximumConflictBranchBytes{
        std::numeric_limits<std::uint64_t>::max()};
    std::uint32_t maximumConflictBranchesPerSlot{8u};
    bool removeTemporaryFiles{true};
    bool removeInvalidConflictBranches{true};
};

struct RunSaveStorageRetentionPlan final {
    std::vector<std::filesystem::path> filesToRemove;
    std::uint64_t bytesBefore{};
    std::uint64_t bytesAfter{};
    std::uint64_t conflictBranchBytesBefore{};
    std::uint64_t conflictBranchBytesAfter{};
    bool totalBudgetSatisfied{};
    bool conflictBranchBudgetSatisfied{};
};

struct RunSaveStorageCleanupFailure final {
    std::filesystem::path path;
    RunSaveDurabilityResult durability;
};

struct RunSaveStorageCleanupResult final {
    std::uint32_t plannedFiles{};
    std::uint32_t removedFiles{};
    std::uint64_t removedBytes{};
    std::vector<RunSaveStorageCleanupFailure> failures;
};

inline bool RunSaveStorageEndsWith(
    const std::string& value,
    const char* suffix) {
    const std::string ending(suffix);
    return value.size() >= ending.size() &&
           value.compare(
               value.size() - ending.size(), ending.size(), ending) == 0;
}

inline bool ClassifyRunSaveStorageFile(
    const std::string& filename,
    RunSaveStorageKind& kind,
    std::string& baseSlot) {
    if (RunSaveStorageEndsWith(filename, ".recovery.sav")) {
        kind = RunSaveStorageKind::Recovery;
        baseSlot = filename.substr(
            0, filename.size() - std::string(".recovery.sav").size());
        return true;
    }
    if (RunSaveStorageEndsWith(filename, ".branch.sav")) {
        kind = RunSaveStorageKind::ConflictBranch;
        const auto marker = filename.find("--branch--");
        baseSlot = marker == std::string::npos
            ? filename.substr(
                0, filename.size() - std::string(".branch.sav").size())
            : filename.substr(0, marker);
        return true;
    }
    if (RunSaveStorageEndsWith(filename, ".tmp")) {
        kind = RunSaveStorageKind::Temporary;
        baseSlot = filename.substr(
            0, filename.size() - std::string(".tmp").size());
        return true;
    }
    if (RunSaveStorageEndsWith(filename, ".sav")) {
        kind = RunSaveStorageKind::Primary;
        baseSlot = filename.substr(
            0, filename.size() - std::string(".sav").size());
        return true;
    }
    return false;
}

inline RunSaveStorageInventory InspectRunSaveStorage(
    const std::filesystem::path& directory) {
    RunSaveStorageInventory result;
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) return result;

    constexpr std::uint64_t maximumEnvelopeBytes =
        static_cast<std::uint64_t>(
            RunSaveEnvelopeCodec::kMaximumPayloadBytes) + 4096u;

    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || error) continue;
        RunSaveStorageEntry entry;
        entry.path = iterator->path();
        entry.filename = entry.path.filename().string();
        if (!ClassifyRunSaveStorageFile(
                entry.filename, entry.kind, entry.baseSlot)) {
            continue;
        }
        entry.size = iterator->file_size(error);
        if (error) {
            error.clear();
            continue;
        }

        result.totalBytes += entry.size;
        switch (entry.kind) {
        case RunSaveStorageKind::Primary:
            result.primaryBytes += entry.size;
            ++result.primaryCount;
            break;
        case RunSaveStorageKind::Recovery:
            result.recoveryBytes += entry.size;
            ++result.recoveryCount;
            break;
        case RunSaveStorageKind::ConflictBranch:
            result.conflictBranchBytes += entry.size;
            ++result.conflictBranchCount;
            break;
        case RunSaveStorageKind::Temporary:
            result.temporaryBytes += entry.size;
            ++result.temporaryCount;
            result.entries.push_back(std::move(entry));
            continue;
        }

        if (entry.size == 0 || entry.size > maximumEnvelopeBytes) {
            entry.envelopeError = RunSaveEnvelopeError::TooLarge;
            ++result.invalidCount;
            result.entries.push_back(std::move(entry));
            continue;
        }
        std::ifstream stream(entry.path, std::ios::binary);
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(entry.size));
        if (!stream ||
            (!bytes.empty() &&
             !stream.read(
                 reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size())))) {
            entry.envelopeError = RunSaveEnvelopeError::Truncated;
            ++result.invalidCount;
            result.entries.push_back(std::move(entry));
            continue;
        }
        const auto decoded = RunSaveEnvelopeCodec::decode(bytes);
        entry.envelopeError = decoded.error;
        entry.valid = decoded.error == RunSaveEnvelopeError::None;
        if (entry.valid) {
            entry.sequence = decoded.envelope.manifest.sequence;
            entry.saveTick = decoded.envelope.manifest.saveTick;
            entry.revisionId =
                decoded.envelope.manifest.identity.cloud.revisionId;
        } else {
            ++result.invalidCount;
        }
        result.entries.push_back(std::move(entry));
    }

    std::sort(
        result.entries.begin(), result.entries.end(),
        [](const RunSaveStorageEntry& a, const RunSaveStorageEntry& b) {
            return a.filename < b.filename;
        });
    return result;
}

inline bool RunSaveStorageNewer(
    const RunSaveStorageEntry& a,
    const RunSaveStorageEntry& b) {
    if (a.sequence != b.sequence) return a.sequence > b.sequence;
    if (a.saveTick != b.saveTick) return a.saveTick > b.saveTick;
    if (a.revisionId != b.revisionId) return a.revisionId > b.revisionId;
    return a.filename > b.filename;
}

inline RunSaveStorageRetentionPlan BuildRunSaveStorageRetentionPlan(
    const RunSaveStorageInventory& inventory,
    const RunSaveStoragePolicy& policy) {
    RunSaveStorageRetentionPlan result;
    result.bytesBefore = inventory.totalBytes;
    result.bytesAfter = inventory.totalBytes;
    result.conflictBranchBytesBefore = inventory.conflictBranchBytes;
    result.conflictBranchBytesAfter = inventory.conflictBranchBytes;

    std::vector<bool> marked(inventory.entries.size(), false);
    auto mark = [&](std::size_t index) {
        if (marked[index]) return;
        marked[index] = true;
        result.bytesAfter -= inventory.entries[index].size;
        if (inventory.entries[index].kind ==
            RunSaveStorageKind::ConflictBranch) {
            result.conflictBranchBytesAfter -=
                inventory.entries[index].size;
        }
    };

    for (std::size_t index = 0; index < inventory.entries.size(); ++index) {
        const auto& entry = inventory.entries[index];
        if (policy.removeTemporaryFiles &&
            entry.kind == RunSaveStorageKind::Temporary) {
            mark(index);
        }
        if (policy.removeInvalidConflictBranches &&
            entry.kind == RunSaveStorageKind::ConflictBranch &&
            !entry.valid) {
            mark(index);
        }
    }

    std::vector<std::string> slots;
    for (const auto& entry : inventory.entries) {
        if (entry.kind == RunSaveStorageKind::ConflictBranch &&
            entry.valid &&
            std::find(slots.begin(), slots.end(), entry.baseSlot) ==
                slots.end()) {
            slots.push_back(entry.baseSlot);
        }
    }
    std::sort(slots.begin(), slots.end());

    for (const auto& slot : slots) {
        std::vector<std::size_t> indices;
        for (std::size_t index = 0; index < inventory.entries.size(); ++index) {
            const auto& entry = inventory.entries[index];
            if (!marked[index] && entry.valid &&
                entry.kind == RunSaveStorageKind::ConflictBranch &&
                entry.baseSlot == slot) {
                indices.push_back(index);
            }
        }
        std::sort(
            indices.begin(), indices.end(),
            [&](std::size_t a, std::size_t b) {
                return RunSaveStorageNewer(
                    inventory.entries[a], inventory.entries[b]);
            });
        for (std::size_t offset = policy.maximumConflictBranchesPerSlot;
             offset < indices.size(); ++offset) {
            mark(indices[offset]);
        }
    }

    std::vector<std::size_t> remainingBranches;
    for (std::size_t index = 0; index < inventory.entries.size(); ++index) {
        const auto& entry = inventory.entries[index];
        if (!marked[index] && entry.valid &&
            entry.kind == RunSaveStorageKind::ConflictBranch) {
            remainingBranches.push_back(index);
        }
    }
    std::sort(
        remainingBranches.begin(), remainingBranches.end(),
        [&](std::size_t a, std::size_t b) {
            return RunSaveStorageNewer(
                inventory.entries[b], inventory.entries[a]);
        });

    for (const auto index : remainingBranches) {
        if (result.bytesAfter <= policy.maximumTotalBytes &&
            result.conflictBranchBytesAfter <=
                policy.maximumConflictBranchBytes) {
            break;
        }
        mark(index);
    }

    for (std::size_t index = 0; index < inventory.entries.size(); ++index) {
        if (marked[index]) {
            result.filesToRemove.push_back(inventory.entries[index].path);
        }
    }
    std::sort(result.filesToRemove.begin(), result.filesToRemove.end());
    result.totalBudgetSatisfied =
        result.bytesAfter <= policy.maximumTotalBytes;
    result.conflictBranchBudgetSatisfied =
        result.conflictBranchBytesAfter <=
            policy.maximumConflictBranchBytes;
    return result;
}

inline RunSaveStorageCleanupResult ApplyRunSaveStorageRetentionPlan(
    const RunSaveStorageRetentionPlan& plan,
    IRunSaveDurability& durability = DefaultRunSaveDurability()) {
    RunSaveStorageCleanupResult result;
    result.plannedFiles = static_cast<std::uint32_t>(
        plan.filesToRemove.size());
    for (const auto& path : plan.filesToRemove) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        const auto removed = durability.removeFile(path);
        if (removed) {
            ++result.removedFiles;
            if (!error) result.removedBytes += size;
        } else {
            result.failures.push_back({path, removed});
        }
    }
    return result;
}

} // namespace rts::roguelite
