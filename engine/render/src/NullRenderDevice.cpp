#include <RTSEngine/Render/NullRenderDevice.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace rts::render {

template<class Handle, class Description>
Handle NullRenderDevice::ResourcePool<Handle, Description>::create(
    const Description& description,
    std::size_t byteCount) {
    std::size_t index = 0;
    for (; index < slots_.size(); ++index) {
        if (!slots_[index].alive) break;
    }
    if (index == slots_.size()) slots_.push_back({});
    auto& slot = slots_[index];
    if (slot.generation == 0) slot.generation = 1;
    slot.alive = true;
    slot.description = description;
    slot.bytes.assign(byteCount, 0u);
    return {static_cast<std::uint32_t>(index + 1u), slot.generation};
}

template<class Handle, class Description>
bool NullRenderDevice::ResourcePool<Handle, Description>::destroy(
    Handle handle) noexcept {
    auto* slot = find(handle);
    if (!slot) return false;
    slot->alive = false;
    slot->description = {};
    slot->bytes.clear();
    slot->generation = nextGeneration(slot->generation);
    return true;
}

template<class Handle, class Description>
bool NullRenderDevice::ResourcePool<Handle, Description>::update(
    Handle handle,
    std::size_t offset,
    const void* data,
    std::size_t size) {
    auto* slot = find(handle);
    if (!slot || (size != 0 && data == nullptr) ||
        offset > slot->bytes.size() ||
        size > slot->bytes.size() - offset) {
        return false;
    }
    if (size != 0) {
        std::memcpy(slot->bytes.data() + offset, data, size);
    }
    return true;
}

template<class Handle, class Description>
bool NullRenderDevice::ResourcePool<Handle, Description>::valid(
    Handle handle) const noexcept {
    return find(handle) != nullptr;
}

template<class Handle, class Description>
const Description*
NullRenderDevice::ResourcePool<Handle, Description>::description(
    Handle handle) const noexcept {
    const auto* slot = find(handle);
    return slot ? &slot->description : nullptr;
}

template<class Handle, class Description>
const std::vector<std::uint8_t>*
NullRenderDevice::ResourcePool<Handle, Description>::bytes(
    Handle handle) const noexcept {
    const auto* slot = find(handle);
    return slot ? &slot->bytes : nullptr;
}

template<class Handle, class Description>
void NullRenderDevice::ResourcePool<Handle, Description>::invalidateAll()
    noexcept {
    for (auto& slot : slots_) {
        slot.alive = false;
        slot.description = {};
        slot.bytes.clear();
        slot.generation = nextGeneration(slot.generation);
    }
}

template<class Handle, class Description>
std::size_t NullRenderDevice::ResourcePool<Handle, Description>::liveCount()
    const noexcept {
    return static_cast<std::size_t>(std::count_if(
        slots_.begin(), slots_.end(),
        [](const Slot& slot) { return slot.alive; }));
}

template<class Handle, class Description>
std::uint32_t
NullRenderDevice::ResourcePool<Handle, Description>::nextGeneration(
    std::uint32_t value) noexcept {
    ++value;
    return value == 0 ? 1u : value;
}

template<class Handle, class Description>
typename NullRenderDevice::ResourcePool<Handle, Description>::Slot*
NullRenderDevice::ResourcePool<Handle, Description>::find(
    Handle handle) noexcept {
    if (!handle.valid() || handle.index > slots_.size()) return nullptr;
    auto& slot = slots_[handle.index - 1u];
    return slot.alive && slot.generation == handle.generation
        ? &slot : nullptr;
}

template<class Handle, class Description>
const typename NullRenderDevice::ResourcePool<Handle, Description>::Slot*
NullRenderDevice::ResourcePool<Handle, Description>::find(
    Handle handle) const noexcept {
    if (!handle.valid() || handle.index > slots_.size()) return nullptr;
    const auto& slot = slots_[handle.index - 1u];
    return slot.alive && slot.generation == handle.generation
        ? &slot : nullptr;
}

std::uint32_t NullRenderDevice::deviceGeneration() const noexcept {
    return deviceGeneration_;
}

BufferHandle NullRenderDevice::createBuffer(
    const BufferDescription& description) {
    if (description.sizeBytes == 0) return {};
    return buffers_.create(description, description.sizeBytes);
}

TextureHandle NullRenderDevice::createTexture(
    const TextureDescription& description) {
    const auto size = textureByteSize(description);
    if (size == 0) return {};
    return textures_.create(description, size);
}

PipelineHandle NullRenderDevice::createPipeline(
    const PipelineDescription& description) {
    if (!validPipeline(description)) return {};
    return pipelines_.create(description, 0);
}

bool NullRenderDevice::updateBuffer(BufferHandle handle,
                                    std::size_t offsetBytes,
                                    const void* data,
                                    std::size_t sizeBytes) {
    return buffers_.update(handle, offsetBytes, data, sizeBytes);
}

bool NullRenderDevice::updateTexture(TextureHandle handle,
                                     const void* data,
                                     std::size_t sizeBytes) {
    const auto* description = textures_.description(handle);
    if (!description || sizeBytes != textureByteSize(*description)) {
        return false;
    }
    return textures_.update(handle, 0, data, sizeBytes);
}

bool NullRenderDevice::destroyBuffer(BufferHandle handle) {
    return buffers_.destroy(handle);
}

bool NullRenderDevice::destroyTexture(TextureHandle handle) {
    return textures_.destroy(handle);
}

