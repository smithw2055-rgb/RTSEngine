#pragma once

#include <RTSEngine/Presentation/RenderPacket.h>
#include <RTSEngine/Render/RenderDevice.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts::presentation {

struct Camera2D final {
    float centerX{};
    float centerY{};
    float worldWidth{32.0f};
    float worldHeight{18.0f};
    bool yDown{true};

    bool valid() const noexcept {
        return worldWidth > 0.0f && worldHeight > 0.0f;
    }
};

struct ResolvedSprite final {
    render::TextureHandle texture{};
    float u0{};
    float v0{};
    float u1{1.0f};
    float v1{1.0f};
    float width{1.0f};
    float height{1.0f};
    float pivotX{0.5f};
    float pivotY{0.5f};
    render::BlendMode blend{render::BlendMode::Alpha};

    bool valid() const noexcept {
        return texture.valid() && width > 0.0f && height > 0.0f &&
               pivotX >= 0.0f && pivotX <= 1.0f &&
               pivotY >= 0.0f && pivotY <= 1.0f;
    }
};

class SpriteResolver {
public:
    virtual ~SpriteResolver() = default;
    virtual bool resolve(LogicalAssetId spriteAsset,
                         ResolvedSprite& output) = 0;
};

struct SpriteVertex final {
    float x{};
    float y{};
    float u{};
    float v{};
    float red{1.0f};
    float green{1.0f};
    float blue{1.0f};
    float alpha{1.0f};
};

struct CompiledDrawBatch final {
    render::RenderPassKind pass{render::RenderPassKind::WorldEntity};
    render::BlendMode blend{render::BlendMode::Alpha};
    render::TextureHandle texture{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::uint64_t sortKey{};
};

struct Compiled2DFrame final {
    std::vector<SpriteVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<CompiledDrawBatch> batches;
    std::uint32_t spriteQuads{};
    std::uint32_t worldUiQuads{};
    std::uint32_t unresolvedSprites{};
    std::uint32_t droppedQuads{};
};

class SpriteBatchCompiler final {
public:
    explicit SpriteBatchCompiler(std::uint32_t maximumQuads = 65536u) noexcept
        : maximumQuads_(std::max<std::uint32_t>(1u, maximumQuads)) {}

    Compiled2DFrame compile(const RenderPacket& packet,
                            SpriteResolver& resolver,
                            render::TextureHandle whiteTexture,
                            const Camera2D& camera) const {
        Compiled2DFrame output;
        if (!camera.valid()) return output;
        output.vertices.reserve(
            std::min<std::size_t>(packet.sprites.size() * 4u,
                                  maximumQuads_ * 4u));
        output.indices.reserve(
            std::min<std::size_t>(packet.sprites.size() * 6u,
                                  maximumQuads_ * 6u));

        for (const auto& sprite : packet.sprites) {
            if (quadCount(output) >= maximumQuads_) {
                ++output.droppedQuads;
                continue;
            }
            ResolvedSprite resolved;
            if (!resolver.resolve(sprite.spriteAsset, resolved) ||
                !resolved.valid()) {
                ++output.unresolvedSprites;
                continue;
            }
            appendSprite(output, sprite, resolved, camera);
            ++output.spriteQuads;
        }

        if (whiteTexture.valid()) {
            for (const auto& ui : packet.worldUi) {
                if (quadCount(output) >= maximumQuads_) {
                    ++output.droppedQuads;
                    continue;
                }
                appendWorldUi(output, ui, whiteTexture, camera);
                ++output.worldUiQuads;
            }
        }
        return output;
    }

private:
    static std::uint32_t quadCount(const Compiled2DFrame& frame) noexcept {
        return static_cast<std::uint32_t>(frame.indices.size() / 6u);
    }

    static render::RenderPassKind mapPass(RenderLayer layer) noexcept {
        switch (layer) {
        case RenderLayer::Terrain: return render::RenderPassKind::Terrain;
        case RenderLayer::WorldShadow:
            return render::RenderPassKind::WorldShadow;
        case RenderLayer::WorldEntity:
            return render::RenderPassKind::WorldEntity;
        case RenderLayer::ProjectileAndEffect:
            return render::RenderPassKind::ProjectileAndEffect;
        case RenderLayer::FogOfWar: return render::RenderPassKind::FogOfWar;
        case RenderLayer::SelectionAndDecal:
            return render::RenderPassKind::SelectionAndDecal;
        case RenderLayer::WorldUi: return render::RenderPassKind::WorldUi;
        case RenderLayer::ScreenUi: return render::RenderPassKind::ScreenUi;
        case RenderLayer::Debug: return render::RenderPassKind::Debug;
        }
        return render::RenderPassKind::WorldEntity;
    }

