#include <RTSEngine/Presentation/Fixed2DRenderer.h>

#include <algorithm>
#include <cstddef>
#include <limits>

namespace rts::presentation {

Fixed2DRenderer::Fixed2DRenderer(
    render::RenderDevice& device,
    SpriteResolver& resolver,
    std::uint32_t maximumQuads) noexcept
    : device_(device), resolver_(resolver), compiler_(maximumQuads) {}

bool Fixed2DRenderer::initialize() {
    const auto generation = device_.deviceGeneration();
    if (initialized_ && generation == deviceGeneration_) return true;
    clearHandles();
    initialized_ = false;
    deviceGeneration_ = generation;

    whiteTexture_ = device_.createTexture(
        {1u, 1u, render::TextureFormat::Rgba8,
         false, render::FilterMode::Nearest,
         render::AddressMode::Clamp,
         render::AddressMode::Clamp});
    const std::array<std::uint8_t, 4> white{{255u, 255u, 255u, 255u}};
    if (!whiteTexture_.valid() ||
        !device_.updateTexture(
            whiteTexture_, white.data(), white.size())) {
        clearHandles();
        return false;
    }

    for (std::size_t index = 0; index < pipelines_.size(); ++index) {
        auto description = spritePipelineDescription(
            static_cast<render::BlendMode>(index));
        pipelines_[index] = device_.createPipeline(description);
        if (!pipelines_[index].valid()) {
            shutdown();
            return false;
        }
    }
    initialized_ = true;
    return true;
}

bool Fixed2DRenderer::render(
    const render::FrameDescription& frame,
    const RenderPacket& packet,
    const Camera2D& camera,
    const UiDrawList* screenUi) {
    if (device_.deviceGeneration() != deviceGeneration_) {
        initialized_ = false;
        clearHandles();
    }
    if (!initialize()) return false;

    const auto compiled = compiler_.compile(
        packet, resolver_, whiteTexture_, camera, screenUi);
    const auto vertexBytes = compiled.vertices.size() * sizeof(SpriteVertex);
    const auto indexBytes = compiled.indices.size() * sizeof(std::uint32_t);
    if (!ensureBuffers(vertexBytes, indexBytes)) return false;
    if (vertexBytes != 0 &&
        !device_.updateBuffer(
            vertexBuffer_, 0, compiled.vertices.data(), vertexBytes)) {
        return false;
    }
    if (indexBytes != 0 &&
        !device_.updateBuffer(
            indexBuffer_, 0, compiled.indices.data(), indexBytes)) {
        return false;
    }

    if (!device_.beginFrame(frame)) return false;
    bool accepted = true;
    std::uint32_t drawCalls = 0;
    for (const auto& batch : compiled.batches) {
        render::DrawCommand command;
        command.pipeline = pipeline(batch.blend);
        command.vertexBuffer = vertexBuffer_;
        command.indexBuffer = indexBuffer_;
        command.texture = batch.texture;
        command.firstElement = batch.firstIndex;
        command.elementCount = batch.indexCount;
        command.instanceCount = 1u;
        command.sortKey = batch.sortKey;
        command.pass = batch.pass;
        if (!device_.submit(command)) {
            accepted = false;
            break;
        }
        ++drawCalls;
    }
    if (!device_.endFrame()) return false;

    lastStats_.submittedFrames += 1u;
    lastStats_.drawCalls = drawCalls;
    lastStats_.spriteQuads = compiled.spriteQuads;
    lastStats_.worldUiQuads = compiled.worldUiQuads;
    lastStats_.worldOverlayQuads = compiled.worldOverlayQuads;
    lastStats_.screenUiQuads = compiled.screenUiQuads;
    lastStats_.unresolvedSprites = compiled.unresolvedSprites;
    lastStats_.droppedQuads = compiled.droppedQuads;
    lastStats_.vertexBytes = vertexBytes;
    lastStats_.indexBytes = indexBytes;
    return accepted;
}

void Fixed2DRenderer::shutdown() noexcept {
    if (device_.deviceGeneration() == deviceGeneration_) {
        if (vertexBuffer_.valid()) {
            (void)device_.destroyBuffer(vertexBuffer_);
        }
        if (indexBuffer_.valid()) {
            (void)device_.destroyBuffer(indexBuffer_);
        }
        if (whiteTexture_.valid()) {
            (void)device_.destroyTexture(whiteTexture_);
        }
        for (const auto handle : pipelines_) {
            if (handle.valid()) (void)device_.destroyPipeline(handle);
        }
    }
    clearHandles();
    initialized_ = false;
}

bool Fixed2DRenderer::initialized() const noexcept {
    return initialized_;
}

render::TextureHandle Fixed2DRenderer::whiteTexture() const noexcept {
    return whiteTexture_;
}

const Fixed2DRendererStats& Fixed2DRenderer::stats() const noexcept {
    return lastStats_;
}

render::PipelineDescription Fixed2DRenderer::spritePipelineDescription(
    render::BlendMode blend) {
    render::PipelineDescription description;
    description.topology = render::PrimitiveTopology::Triangles;
    description.blend = blend;
    description.depthTest = false;
    description.depthWrite = false;
    description.shaderKey = kSpriteShaderKey;
    description.vertexStride = sizeof(SpriteVertex);
    description.attributes = {
        {static_cast<std::uint32_t>(offsetof(SpriteVertex, x)),
         render::VertexFormat::Float2, 0u},
        {static_cast<std::uint32_t>(offsetof(SpriteVertex, u)),
         render::VertexFormat::Float2, 0u},
        {static_cast<std::uint32_t>(offsetof(SpriteVertex, red)),
         render::VertexFormat::Float4, 0u}
    };
    description.indexType = render::IndexType::UInt32;
    description.colorFormat = render::TextureFormat::Rgba8;
    return description;
}

std::size_t Fixed2DRenderer::pipelineIndex(
    render::BlendMode blend) noexcept {
    return static_cast<std::size_t>(blend);
}

render::PipelineHandle Fixed2DRenderer::pipeline(
    render::BlendMode blend) const noexcept {
    const auto index = pipelineIndex(blend);
    return index < pipelines_.size() ? pipelines_[index]
                                     : render::PipelineHandle{};
}

bool Fixed2DRenderer::ensureBuffers(
    std::size_t vertexBytes,
    std::size_t indexBytes) {
    const auto requiredVertex = std::max<std::size_t>(vertexBytes, 1u);
    const auto requiredIndex = std::max<std::size_t>(indexBytes, 1u);
    if (!vertexBuffer_.valid() || vertexCapacity_ < requiredVertex) {
        if (vertexBuffer_.valid()) {
            (void)device_.destroyBuffer(vertexBuffer_);
        }
        vertexCapacity_ = growCapacity(requiredVertex);
        vertexBuffer_ = device_.createBuffer(
            {vertexCapacity_, render::BufferUsage::DynamicVertex, true});
        if (!vertexBuffer_.valid()) return false;
    }
    if (!indexBuffer_.valid() || indexCapacity_ < requiredIndex) {
        if (indexBuffer_.valid()) {
            (void)device_.destroyBuffer(indexBuffer_);
        }
        indexCapacity_ = growCapacity(requiredIndex);
        indexBuffer_ = device_.createBuffer(
            {indexCapacity_, render::BufferUsage::Index, true});
        if (!indexBuffer_.valid()) return false;
    }
    return true;
}

std::size_t Fixed2DRenderer::growCapacity(
    std::size_t required) noexcept {
    std::size_t capacity = 4096u;
    while (capacity < required &&
           capacity <= std::numeric_limits<std::size_t>::max() / 2u) {
        capacity *= 2u;
    }
    return std::max(capacity, required);
}

void Fixed2DRenderer::clearHandles() noexcept {
    vertexBuffer_ = {};
    indexBuffer_ = {};
    whiteTexture_ = {};
    pipelines_.fill({});
    vertexCapacity_ = 0;
    indexCapacity_ = 0;
}

} // namespace rts::presentation
