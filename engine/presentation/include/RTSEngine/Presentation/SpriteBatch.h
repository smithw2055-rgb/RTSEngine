#pragma once

#include <RTSEngine/Presentation/RenderPacket.h>
#include <RTSEngine/Presentation/ScreenUi.h>
#include <RTSEngine/Render/RenderDevice.h>

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
    std::uint32_t worldOverlayQuads{};
    std::uint32_t screenUiQuads{};
    std::uint32_t unresolvedSprites{};
    std::uint32_t droppedQuads{};
};

class SpriteBatchCompiler final {
public:
    explicit SpriteBatchCompiler(
        std::uint32_t maximumQuads = 65536u) noexcept;

    Compiled2DFrame compile(const RenderPacket& packet,
                            SpriteResolver& resolver,
                            render::TextureHandle whiteTexture,
                            const Camera2D& camera,
                            const UiDrawList* screenUi = nullptr) const;

private:
    static std::uint32_t quadCount(
        const Compiled2DFrame& frame) noexcept;
    static render::RenderPassKind mapPass(RenderLayer layer) noexcept;
    static float ndcX(float value, const Camera2D& camera) noexcept;
    static float ndcY(float value, const Camera2D& camera) noexcept;

    static void appendSprite(Compiled2DFrame& output,
                             const SpriteInstance& instance,
                             const ResolvedSprite& sprite,
                             const Camera2D& camera);
    static void appendWorldOverlay(Compiled2DFrame& output,
                                   const WorldOverlayQuad& overlay,
                                   render::TextureHandle whiteTexture,
                                   const Camera2D& camera);
    static void appendWorldUi(Compiled2DFrame& output,
                              const WorldUiElement& ui,
                              render::TextureHandle whiteTexture,
                              const Camera2D& camera);
    static void appendScreenUi(Compiled2DFrame& output,
                               const UiQuad& ui,
                               const UiDrawList& list);
    static float screenNdcX(float value,
                            const UiDrawList& list) noexcept;
    static float screenNdcY(float value,
                            const UiDrawList& list) noexcept;
    static void appendScreenQuad(Compiled2DFrame& output,
                                 const UiQuad& ui,
                                 const UiDrawList& list);
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
                           const Camera2D& camera);

    std::uint32_t maximumQuads_;
};

} // namespace rts::presentation