    static float ndcX(float value, const Camera2D& camera) noexcept {
        const auto left = camera.centerX - camera.worldWidth * 0.5f;
        return ((value - left) / camera.worldWidth) * 2.0f - 1.0f;
    }

    static float ndcY(float value, const Camera2D& camera) noexcept {
        const auto top = camera.centerY - camera.worldHeight * 0.5f;
        const auto normalized = (value - top) / camera.worldHeight;
        return camera.yDown ? 1.0f - normalized * 2.0f
                            : normalized * 2.0f - 1.0f;
    }

    static void appendSprite(Compiled2DFrame& output,
                             const SpriteInstance& instance,
                             const ResolvedSprite& sprite,
                             const Camera2D& camera) {
        const auto left = instance.x - sprite.width * sprite.pivotX;
        const auto top = instance.y - sprite.height * sprite.pivotY;
        const auto right = left + sprite.width;
        const auto bottom = top + sprite.height;
        appendQuad(output, mapPass(instance.layer), sprite.blend,
                   sprite.texture,
                   left, top, right, bottom,
                   sprite.u0, sprite.v0, sprite.u1, sprite.v1,
                   1.0f, 1.0f, 1.0f, instance.opacity,
                   camera);
    }

    static void appendWorldUi(Compiled2DFrame& output,
                              const WorldUiElement& ui,
                              render::TextureHandle whiteTexture,
                              const Camera2D& camera) {
        const auto value = std::clamp(ui.value, 0.0f, 1.0f);
        const auto width = 0.8f * value;
        const auto height = 0.1f;
        const auto left = ui.x - 0.4f;
        const auto top = ui.y - height * 0.5f;
        const auto right = left + width;
        const auto bottom = top + height;
        const bool health = ui.type == WorldUiType::HealthBar;
        appendQuad(output, render::RenderPassKind::WorldUi,
                   render::BlendMode::Alpha, whiteTexture,
                   left, top, right, bottom,
                   0.0f, 0.0f, 1.0f, 1.0f,
                   health ? 0.2f : 0.2f,
                   health ? 0.9f : 0.6f,
                   health ? 0.2f : 1.0f,
                   ui.opacity, camera);
    }

    static void appendQuad(Compiled2DFrame& output,
                           render::RenderPassKind pass,
                           render::BlendMode blend,
                           render::TextureHandle texture,
                           float left,
                           float top,
                           float right,
                           float bottom,
                           float u0,
                           float v0,
                           float u1,
                           float v1,
                           float red,
                           float green,
                           float blue,
                           float alpha,
                           const Camera2D& camera) {
        const auto baseVertex = static_cast<std::uint32_t>(
            output.vertices.size());
        const auto firstIndex = static_cast<std::uint32_t>(
            output.indices.size());
        output.vertices.push_back(
            {ndcX(left, camera), ndcY(top, camera),
             u0, v0, red, green, blue, alpha});
        output.vertices.push_back(
            {ndcX(right, camera), ndcY(top, camera),
             u1, v0, red, green, blue, alpha});
        output.vertices.push_back(
            {ndcX(right, camera), ndcY(bottom, camera),
             u1, v1, red, green, blue, alpha});
        output.vertices.push_back(
            {ndcX(left, camera), ndcY(bottom, camera),
             u0, v1, red, green, blue, alpha});
        output.indices.insert(
            output.indices.end(),
            {baseVertex, baseVertex + 1u, baseVertex + 2u,
             baseVertex, baseVertex + 2u, baseVertex + 3u});

        if (!output.batches.empty()) {
            auto& previous = output.batches.back();
            if (previous.pass == pass && previous.blend == blend &&
                previous.texture == texture &&
                previous.firstIndex + previous.indexCount == firstIndex) {
                previous.indexCount += 6u;
                return;
            }
        }
        output.batches.push_back(
            {pass, blend, texture, firstIndex, 6u,
             static_cast<std::uint64_t>(output.batches.size())});
    }

    std::uint32_t maximumQuads_;
};

} // namespace rts::presentation
