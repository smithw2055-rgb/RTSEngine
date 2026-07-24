#pragma once

#include <RTSEngine/Assets/AssetTypes.h>
#include <RTSEngine/Assets/CookedAsset.h>
#include <RTSEngine/Assets/Vfs.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::assets {

class AssetManager final {
private:
    struct Record final {
        AssetManifestEntry manifest{};
        AssetState state{AssetState::Unloaded};
        AssetFailure failure{AssetFailure::None};
        LoadedAsset loaded{};
        std::uint32_t generation{};
        std::uint32_t internalDependents{};
    };

    struct RequestSlot final {
        std::uint32_t generation{1};
        bool alive{};
        AssetRequestStatus status{};
    };

    using RecordIterator = std::vector<Record>::iterator;
    using ConstRecordIterator = std::vector<Record>::const_iterator;

public:
    explicit AssetManager(VirtualFileSystem& fileSystem,
                          std::size_t cpuBudgetBytes = 0) noexcept;

    bool registerAsset(AssetManifestEntry entry);
    AssetRequestHandle request(AssetKey key);
    bool cancel(AssetRequestHandle handle);
    bool releaseRequest(AssetRequestHandle handle);
    std::size_t process(
        std::size_t maximumRequests =
            std::numeric_limits<std::size_t>::max());

    bool requestStatus(AssetRequestHandle handle,
                       AssetRequestStatus& output) const noexcept;
    const LoadedAsset* loaded(AssetKey key);
    const LoadedAsset* peek(AssetKey key) const noexcept;
    AssetState state(AssetKey key) const noexcept;
    AssetFailure failure(AssetKey key) const noexcept;

    bool retain(AssetKey key);
    bool release(AssetKey key);
    bool unload(AssetKey key);
    bool hotReload(AssetKey key);
    bool setCpuBudget(std::size_t bytes);

    AssetManagerStats stats() const noexcept;
    std::size_t registeredAssetCount() const noexcept;

private:
    RecordIterator lowerRecord(AssetKey key);
    ConstRecordIterator lowerRecord(AssetKey key) const;
    Record* findRecord(AssetKey key) noexcept;
    const Record* findRecord(AssetKey key) const noexcept;

    AssetRequestHandle allocateRequest();
    RequestSlot* findRequest(AssetRequestHandle handle) noexcept;
    const RequestSlot* findRequest(
        AssetRequestHandle handle) const noexcept;
    bool hasPendingRequest(AssetKey key) const noexcept;

    bool loadRecursive(Record& record, std::vector<AssetKey>& stack);
    bool readCandidate(const Record& record,
                       CookedAsset& output,
                       AssetFailure& failure) const;
    void pinDependencies(
        const std::vector<AssetDependency>& dependencies);
    void unpinDependencies(
        const std::vector<AssetDependency>& dependencies) noexcept;
    bool ensureCapacity(
        std::size_t additionalBytes,
        const std::vector<AssetKey>& protectedKeys);
    void evict(Record& record, bool budgetEviction) noexcept;
    void touch(Record& record) noexcept;
    std::uint64_t nextAccessOrdinal() noexcept;
    static std::uint32_t nextGeneration(std::uint32_t value) noexcept;

    VirtualFileSystem& fileSystem_;
    std::vector<Record> records_;
    std::vector<RequestSlot> requests_;
    std::vector<AssetRequestHandle> pending_;
    std::size_t cpuBudget_{};
    std::size_t cpuBytes_{};
    std::uint64_t accessOrdinal_{};
    std::uint64_t completedRequests_{};
    std::uint64_t failedRequests_{};
    std::uint64_t cancelledRequests_{};
    std::uint64_t evictedAssets_{};
    std::uint64_t hotReloads_{};
};

} // namespace rts::assets
