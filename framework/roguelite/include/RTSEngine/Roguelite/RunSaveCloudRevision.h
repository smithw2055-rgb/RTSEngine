#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::roguelite {

struct RunSaveCloudClockEntry final {
    std::uint64_t deviceId{};
    std::uint64_t counter{};

    friend bool operator==(
        const RunSaveCloudClockEntry& a,
        const RunSaveCloudClockEntry& b) noexcept {
        return a.deviceId == b.deviceId && a.counter == b.counter;
    }
};

struct RunSaveCloudRevision final {
    static constexpr std::uint32_t kMaximumParents = 2u;
    static constexpr std::uint32_t kMaximumDevices = 32u;

    std::uint64_t lineageId{};
    std::uint64_t revisionId{};
    std::uint64_t deviceId{};
    std::uint64_t logicalClock{};
    std::vector<std::uint64_t> parentRevisionIds;
    std::vector<RunSaveCloudClockEntry> vectorClock;

    bool tracked() const noexcept {
        return lineageId != 0 || revisionId != 0 || deviceId != 0 ||
               logicalClock != 0 || !parentRevisionIds.empty() ||
               !vectorClock.empty();
    }
};

inline bool ValidateRunSaveCloudRevision(
    const RunSaveCloudRevision& value,
    bool allowUnfinalizedRevision = false) noexcept {
    if (!value.tracked()) return true;
    if (value.lineageId == 0 || value.deviceId == 0 ||
        value.logicalClock == 0 ||
        (!allowUnfinalizedRevision && value.revisionId == 0) ||
        value.parentRevisionIds.size() >
            RunSaveCloudRevision::kMaximumParents ||
        value.vectorClock.empty() ||
        value.vectorClock.size() > RunSaveCloudRevision::kMaximumDevices) {
        return false;
    }

    std::uint64_t previousParent = 0;
    for (const auto parent : value.parentRevisionIds) {
        if (parent == 0 || parent <= previousParent ||
            parent == value.revisionId) {
            return false;
        }
        previousParent = parent;
    }

    bool foundCurrentDevice = false;
    std::uint64_t previousDevice = 0;
    for (const auto& entry : value.vectorClock) {
        if (entry.deviceId == 0 || entry.counter == 0 ||
            entry.deviceId <= previousDevice) {
            return false;
        }
        previousDevice = entry.deviceId;
        if (entry.deviceId == value.deviceId) {
            if (entry.counter != value.logicalClock) return false;
            foundCurrentDevice = true;
        }
    }
    return foundCurrentDevice;
}

inline std::uint64_t RunSaveCloudClockValue(
    const RunSaveCloudRevision& value,
    std::uint64_t deviceId) noexcept {
    const auto iterator = std::lower_bound(
        value.vectorClock.begin(),
        value.vectorClock.end(),
        deviceId,
        [](const RunSaveCloudClockEntry& entry, std::uint64_t key) {
            return entry.deviceId < key;
        });
    return iterator != value.vectorClock.end() &&
                   iterator->deviceId == deviceId
        ? iterator->counter
        : 0u;
}

inline bool RunSaveCloudClockDominates(
    const RunSaveCloudRevision& descendant,
    const RunSaveCloudRevision& ancestor) noexcept {
    if (!ValidateRunSaveCloudRevision(descendant) ||
        !ValidateRunSaveCloudRevision(ancestor) ||
        !descendant.tracked() || !ancestor.tracked() ||
        descendant.lineageId != ancestor.lineageId) {
        return false;
    }

    bool strictlyGreater = false;
    for (const auto& entry : ancestor.vectorClock) {
        const auto current = RunSaveCloudClockValue(
            descendant, entry.deviceId);
        if (current < entry.counter) return false;
        strictlyGreater = strictlyGreater || current > entry.counter;
    }
    if (!strictlyGreater) {
        for (const auto& entry : descendant.vectorClock) {
            if (RunSaveCloudClockValue(ancestor, entry.deviceId) == 0) {
                strictlyGreater = true;
                break;
            }
        }
    }
    return strictlyGreater;
}

inline std::uint64_t RunSaveCloudClockTotal(
    const RunSaveCloudRevision& value) noexcept {
    std::uint64_t total = 0;
    for (const auto& entry : value.vectorClock) {
        const auto next = total + entry.counter;
        if (next < total) return ~std::uint64_t{0};
        total = next;
    }
    return total;
}

inline RunSaveCloudRevision MakeRunSaveCloudRevision(
    std::uint64_t lineageId,
    std::uint64_t deviceId,
    std::vector<RunSaveCloudRevision> parents = {}) {
    RunSaveCloudRevision result;
    if (lineageId == 0 || deviceId == 0 ||
        parents.size() > RunSaveCloudRevision::kMaximumParents) {
        return result;
    }

    result.lineageId = lineageId;
    result.deviceId = deviceId;

    for (const auto& parent : parents) {
        if (!ValidateRunSaveCloudRevision(parent) || !parent.tracked() ||
            parent.lineageId != lineageId) {
            return {};
        }
        result.parentRevisionIds.push_back(parent.revisionId);
        for (const auto& entry : parent.vectorClock) {
            auto iterator = std::lower_bound(
                result.vectorClock.begin(),
                result.vectorClock.end(),
                entry.deviceId,
                [](const RunSaveCloudClockEntry& current,
                   std::uint64_t key) {
                    return current.deviceId < key;
                });
            if (iterator != result.vectorClock.end() &&
                iterator->deviceId == entry.deviceId) {
                iterator->counter = std::max(
                    iterator->counter, entry.counter);
            } else {
                result.vectorClock.insert(iterator, entry);
            }
        }
    }

    std::sort(
        result.parentRevisionIds.begin(),
        result.parentRevisionIds.end());
    result.parentRevisionIds.erase(
        std::unique(
            result.parentRevisionIds.begin(),
            result.parentRevisionIds.end()),
        result.parentRevisionIds.end());
    if (result.parentRevisionIds.size() != parents.size() ||
        result.vectorClock.size() > RunSaveCloudRevision::kMaximumDevices) {
        return {};
    }

    auto iterator = std::lower_bound(
        result.vectorClock.begin(),
        result.vectorClock.end(),
        deviceId,
        [](const RunSaveCloudClockEntry& entry, std::uint64_t key) {
            return entry.deviceId < key;
        });
    if (iterator != result.vectorClock.end() &&
        iterator->deviceId == deviceId) {
        if (iterator->counter == ~std::uint64_t{0}) return {};
        ++iterator->counter;
        result.logicalClock = iterator->counter;
    } else {
        if (result.vectorClock.size() ==
            RunSaveCloudRevision::kMaximumDevices) {
            return {};
        }
        result.logicalClock = 1u;
        result.vectorClock.insert(iterator, {deviceId, 1u});
    }

    if (!ValidateRunSaveCloudRevision(result, true)) return {};
    return result;
}

} // namespace rts::roguelite
