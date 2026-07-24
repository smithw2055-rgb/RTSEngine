#include <RTSEngine/Assets/AssetManager.h>

#include <algorithm>
#include <string>
#include <utility>

namespace rts::assets {

AssetManager::AssetManager(VirtualFileSystem& fileSystem,
                           std::size_t cpuBudgetBytes) noexcept
    : fileSystem_(fileSystem), cpuBudget_(cpuBudgetBytes) {}

bool AssetManager::registerAsset(AssetManifestEntry entry) {
    std::string normalized;
    if (!entry.key.valid() ||
        !NormalizeVirtualPath(entry.path, normalized)) {
        return false;
    }
    entry.path = std::move(normalized);
    const auto iterator = lowerRecord(entry.key);
    if (iterator != records_.end() && iterator->manifest.key == entry.key) {
        return false;
    }
    if (std::any_of(records_.begin(), records_.end(),
                    [&](const Record& value) {
                        return value.manifest.path == entry.path;
                    })) {
        return false;
    }
    Record record;
    record.manifest = std::move(entry);
    records_.insert(iterator, std::move(record));
    return true;
}

AssetRequestHandle AssetManager::request(AssetKey key) {
    auto* record = findRecord(key);
    if (!record) return {};

    const auto handle = allocateRequest();
    auto* slot = findRequest(handle);
    slot->status.key = key;
    if (record->state == AssetState::Ready) {
        touch(*record);
        slot->status.state = AssetState::Ready;
        slot->status.completed = true;
        ++completedRequests_;
        return handle;
    }

    if (record->state == AssetState::Unloaded ||
        record->state == AssetState::Failed) {
        record->state = AssetState::Queued;
        record->failure = AssetFailure::None;
    }
    slot->status.state = AssetState::Queued;
    pending_.push_back(handle);
    return handle;
}

bool AssetManager::cancel(AssetRequestHandle handle) {
    auto* slot = findRequest(handle);
    if (!slot || slot->status.completed || slot->status.cancelled) {
        return false;
    }
    slot->status.cancelled = true;
    return true;
}

bool AssetManager::releaseRequest(AssetRequestHandle handle) {
    auto* slot = findRequest(handle);
    if (!slot || !slot->status.completed) return false;
    slot->alive = false;
    slot->status = {};
    slot->generation = nextGeneration(slot->generation);
    return true;
}

std::size_t AssetManager::process(std::size_t maximumRequests) {
    std::size_t processed = 0;
    while (processed < maximumRequests && !pending_.empty()) {
        const auto handle = pending_.front();
        pending_.erase(pending_.begin());
        auto* slot = findRequest(handle);
        if (!slot || slot->status.completed) continue;

        auto* record = findRecord(slot->status.key);
        if (slot->status.cancelled) {
            slot->status.state = record ? record->state
                                        : AssetState::Failed;
            slot->status.failure = AssetFailure::Cancelled;
            slot->status.completed = true;
            if (record && record->state == AssetState::Queued &&
                !hasPendingRequest(record->manifest.key)) {
                record->state = AssetState::Unloaded;
            }
            ++cancelledRequests_;
            ++processed;
            continue;
        }

        std::vector<AssetKey> stack;
        const bool loaded = record && loadRecursive(*record, stack);
        slot->status.state = loaded ? AssetState::Ready
                                    : AssetState::Failed;
        slot->status.failure = loaded
            ? AssetFailure::None
            : (record ? record->failure : AssetFailure::UnknownAsset);
        slot->status.completed = true;
        if (loaded) {
            ++completedRequests_;
        } else {
            ++failedRequests_;
        }
        ++processed;
    }
    return processed;
}

bool AssetManager::requestStatus(
    AssetRequestHandle handle,
    AssetRequestStatus& output) const noexcept {
    const auto* slot = findRequest(handle);
    if (!slot) return false;
    output = slot->status;
    if (!output.completed) {
        const auto* record = findRecord(output.key);
        if (record) {
            output.state = record->state;
            if (record->failure != AssetFailure::None) {
                output.failure = record->failure;
            }
        }
    }
    return true;
}

const LoadedAsset* AssetManager::loaded(AssetKey key) {
    auto* record = findRecord(key);
    if (!record || record->state != AssetState::Ready) return nullptr;
    touch(*record);
    return &record->loaded;
}

const LoadedAsset* AssetManager::peek(AssetKey key) const noexcept {
    const auto* record = findRecord(key);
    return record && record->state == AssetState::Ready
        ? &record->loaded : nullptr;
}

AssetState AssetManager::state(AssetKey key) const noexcept {
    const auto* record = findRecord(key);
    return record ? record->state : AssetState::Failed;
}

AssetFailure AssetManager::failure(AssetKey key) const noexcept {
    const auto* record = findRecord(key);
    return record ? record->failure : AssetFailure::UnknownAsset;
}

bool AssetManager::retain(AssetKey key) {
    auto* record = findRecord(key);
    if (!record || record->state != AssetState::Ready ||
        record->loaded.retainCount ==
            std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    ++record->loaded.retainCount;
    touch(*record);
    return true;
}

bool AssetManager::release(AssetKey key) {
    auto* record = findRecord(key);
    if (!record || record->state != AssetState::Ready ||
        record->loaded.retainCount == 0) {
        return false;
    }
    --record->loaded.retainCount;
    return true;
}

bool AssetManager::unload(AssetKey key) {
    auto* record = findRecord(key);
    if (!record || record->state != AssetState::Ready ||
        record->loaded.retainCount != 0 ||
        record->internalDependents != 0) {
        return false;
    }
    evict(*record, false);
    return true;
}

bool AssetManager::hotReload(AssetKey key) {
    auto* record = findRecord(key);
    if (!record || record->state != AssetState::Ready) return false;

    CookedAsset candidate;
    AssetFailure candidateFailure = AssetFailure::None;
    if (!readCandidate(*record, candidate, candidateFailure)) {
        record->failure = candidateFailure;
        return false;
    }

    std::vector<AssetKey> stack{key};
    for (const auto& dependency : candidate.dependencies) {
        auto* dependencyRecord = findRecord(dependency.key);
        if (!dependencyRecord) {
            record->failure = AssetFailure::DependencyMissing;
            return false;
        }
        if (!loadRecursive(*dependencyRecord, stack) ||
            dependencyRecord->loaded.cooked.schemaVersion <
                dependency.minimumSchemaVersion) {
            record->failure = dependencyRecord->failure ==
                    AssetFailure::DependencyCycle
                ? AssetFailure::DependencyCycle
                : AssetFailure::DependencyFailed;
            return false;
        }
    }

    const auto oldBytes = record->loaded.cooked.payload.size();
    const auto newBytes = candidate.payload.size();
    std::vector<AssetKey> protectedKeys{key};
    for (const auto& dependency : candidate.dependencies) {
        protectedKeys.push_back(dependency.key);
    }
    if (newBytes > oldBytes &&
        !ensureCapacity(newBytes - oldBytes, protectedKeys)) {
        record->failure = AssetFailure::BudgetExceeded;
        return false;
    }

    unpinDependencies(record->loaded.cooked.dependencies);
    cpuBytes_ = oldBytes <= cpuBytes_ ? cpuBytes_ - oldBytes : 0;
    record->loaded.cooked = std::move(candidate);
    record->generation = nextGeneration(record->generation);
    record->loaded.generation = record->generation;
    record->loaded.lastAccessOrdinal = nextAccessOrdinal();
    cpuBytes_ += record->loaded.cooked.payload.size();
    pinDependencies(record->loaded.cooked.dependencies);
    record->failure = AssetFailure::None;
    ++hotReloads_;
    return true;
}

bool AssetManager::setCpuBudget(std::size_t bytes) {
    const auto previous = cpuBudget_;
    cpuBudget_ = bytes;
    if (ensureCapacity(0, {})) return true;
    cpuBudget_ = previous;
    return false;
}

AssetManagerStats AssetManager::stats() const noexcept {
    AssetManagerStats result;
    result.cpuBytes = cpuBytes_;
    result.cpuBudget = cpuBudget_;
    result.readyAssets = static_cast<std::uint32_t>(std::count_if(
        records_.begin(), records_.end(),
        [](const Record& value) {
            return value.state == AssetState::Ready;
        }));
    result.queuedRequests = static_cast<std::uint32_t>(pending_.size());
    result.completedRequests = completedRequests_;
    result.failedRequests = failedRequests_;
    result.cancelledRequests = cancelledRequests_;
    result.evictedAssets = evictedAssets_;
    result.hotReloads = hotReloads_;
    return result;
}

std::size_t AssetManager::registeredAssetCount() const noexcept {
    return records_.size();
}

AssetManager::RecordIterator AssetManager::lowerRecord(AssetKey key) {
    return std::lower_bound(
        records_.begin(), records_.end(), key,
        [](const Record& value, AssetKey lookup) {
            return value.manifest.key < lookup;
        });
}

AssetManager::ConstRecordIterator AssetManager::lowerRecord(
    AssetKey key) const {
    return std::lower_bound(
        records_.begin(), records_.end(), key,
        [](const Record& value, AssetKey lookup) {
            return value.manifest.key < lookup;
        });
}

AssetManager::Record* AssetManager::findRecord(AssetKey key) noexcept {
    const auto iterator = lowerRecord(key);
    return iterator != records_.end() && iterator->manifest.key == key
        ? &*iterator : nullptr;
}

const AssetManager::Record* AssetManager::findRecord(
    AssetKey key) const noexcept {
    const auto iterator = lowerRecord(key);
    return iterator != records_.end() && iterator->manifest.key == key
        ? &*iterator : nullptr;
}

AssetRequestHandle AssetManager::allocateRequest() {
    std::size_t index = 0;
    for (; index < requests_.size(); ++index) {
        if (!requests_[index].alive) break;
    }
    if (index == requests_.size()) requests_.push_back({});
    auto& slot = requests_[index];
    if (slot.generation == 0) slot.generation = 1;
    slot.alive = true;
    slot.status = {};
    return {static_cast<std::uint32_t>(index + 1u), slot.generation};
}

AssetManager::RequestSlot* AssetManager::findRequest(
    AssetRequestHandle handle) noexcept {
    if (!handle.valid() || handle.index > requests_.size()) return nullptr;
    auto& slot = requests_[handle.index - 1u];
    return slot.alive && slot.generation == handle.generation
        ? &slot : nullptr;
}

const AssetManager::RequestSlot* AssetManager::findRequest(
    AssetRequestHandle handle) const noexcept {
    if (!handle.valid() || handle.index > requests_.size()) return nullptr;
    const auto& slot = requests_[handle.index - 1u];
    return slot.alive && slot.generation == handle.generation
        ? &slot : nullptr;
}

bool AssetManager::hasPendingRequest(AssetKey key) const noexcept {
    return std::any_of(
        pending_.begin(), pending_.end(),
        [&](AssetRequestHandle handle) {
            const auto* slot = findRequest(handle);
            return slot && !slot->status.completed &&
                   !slot->status.cancelled && slot->status.key == key;
        });
}

bool AssetManager::loadRecursive(
    Record& record,
    std::vector<AssetKey>& stack) {
    if (record.state == AssetState::Ready) {
        touch(record);
        return true;
    }
    if (record.state == AssetState::Loading ||
        std::find(stack.begin(), stack.end(), record.manifest.key) !=
            stack.end()) {
        record.failure = AssetFailure::DependencyCycle;
        return false;
    }

    record.state = AssetState::Loading;
    record.failure = AssetFailure::None;
    stack.push_back(record.manifest.key);

    CookedAsset candidate;
    AssetFailure candidateFailure = AssetFailure::None;
    if (!readCandidate(record, candidate, candidateFailure)) {
        record.state = AssetState::Failed;
        record.failure = candidateFailure;
        stack.pop_back();
        return false;
    }

    std::vector<AssetKey> protectedKeys{record.manifest.key};
    for (const auto& dependency : candidate.dependencies) {
        auto* dependencyRecord = findRecord(dependency.key);
        if (!dependencyRecord) {
            record.state = AssetState::Failed;
            record.failure = AssetFailure::DependencyMissing;
            stack.pop_back();
            return false;
        }
        if (!loadRecursive(*dependencyRecord, stack) ||
            dependencyRecord->loaded.cooked.schemaVersion <
                dependency.minimumSchemaVersion) {
            record.state = AssetState::Failed;
            record.failure = dependencyRecord->failure ==
                    AssetFailure::DependencyCycle
                ? AssetFailure::DependencyCycle
                : AssetFailure::DependencyFailed;
            stack.pop_back();
            return false;
        }
        protectedKeys.push_back(dependency.key);
    }

    if (!ensureCapacity(candidate.payload.size(), protectedKeys)) {
        record.state = AssetState::Failed;
        record.failure = AssetFailure::BudgetExceeded;
        stack.pop_back();
        return false;
    }

    record.generation = nextGeneration(record.generation);
    record.loaded = {};
    record.loaded.cooked = std::move(candidate);
    record.loaded.generation = record.generation;
    record.loaded.lastAccessOrdinal = nextAccessOrdinal();
    record.state = AssetState::Ready;
    record.failure = AssetFailure::None;
    cpuBytes_ += record.loaded.cooked.payload.size();
    pinDependencies(record.loaded.cooked.dependencies);
    stack.pop_back();
    return true;
}

bool AssetManager::readCandidate(
    const Record& record,
    CookedAsset& output,
    AssetFailure& failure) const {
    std::vector<std::uint8_t> bytes;
    if (!fileSystem_.read(record.manifest.path, bytes)) {
        failure = AssetFailure::ReadFailed;
        return false;
    }
    if (!DecodeCookedAsset(bytes, output)) {
        failure = AssetFailure::DecodeFailed;
        return false;
    }
    if (output.key != record.manifest.key) {
        failure = AssetFailure::KeyMismatch;
        return false;
    }
    if (record.manifest.expectedSchemaVersion != 0 &&
        output.schemaVersion != record.manifest.expectedSchemaVersion) {
        failure = AssetFailure::SchemaMismatch;
        return false;
    }
    return true;
}

void AssetManager::pinDependencies(
    const std::vector<AssetDependency>& dependencies) {
    for (const auto& dependency : dependencies) {
        auto* record = findRecord(dependency.key);
        if (record && record->internalDependents !=
                          std::numeric_limits<std::uint32_t>::max()) {
            ++record->internalDependents;
        }
    }
}

void AssetManager::unpinDependencies(
    const std::vector<AssetDependency>& dependencies) noexcept {
    for (const auto& dependency : dependencies) {
        auto* record = findRecord(dependency.key);
        if (record && record->internalDependents != 0) {
            --record->internalDependents;
        }
    }
}

bool AssetManager::ensureCapacity(
    std::size_t additionalBytes,
    const std::vector<AssetKey>& protectedKeys) {
    if (cpuBudget_ == 0) return true;
    if (additionalBytes > cpuBudget_) return false;

    while (cpuBytes_ > cpuBudget_ - additionalBytes) {
        Record* candidate = nullptr;
        for (auto& record : records_) {
            if (record.state != AssetState::Ready ||
                record.loaded.retainCount != 0 ||
                record.internalDependents != 0 ||
                std::find(protectedKeys.begin(), protectedKeys.end(),
                          record.manifest.key) != protectedKeys.end()) {
                continue;
            }
            if (!candidate ||
                record.loaded.lastAccessOrdinal <
                    candidate->loaded.lastAccessOrdinal ||
                (record.loaded.lastAccessOrdinal ==
                     candidate->loaded.lastAccessOrdinal &&
                 record.manifest.key < candidate->manifest.key)) {
                candidate = &record;
            }
        }
        if (!candidate) return false;
        evict(*candidate, true);
    }
    return true;
}

void AssetManager::evict(Record& record, bool budgetEviction) noexcept {
    if (record.state != AssetState::Ready) return;
    unpinDependencies(record.loaded.cooked.dependencies);
    const auto bytes = record.loaded.cooked.payload.size();
    cpuBytes_ = bytes <= cpuBytes_ ? cpuBytes_ - bytes : 0;
    record.loaded = {};
    record.state = AssetState::Unloaded;
    record.failure = AssetFailure::None;
    if (budgetEviction) ++evictedAssets_;
}

void AssetManager::touch(Record& record) noexcept {
    record.loaded.lastAccessOrdinal = nextAccessOrdinal();
}

std::uint64_t AssetManager::nextAccessOrdinal() noexcept {
    ++accessOrdinal_;
    if (accessOrdinal_ == 0) accessOrdinal_ = 1;
    return accessOrdinal_;
}

std::uint32_t AssetManager::nextGeneration(std::uint32_t value) noexcept {
    ++value;
    return value == 0 ? 1u : value;
}

} // namespace rts::assets