bool NullRenderDevice::destroyPipeline(PipelineHandle handle) {
    return pipelines_.destroy(handle);
}

bool NullRenderDevice::beginFrame(const FrameDescription& description) {
    if (frameActive_ || !description.window.valid() ||
        description.framebufferWidth == 0 ||
        description.framebufferHeight == 0) {
        return false;
    }
    frameActive_ = true;
    current_ = {};
    current_.description = description;
    return true;
}

bool NullRenderDevice::submit(const DrawCommand& command) {
    const auto* pipeline = pipelines_.description(command.pipeline);
    if (!frameActive_ || !pipeline ||
        !buffers_.valid(command.vertexBuffer) ||
        command.elementCount == 0 || command.instanceCount == 0 ||
        (command.indexBuffer.valid() &&
         !buffers_.valid(command.indexBuffer)) ||
        (command.texture.valid() &&
         !textures_.valid(command.texture)) ||
        !drawWithinBuffers(command, *pipeline)) {
        return false;
    }
    current_.draws.push_back(command);
    return true;
}

bool NullRenderDevice::endFrame() {
    if (!frameActive_) return false;
    frameActive_ = false;
    std::stable_sort(
        current_.draws.begin(), current_.draws.end(),
        [](const DrawCommand& a, const DrawCommand& b) {
            if (a.pass != b.pass) {
                return static_cast<std::uint8_t>(a.pass) <
                       static_cast<std::uint8_t>(b.pass);
            }
            return a.sortKey < b.sortKey;
        });
    lastFrame_ = std::move(current_);
    current_ = {};
    ++submittedFrames_;
    return true;
}

void NullRenderDevice::reset() noexcept {
    frameActive_ = false;
    current_ = {};
    lastFrame_ = {};
    buffers_.invalidateAll();
    textures_.invalidateAll();
    pipelines_.invalidateAll();
    ++deviceGeneration_;
    if (deviceGeneration_ == 0) deviceGeneration_ = 1;
}

bool NullRenderDevice::valid(BufferHandle handle) const noexcept {
    return buffers_.valid(handle);
}

bool NullRenderDevice::valid(TextureHandle handle) const noexcept {
    return textures_.valid(handle);
}

bool NullRenderDevice::valid(PipelineHandle handle) const noexcept {
    return pipelines_.valid(handle);
}

const std::vector<std::uint8_t>* NullRenderDevice::bufferBytes(
    BufferHandle handle) const noexcept {
    return buffers_.bytes(handle);
}

const std::vector<std::uint8_t>* NullRenderDevice::textureBytes(
    TextureHandle handle) const noexcept {
    return textures_.bytes(handle);
}

std::size_t NullRenderDevice::liveBufferCount() const noexcept {
    return buffers_.liveCount();
}

std::size_t NullRenderDevice::liveTextureCount() const noexcept {
    return textures_.liveCount();
}

std::size_t NullRenderDevice::livePipelineCount() const noexcept {
    return pipelines_.liveCount();
}

bool NullRenderDevice::frameActive() const noexcept {
    return frameActive_;
}

std::uint64_t NullRenderDevice::submittedFrames() const noexcept {
    return submittedFrames_;
}

const RecordedFrame& NullRenderDevice::lastFrame() const noexcept {
    return lastFrame_;
}

std::size_t NullRenderDevice::textureByteSize(
    const TextureDescription& description) noexcept {
    const auto bytesPerPixel = TextureBytesPerPixel(description.format);
    if (description.width == 0 || description.height == 0 ||
        bytesPerPixel == 0) {
        return 0;
    }
    const auto pixels = static_cast<std::uint64_t>(description.width) *
                        static_cast<std::uint64_t>(description.height);
    const auto bytes = pixels * bytesPerPixel;
    return bytes <= std::numeric_limits<std::size_t>::max()
        ? static_cast<std::size_t>(bytes) : 0u;
}

bool NullRenderDevice::validPipeline(
    const PipelineDescription& description) noexcept {
    if (description.vertexStride == 0 && !description.attributes.empty()) {
        return false;
    }
    for (const auto& attribute : description.attributes) {
        const auto bytes = VertexFormatComponents(attribute.format) *
                           sizeof(float);
        if (bytes == 0 ||
            attribute.offsetBytes > description.vertexStride ||
            bytes > description.vertexStride - attribute.offsetBytes) {
            return false;
        }
    }
    return true;
}

bool NullRenderDevice::drawWithinBuffers(
    const DrawCommand& command,
    const PipelineDescription& pipeline) const noexcept {
    const auto* vertices = buffers_.bytes(command.vertexBuffer);
    if (!vertices || command.vertexOffsetBytes > vertices->size()) {
        return false;
    }

    if (command.indexBuffer.valid()) {
        const auto* indices = buffers_.bytes(command.indexBuffer);
        if (!indices || pipeline.indexType == IndexType::None ||
            command.indexOffsetBytes > indices->size()) {
            return false;
        }
        const auto indexSize = pipeline.indexType == IndexType::UInt16
            ? 2u : 4u;
        const auto required = static_cast<std::uint64_t>(
            command.firstElement + command.elementCount) * indexSize;
        return required <= indices->size() - command.indexOffsetBytes;
    }

    if (pipeline.vertexStride == 0) return true;
    const auto required = static_cast<std::uint64_t>(
        command.firstElement + command.elementCount) *
        pipeline.vertexStride;
    return required <= vertices->size() - command.vertexOffsetBytes;
}

} // namespace rts::render
