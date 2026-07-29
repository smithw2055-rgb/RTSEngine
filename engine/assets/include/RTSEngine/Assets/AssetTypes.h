#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rts::assets {

enum class AssetType : std::uint16_t {
    Unknown,
    Texture2D,
    Sprite,
    AnimationClip,
    Effect,
    AudioClip,
    Font,
    Material2D,
    Shader,
    Binary,
    ScriptModule,
    ScriptBundle
};

struct AssetKey final {
    AssetType type{AssetType::Unknown};
    std::uint64_t id{};

    constexpr bool valid() const noexcept {
        return type != AssetType::Unknown && id != 0;
    }

    friend constexpr bool operator==(AssetKey a, AssetKey b) noexcept {
        return a.type == b.type && a.id == b.id;
    }

    friend constexpr bool operator!=(AssetKey a, AssetKey b) noexcept {
        return !(a == b);
    }

    friend constexpr bool operator<(AssetKey a, AssetKey b) noexcept {
        return static_cast<std::uint16_t>(a.type) <
                   static_cast<std::uint16_t>(b.type) ||
               (a.type == b.type && a.id < b.id);
    }
};

struct AssetDependency final {
    AssetKey key{};
    std::uint32_t minimumSchemaVersion{1};

    friend constexpr bool operator==(AssetDependency a,
                                     AssetDependency b) noexcept {
        return a.key == b.key &&
               a.minimumSchemaVersion == b.minimumSchemaVersion;
    }

    friend constexpr bool operator<(AssetDependency a,
                                    AssetDependency b) noexcept {
        return a.key < b.key ||
               (a.key == b.key &&
                a.minimumSchemaVersion < b.minimumSchemaVersion);
    }
};

struct CookedAsset final {
    AssetKey key{};
    std::uint32_t schemaVersion{1};
    std::uint64_t payloadHash{};
    std::vector<AssetDependency> dependencies;
    std::vector<std::uint8_t> payload;
};

struct AssetManifestEntry final {
    AssetKey key{};
    std::string path;
    std::uint32_t expectedSchemaVersion{};
};

enum class AssetState : std::uint8_t {
    Unloaded,
    Queued,
    Loading,
    Ready,
    Failed
};

enum class AssetFailure : std::uint8_t {
    None,
    UnknownAsset,
    InvalidPath,
    ReadFailed,
    DecodeFailed,
    KeyMismatch,
    SchemaMismatch,
    DependencyMissing,
    DependencyCycle,
    DependencyFailed,
    BudgetExceeded,
    Cancelled
};

struct LoadedAsset final {
    CookedAsset cooked{};
    std::uint32_t generation{};
    std::uint32_t retainCount{};
    std::uint64_t lastAccessOrdinal{};
};

struct AssetRequestHandle final {
    std::uint32_t index{};
    std::uint32_t generation{};

    constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }

    friend constexpr bool operator==(AssetRequestHandle a,
                                     AssetRequestHandle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
};

struct AssetRequestStatus final {
    AssetKey key{};
    AssetState state{AssetState::Unloaded};
    AssetFailure failure{AssetFailure::None};
    bool cancelled{};
    bool completed{};
};

struct AssetManagerStats final {
    std::size_t cpuBytes{};
    std::size_t cpuBudget{};
    std::uint32_t readyAssets{};
    std::uint32_t queuedRequests{};
    std::uint64_t completedRequests{};
    std::uint64_t failedRequests{};
    std::uint64_t cancelledRequests{};
    std::uint64_t evictedAssets{};
    std::uint64_t hotReloads{};
};

constexpr bool ValidAssetType(AssetType value) noexcept {
    return value >= AssetType::Unknown && value <= AssetType::ScriptBundle;
}

} // namespace rts::assets
