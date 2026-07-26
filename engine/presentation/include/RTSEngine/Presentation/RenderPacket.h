#pragma once

#include <RTSEngine/Presentation/SnapshotInterpolation.h>
#include <RTSEngine/Render/RenderDevice.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::presentation {

enum class WorldUiType : std::uint8_t {
    HealthBar,
    ConstructionProgress
};

struct SpriteInstance final {
    ViewId viewId{};
    LogicalAssetId spriteAsset{};
    LogicalAssetId animationAsset{};
    RenderLayer layer{RenderLayer::WorldEntity};
    float x{};
    float y{};
    float opacity{1.0f};
    std::int32_t sortBias{};
    ViewLifecycle lifecycle{ViewLifecycle::Stable};
    render::BlendMode blend{render::BlendMode::Alpha};
};

struct WorldUiElement final {
    ViewId viewId{};
    WorldUiType type{WorldUiType::HealthBar};
    float x{};
    float y{};
    float value{};
    float opacity{1.0f};
};


struct WorldOverlayQuad final {
    float x{};
    float y{};
    float width{1.0f};
    float height{1.0f};
    float red{1.0f};
    float green{1.0f};
    float blue{1.0f};
    float alpha{0.25f};
    RenderLayer layer{RenderLayer::SelectionAndDecal};
    render::BlendMode blend{render::BlendMode::Alpha};
    std::int32_t sortBias{};
};

struct RenderPacket final {
    std::uint64_t previousTick{};
    std::uint64_t currentTick{};
    std::uint64_t simulationHash{};
    float alpha{};
    std::int32_t availableResources{};
    VisibilityMask visibility{};
    std::vector<SpriteInstance> sprites;
    std::vector<WorldUiElement> worldUi;
    std::vector<WorldOverlayQuad> worldOverlays;
};

class RenderPacketBuilder final {
public:
    static RenderPacket build(const InterpolatedScene& scene) {
        RenderPacket packet;
        packet.previousTick = scene.previousTick;
        packet.currentTick = scene.currentTick;
        packet.simulationHash = scene.simulationHash;
        packet.alpha = scene.alpha;
        packet.availableResources = scene.availableResources;
        packet.visibility = scene.visibility;

        packet.sprites.reserve(scene.entities.size());
        for (const auto& interpolated : scene.entities) {
            const auto& entity = interpolated.current;
            if (!entity.visible || entity.spriteAsset == 0 ||
                interpolated.opacity <= 0.0f) {
                continue;
            }
            packet.sprites.push_back(
                {entity.viewId,
                 entity.spriteAsset,
                 entity.animationAsset,
                 entity.layer,
                 interpolated.x,
                 interpolated.y,
                 std::clamp(interpolated.opacity, 0.0f, 1.0f),
                 entity.sortBias,
                 interpolated.lifecycle,
                 render::BlendMode::Alpha});

            appendWorldUi(packet.worldUi, interpolated);
        }

        sort(packet);
        return packet;
    }

    static void sort(RenderPacket& packet) {
        std::stable_sort(
            packet.sprites.begin(), packet.sprites.end(),
            [](const SpriteInstance& a, const SpriteInstance& b) {
                if (a.layer != b.layer) {
                    return static_cast<std::uint8_t>(a.layer) <
                           static_cast<std::uint8_t>(b.layer);
                }
                if (a.y != b.y) return a.y < b.y;
                if (a.sortBias != b.sortBias) return a.sortBias < b.sortBias;
                return a.viewId < b.viewId;
            });
        std::stable_sort(
            packet.worldOverlays.begin(), packet.worldOverlays.end(),
            [](const WorldOverlayQuad& a, const WorldOverlayQuad& b) {
                if (a.layer != b.layer) {
                    return static_cast<std::uint8_t>(a.layer) <
                           static_cast<std::uint8_t>(b.layer);
                }
                if (a.y != b.y) return a.y < b.y;
                return a.sortBias < b.sortBias;
            });
        std::stable_sort(
            packet.worldUi.begin(), packet.worldUi.end(),
            [](const WorldUiElement& a, const WorldUiElement& b) {
                if (a.y != b.y) return a.y < b.y;
                if (a.viewId != b.viewId) return a.viewId < b.viewId;
                return static_cast<std::uint8_t>(a.type) <
                       static_cast<std::uint8_t>(b.type);
            });
    }

private:
    static float ratio(std::int64_t current,
                       std::int64_t maximum) noexcept {
        if (maximum <= 0) return 0.0f;
        return std::clamp(
            static_cast<float>(current) / static_cast<float>(maximum),
            0.0f, 1.0f);
    }

    static void appendWorldUi(
        std::vector<WorldUiElement>& output,
        const InterpolatedEntity& interpolated) {
        const auto& entity = interpolated.current;
        const auto opacity = std::clamp(interpolated.opacity, 0.0f, 1.0f);
        if (entity.healthMaximum > 0 &&
            entity.healthCurrent < entity.healthMaximum) {
            output.push_back(
                {entity.viewId,
                 WorldUiType::HealthBar,
                 interpolated.x,
                 interpolated.y - 0.75f,
                 ratio(entity.healthCurrent, entity.healthMaximum),
                 opacity});
        }
        if (entity.requiredTicks > 0 &&
            entity.progressTicks < entity.requiredTicks) {
            output.push_back(
                {entity.viewId,
                 WorldUiType::ConstructionProgress,
                 interpolated.x,
                 interpolated.y - 0.5f,
                 ratio(entity.progressTicks, entity.requiredTicks),
                 opacity});
        }
    }
};

} // namespace rts::presentation
