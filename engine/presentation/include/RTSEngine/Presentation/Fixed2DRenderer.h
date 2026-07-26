#pragma once

#include <RTSEngine/Presentation/SpriteBatch.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace rts::presentation {

struct Fixed2DRendererStats final {
    std::uint64_t submittedFrames{};
    std::uint32_t drawCalls{};
    std::uint32_t spriteQuads{};
    std::uint32_t worldUiQuads{};
    std::uint32_t worldOverlayQuads{};
    std::uint32_t screenUiQuads{};
    std::uint32_t unresolvedSprites{};
    std::uint32_t droppedQuads{};
    std::size_t vertexBytes{};
    std::size_t indexBytes{};
};

class Fixed2DRenderer final {
public:
    static constexpr render::ShaderKey kSpriteShaderKey =
        0x5254533253505249ull; // "RTS2SPRI"

    Fixed2DRenderer(render::RenderDevice& device,
                    SpriteResolver& resolver,
                    std::uint32_t maximumQuads = 65536u) noexcept;

    bool initialize();
    bool render(const render::FrameDescription& frame,
                const RenderPacket& packet,
                const Camera2D& camera,
                const UiDrawList* screenUi = nullptr);
    void shutdown() noexcept;

    bool initialized() const noexcept;
    render::TextureHandle whiteTexture() const noexcept;
    const Fixed2DRendererStats& stats() const noexcept;

    static render::PipelineDescription spritePipelineDescription(
        render::BlendMode blend);

private:
    static std::size_t pipelineIndex(render::BlendMode blend) noexcept;
    render::PipelineHandle pipeline(render::BlendMode blend) const noexcept;
    bool ensureBuffers(std::size_t vertexBytes,
                       std::size_t indexBytes);
    static std::size_t growCapacity(std::size_t required) noexcept;
    void clearHandles() noexcept;

    render::RenderDevice& device_;
    SpriteResolver& resolver_;
    SpriteBatchCompiler compiler_;
    render::BufferHandle vertexBuffer_{};
    render::BufferHandle indexBuffer_{};
    render::TextureHandle whiteTexture_{};
    std::array<render::PipelineHandle, 4> pipelines_{};
    std::size_t vertexCapacity_{};
    std::size_t indexCapacity_{};
    std::uint32_t deviceGeneration_{};
    bool initialized_{};
    Fixed2DRendererStats lastStats_{};
};

} // namespace rts::presentation
