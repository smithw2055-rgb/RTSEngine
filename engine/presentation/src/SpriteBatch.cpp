#include <RTSEngine/Presentation/SpriteBatch.h>

#include <algorithm>
#include <cstddef>

namespace rts::presentation {

SpriteBatchCompiler::SpriteBatchCompiler(
    std::uint32_t maximumQuads) noexcept
    : maximumQuads_(std::max<std::uint32_t>(1u, maximumQuads)) {}

Compiled2DFrame SpriteBatchCompiler::compile(
    const RenderPacket& packet,
    SpriteResolver& resolver,
    render::TextureHandle whiteTexture,
    const Camera2D& camera,
    const UiDrawList* screenUi) const {
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
        for (const auto& overlay : packet.worldOverlays) {
            if (quadCount(output) >= maximumQuads_) {
                ++output.droppedQuads;
                continue;
            }
            appendWorldOverlay(output, overlay, whiteTexture, camera);
            ++output.worldOverlayQuads;
        }
        for (const auto& ui : packet.worldUi) {
            if (quadCount(output) >= maximumQuads_) {
                ++output.droppedQuads;
                continue;
            }
            appendWorldUi(output, ui, whiteTexture, camera);
            ++output.worldUiQuads;
        }
    }

    if (screenUi && screenUi->valid()) {
        for (const auto& ui : screenUi->quads) {
            if (quadCount(output) >= maximumQuads_) {
                ++output.droppedQuads;
                continue;
            }
            if (!ui.texture.valid() || ui.rect.width <= 0.0f ||
                ui.rect.height <= 0.0f || ui.color.alpha <= 0.0f) {
                ++output.droppedQuads;
                continue;
            }
            appendScreenUi(output, ui, *screenUi);
            ++output.screenUiQuads;
        }
    }
    return output;
}

std::uint32_t SpriteBatchCompiler::quadCount(
    const Compiled2DFrame& frame) noexcept {
    return static_cast<std::uint32_t>(frame.indices.size() / 6u);
}

render::RenderPassKind SpriteBatchCompiler::mapPass(
    RenderLayer layer) noexcept {
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

float SpriteBatchCompiler::ndcX(float value,
                                const Camera2D& camera) noexcept {
    const auto left = camera.centerX - camera.worldWidth * 0.5f;
    return ((value - left) / camera.worldWidth) * 2.0f - 1.0f;
}

float SpriteBatchCompiler::ndcY(float value,
                                const Camera2D& camera) noexcept {
    const auto top = camera.centerY - camera.worldHeight * 0.5f;
    const auto normalized = (value - top) / camera.worldHeight;
    return camera.yDown ? 1.0f - normalized * 2.0f
                        : normalized * 2.0f - 1.0f;
}

void SpriteBatchCompiler::appendSprite(
    Compiled2DFrame& output,
    const SpriteInstance& instance,
    const ResolvedSprite& sprite,
    const Camera2D& camera) {
    const auto left = instance.x - sprite.width * sprite.pivotX;
    const auto top = instance.y - sprite.height * sprite.pivotY;
    const auto right = left + sprite.width;
    const auto bottom = top + sprite.height;
    appendQuad(output, mapPass(instance.layer), instance.blend,
               sprite.texture,
               left, top, right, bottom,
               sprite.u0, sprite.v0, sprite.u1, sprite.v1,
               1.0f, 1.0f, 1.0f, instance.opacity,
               camera);
}


void SpriteBatchCompiler::appendWorldOverlay(
    Compiled2DFrame& output,
    const WorldOverlayQuad& overlay,
    render::TextureHandle whiteTexture,
    const Camera2D& camera) {
    const auto halfWidth = std::max(0.0f, overlay.width) * 0.5f;
    const auto halfHeight = std::max(0.0f, overlay.height) * 0.5f;
    appendQuad(output, mapPass(overlay.layer), overlay.blend, whiteTexture,
               overlay.x - halfWidth, overlay.y - halfHeight,
               overlay.x + halfWidth, overlay.y + halfHeight,
               0.0f, 0.0f, 1.0f, 1.0f,
               overlay.red, overlay.green, overlay.blue, overlay.alpha,
               camera);
}

void SpriteBatchCompiler::appendWorldUi(
    Compiled2DFrame& output,
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


void SpriteBatchCompiler::appendScreenUi(
    Compiled2DFrame& output,
    const UiQuad& ui,
    const UiDrawList& list) {
    appendScreenQuad(output, ui, list);
}

float SpriteBatchCompiler::screenNdcX(
    float value,
    const UiDrawList& list) noexcept {
    return (value / static_cast<float>(list.framebufferWidth)) * 2.0f - 1.0f;
}

float SpriteBatchCompiler::screenNdcY(
    float value,
    const UiDrawList& list) noexcept {
    return 1.0f - (value / static_cast<float>(list.framebufferHeight)) * 2.0f;
}

void SpriteBatchCompiler::appendScreenQuad(
    Compiled2DFrame& output,
    const UiQuad& ui,
    const UiDrawList& list) {
    const auto left = ui.rect.x;
    const auto top = ui.rect.y;
    const auto right = ui.rect.x + ui.rect.width;
    const auto bottom = ui.rect.y + ui.rect.height;
    const auto baseVertex = static_cast<std::uint32_t>(output.vertices.size());
    const auto firstIndex = static_cast<std::uint32_t>(output.indices.size());
    output.vertices.push_back(
        {screenNdcX(left, list), screenNdcY(top, list),
         ui.u0, ui.v0, ui.color.red, ui.color.green,
         ui.color.blue, ui.color.alpha});
    output.vertices.push_back(
        {screenNdcX(right, list), screenNdcY(top, list),
         ui.u1, ui.v0, ui.color.red, ui.color.green,
         ui.color.blue, ui.color.alpha});
    output.vertices.push_back(
        {screenNdcX(right, list), screenNdcY(bottom, list),
         ui.u1, ui.v1, ui.color.red, ui.color.green,
         ui.color.blue, ui.color.alpha});
    output.vertices.push_back(
        {screenNdcX(left, list), screenNdcY(bottom, list),
         ui.u0, ui.v1, ui.color.red, ui.color.green,
         ui.color.blue, ui.color.alpha});
    output.indices.insert(
        output.indices.end(),
        {baseVertex, baseVertex + 1u, baseVertex + 2u,
         baseVertex, baseVertex + 2u, baseVertex + 3u});

    if (!output.batches.empty()) {
        auto& previous = output.batches.back();
        if (previous.pass == render::RenderPassKind::ScreenUi &&
            previous.blend == ui.blend &&
            previous.texture == ui.texture &&
            previous.firstIndex + previous.indexCount == firstIndex) {
            previous.indexCount += 6u;
            return;
        }
    }
    output.batches.push_back(
        {render::RenderPassKind::ScreenUi, ui.blend, ui.texture,
         firstIndex, 6u, ui.sortKey});
}

void SpriteBatchCompiler::appendQuad(
    Compiled2DFrame& output,
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

} // namespace rts::presentation
