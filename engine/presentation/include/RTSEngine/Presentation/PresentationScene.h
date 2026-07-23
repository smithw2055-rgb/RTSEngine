#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::presentation {

using ViewId = std::uint64_t;
using LogicalAssetId = std::uint64_t;

enum class SceneEntityKind : std::uint8_t {
    Unit,
    Construction,
    Building,
    Projectile,
    Effect
};

enum class RenderLayer : std::uint8_t {
    Terrain,
    WorldShadow,
    WorldEntity,
    ProjectileAndEffect,
    FogOfWar,
    SelectionAndDecal,
    WorldUi,
    ScreenUi,
    Debug
};

struct VisualKey final {
    SceneEntityKind kind{SceneEntityKind::Unit};
    std::uint32_t definitionId{};

    friend constexpr bool operator==(VisualKey a, VisualKey b) noexcept {
        return a.kind == b.kind && a.definitionId == b.definitionId;
    }

    friend constexpr bool operator<(VisualKey a, VisualKey b) noexcept {
        return static_cast<std::uint8_t>(a.kind) <
                   static_cast<std::uint8_t>(b.kind) ||
               (a.kind == b.kind && a.definitionId < b.definitionId);
    }
};

struct VisualBinding final {
    VisualKey key{};
    LogicalAssetId spriteAsset{};
    LogicalAssetId animationAsset{};
    RenderLayer layer{RenderLayer::WorldEntity};
    std::int32_t sortBias{};
};

class VisualCatalog final {
public:
    bool upsert(VisualBinding binding) {
        if (binding.key.definitionId == 0 || binding.spriteAsset == 0) {
            return false;
        }
        const auto iterator = std::lower_bound(
            bindings_.begin(), bindings_.end(), binding.key,
            [](const VisualBinding& value, VisualKey key) {
                return value.key < key;
            });
        if (iterator != bindings_.end() && iterator->key == binding.key) {
            *iterator = binding;
        } else {
            bindings_.insert(iterator, binding);
        }
        return true;
    }

    const VisualBinding* resolve(SceneEntityKind kind,
                                 std::uint32_t definitionId) const noexcept {
        const VisualKey key{kind, definitionId};
        const auto iterator = std::lower_bound(
            bindings_.begin(), bindings_.end(), key,
            [](const VisualBinding& value, VisualKey lookup) {
                return value.key < lookup;
            });
        return iterator != bindings_.end() && iterator->key == key
            ? &*iterator : nullptr;
    }

    const std::vector<VisualBinding>& bindings() const noexcept {
        return bindings_;
    }

private:
    std::vector<VisualBinding> bindings_;
};

struct PresentationEntity final {
    ViewId viewId{};
    std::uint32_t sourceIndex{};
    std::uint32_t sourceGeneration{};
    SceneEntityKind kind{SceneEntityKind::Unit};
    std::uint32_t definitionId{};
    std::uint32_t teamId{};
    float x{};
    float y{};
    std::int32_t healthCurrent{};
    std::int32_t healthMaximum{};
    std::uint32_t progressTicks{};
    std::uint32_t requiredTicks{};
    bool moving{};
    bool visible{true};
    LogicalAssetId spriteAsset{};
    LogicalAssetId animationAsset{};
    RenderLayer layer{RenderLayer::WorldEntity};
    std::int32_t sortBias{};

    friend bool operator==(const PresentationEntity& a,
                           const PresentationEntity& b) noexcept {
        return a.viewId == b.viewId &&
               a.sourceIndex == b.sourceIndex &&
               a.sourceGeneration == b.sourceGeneration &&
               a.kind == b.kind &&
               a.definitionId == b.definitionId &&
               a.teamId == b.teamId &&
               a.x == b.x && a.y == b.y &&
               a.healthCurrent == b.healthCurrent &&
               a.healthMaximum == b.healthMaximum &&
               a.progressTicks == b.progressTicks &&
               a.requiredTicks == b.requiredTicks &&
               a.moving == b.moving && a.visible == b.visible &&
               a.spriteAsset == b.spriteAsset &&
               a.animationAsset == b.animationAsset &&
               a.layer == b.layer && a.sortBias == b.sortBias;
    }
};

struct VisibilityMask final {
    std::int32_t width{};
    std::int32_t height{};
    std::vector<std::uint8_t> current;
    std::vector<std::uint8_t> explored;
};

struct PresentationScene final {
    std::uint64_t tick{};
    std::uint64_t simulationHash{};
    std::int32_t availableResources{};
    std::uint32_t observedTeam{};
    VisibilityMask visibility{};
    std::vector<PresentationEntity> entities;
};

constexpr ViewId MakeViewId(std::uint32_t index,
                            std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32u) |
           static_cast<std::uint64_t>(index);
}

inline bool IsCanonicalScene(const PresentationScene& scene) noexcept {
    ViewId previous = 0;
    for (const auto& entity : scene.entities) {
        if (entity.viewId == 0 || entity.viewId <= previous) return false;
        previous = entity.viewId;
    }
    const auto expected = scene.visibility.width > 0 &&
                          scene.visibility.height > 0
        ? static_cast<std::size_t>(scene.visibility.width) *
          static_cast<std::size_t>(scene.visibility.height)
        : 0u;
    return scene.visibility.current.size() == expected &&
           scene.visibility.explored.size() == expected;
}

} // namespace rts::presentation
