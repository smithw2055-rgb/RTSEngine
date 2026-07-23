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

using ShaderKey = std::uint64_t;

enum class RenderPassKind : std::uint8_t {
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

enum class IndexType : std::uint8_t {
    None,
    UInt16,
    UInt32
};

enum class VertexFormat : std::uint8_t {
    Float2,
    Float4
};

enum class FilterMode : std::uint8_t {
    Nearest,
    Linear
};

enum class AddressMode : std::uint8_t {
    Clamp,
    Repeat
};

struct BufferDescription final {
    std::size_t sizeBytes{};
    BufferUsage usage{BufferUsage::Vertex};
    bool dynamicUpdate{};
};

struct TextureDescription final {
    std::uint32_t width{};
    std::uint32_t height{};
    TextureFormat format{TextureFormat::Rgba8};
    bool dynamicUpdate{};
    FilterMode filter{FilterMode::Nearest};
    AddressMode addressU{AddressMode::Clamp};
    AddressMode addressV{AddressMode::Clamp};
};

struct VertexAttributeDescription final {
    std::uint32_t offsetBytes{};
    VertexFormat format{VertexFormat::Float2};
    std::uint32_t bufferSlot{};
};

struct PipelineDescription final {
    PrimitiveTopology topology{PrimitiveTopology::Triangles};
    BlendMode blend{BlendMode::Alpha};
    bool depthTest{};
    bool depthWrite{};
    ShaderKey shaderKey{};
    std::uint32_t vertexStride{};
    std::vector<VertexAttributeDescription> attributes;
    IndexType indexType{IndexType::UInt16};
    TextureFormat colorFormat{TextureFormat::Rgba8};
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
    RenderPassKind pass{RenderPassKind::WorldEntity};
    std::uint32_t vertexOffsetBytes{};
    std::uint32_t indexOffsetBytes{};
};

struct FrameDescription final {
    platform::WindowHandle window{};
    std::uint32_t framebufferWidth{};
    std::uint32_t framebufferHeight{};
    float clearRed{};
    float clearGreen{};
    float clearBlue{};
    float clearAlpha{1.0f};
};

class RenderDevice {
public:
    virtual ~RenderDevice() = default;

    virtual std::uint32_t deviceGeneration() const noexcept = 0;

    virtual BufferHandle createBuffer(const BufferDescription& description) = 0;
    virtual TextureHandle createTexture(const TextureDescription& description) = 0;
    virtual PipelineHandle createPipeline(
        const PipelineDescription& description) = 0;

    virtual bool updateBuffer(BufferHandle handle,
                              std::size_t offsetBytes,
                              const void* data,
                              std::size_t sizeBytes) = 0;
    virtual bool updateTexture(TextureHandle handle,
                               const void* data,
                               std::size_t sizeBytes) = 0;

    virtual bool destroyBuffer(BufferHandle handle) = 0;
    virtual bool destroyTexture(TextureHandle handle) = 0;
    virtual bool destroyPipeline(PipelineHandle handle) = 0;

    virtual bool beginFrame(const FrameDescription& description) = 0;
    virtual bool submit(const DrawCommand& command) = 0;
    virtual bool endFrame() = 0;

    // Invalidates every GPU handle and begins a new device generation.
    virtual void reset() noexcept = 0;
};

constexpr std::size_t TextureBytesPerPixel(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::R8: return 1u;
    case TextureFormat::Rgba8: return 4u;
    case TextureFormat::Depth24Stencil8: return 4u;
    }
    return 0u;
}

constexpr std::uint32_t VertexFormatComponents(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float2: return 2u;
    case VertexFormat::Float4: return 4u;
    }
    return 0u;
}

} // namespace rts::render
