#pragma once

#include <RTSEngine/Platform/Platform.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts::render {

template<class Tag>
struct ResourceHandle final {
    std::uint32_t index{};
    std::uint32_t generation{};

    constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }

    friend constexpr bool operator==(ResourceHandle a,
                                     ResourceHandle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }

    friend constexpr bool operator!=(ResourceHandle a,
                                     ResourceHandle b) noexcept {
        return !(a == b);
    }
};

struct BufferTag;
struct TextureTag;
struct PipelineTag;

using BufferHandle = ResourceHandle<BufferTag>;
using TextureHandle = ResourceHandle<TextureTag>;
using PipelineHandle = ResourceHandle<PipelineTag>;

enum class BufferUsage : std::uint8_t {
    Vertex,
    Index,
    Uniform,
    DynamicVertex
};

enum class TextureFormat : std::uint8_t {
    R8,
    Rgba8,
    Depth24Stencil8
};

enum class PrimitiveTopology : std::uint8_t {
    Triangles,
    Lines
};

enum class BlendMode : std::uint8_t {
    Opaque,
    Alpha,
    Additive,
    Multiply
};

struct BufferDescription final {
    std::size_t sizeBytes{};
    BufferUsage usage{BufferUsage::Vertex};
};

struct TextureDescription final {
    std::uint32_t width{};
    std::uint32_t height{};
    TextureFormat format{TextureFormat::Rgba8};
};

struct PipelineDescription final {
    PrimitiveTopology topology{PrimitiveTopology::Triangles};
    BlendMode blend{BlendMode::Alpha};
    bool depthTest{};
    bool depthWrite{};
};

struct DrawCommand final {
    PipelineHandle pipeline{};
    BufferHandle vertexBuffer{};
    BufferHandle indexBuffer{};
    TextureHandle texture{};
    std::uint32_t firstElement{};
    std::uint32_t elementCount{};
    std::uint32_t instanceCount{1};
    std::uint64_t sortKey{};
};

struct FrameDescription final {
    platform::WindowHandle window{};
    std::uint32_t framebufferWidth{};
    std::uint32_t framebufferHeight{};
};

class RenderDevice {
public:
    virtual ~RenderDevice() = default;

    virtual std::uint32_t deviceGeneration() const noexcept = 0;

    virtual BufferHandle createBuffer(const BufferDescription& description) = 0;
    virtual TextureHandle createTexture(const TextureDescription& description) = 0;
    virtual PipelineHandle createPipeline(
        const PipelineDescription& description) = 0;

    virtual bool destroyBuffer(BufferHandle handle) = 0;
    virtual bool destroyTexture(TextureHandle handle) = 0;
    virtual bool destroyPipeline(PipelineHandle handle) = 0;

    virtual bool beginFrame(const FrameDescription& description) = 0;
    virtual bool submit(const DrawCommand& command) = 0;
    virtual bool endFrame() = 0;

    // Invalidates every GPU handle and begins a new device generation.
    virtual void reset() noexcept = 0;
};

} // namespace rts::render
