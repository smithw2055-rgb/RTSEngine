#include <RTSEngine/Render/SokolRenderDevice.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::render {
namespace {

sg_pixel_format toSokol(TextureFormat value) noexcept {
    switch (value) {
    case TextureFormat::R8: return SG_PIXELFORMAT_R8;
    case TextureFormat::Rgba8: return SG_PIXELFORMAT_RGBA8;
    case TextureFormat::Depth24Stencil8:
        return SG_PIXELFORMAT_DEPTH_STENCIL;
    }
    return SG_PIXELFORMAT_RGBA8;
}

sg_vertex_format toSokol(VertexFormat value) noexcept {
    switch (value) {
    case VertexFormat::Float2: return SG_VERTEXFORMAT_FLOAT2;
    case VertexFormat::Float4: return SG_VERTEXFORMAT_FLOAT4;
    }
    return SG_VERTEXFORMAT_INVALID;
}

sg_primitive_type toSokol(PrimitiveTopology value) noexcept {
    return value == PrimitiveTopology::Lines
        ? SG_PRIMITIVETYPE_LINES : SG_PRIMITIVETYPE_TRIANGLES;
}

sg_index_type toSokol(IndexType value) noexcept {
    switch (value) {
    case IndexType::None: return SG_INDEXTYPE_NONE;
    case IndexType::UInt16: return SG_INDEXTYPE_UINT16;
    case IndexType::UInt32: return SG_INDEXTYPE_UINT32;
    }
    return SG_INDEXTYPE_NONE;
}

sg_filter toSokol(FilterMode value) noexcept {
    return value == FilterMode::Linear ? SG_FILTER_LINEAR : SG_FILTER_NEAREST;
}

sg_wrap toSokol(AddressMode value) noexcept {
    return value == AddressMode::Repeat
        ? SG_WRAP_REPEAT : SG_WRAP_CLAMP_TO_EDGE;
}

void applyBlend(BlendMode value, sg_blend_state& blend) noexcept {
    blend = {};
    if (value == BlendMode::Opaque) return;
    blend.enabled = true;
    blend.op_rgb = SG_BLENDOP_ADD;
    blend.op_alpha = SG_BLENDOP_ADD;
    switch (value) {
    case BlendMode::Opaque:
        break;
    case BlendMode::Alpha:
        blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
        blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
        blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
        blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
        break;
    case BlendMode::Multiply:
        blend.src_factor_rgb = SG_BLENDFACTOR_DST_COLOR;
        blend.dst_factor_rgb = SG_BLENDFACTOR_ZERO;
        blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
        blend.dst_factor_alpha = SG_BLENDFACTOR_ZERO;
        break;
    }
}

template<class Slot>
std::uint32_t allocateSlot(std::vector<Slot>& slots) {
    std::size_t index = 0;
    for (; index < slots.size(); ++index) {
        if (!slots[index].alive) break;
    }
    if (index == slots.size()) slots.push_back({});
    auto& slot = slots[index];
    if (slot.generation == 0) slot.generation = 1;
    slot.alive = true;
    return static_cast<std::uint32_t>(index + 1u);
}

std::uint32_t nextGeneration(std::uint32_t value) noexcept {
    ++value;
    return value == 0 ? 1u : value;
}

} // namespace

struct SokolRenderDevice::Impl final {
    struct BufferSlot final {
        std::uint32_t generation{1};
        bool alive{};
        BufferDescription description{};
        sg_buffer resource{};
    };

    struct TextureSlot final {
        std::uint32_t generation{1};
        bool alive{};
        TextureDescription description{};
        sg_image image{};
        sg_view view{};
        sg_sampler sampler{};
    };

    struct PipelineSlot final {
        std::uint32_t generation{1};
        bool alive{};
        PipelineDescription description{};
        sg_shader shader{};
        sg_pipeline pipeline{};
    };

    explicit Impl(SokolBackendCallbacks value)
        : callbacks(value) {
        if (!callbacks.swapchain || !callbacks.shaderDescription) return;
        if (callbacks.manageSokolLifecycle && !sg_isvalid()) {
            if (!callbacks.environment) return;
            sg_desc description{};
            description.environment = callbacks.environment(callbacks.userData);
            sg_setup(&description);
            ownsSokol = sg_isvalid();
        }
        ready = sg_isvalid();
    }

    ~Impl() {
        destroyAll();
        if (ownsSokol && sg_isvalid()) sg_shutdown();
    }

    BufferSlot* buffer(BufferHandle handle) noexcept {
        if (!handle.valid() || handle.index > buffers.size()) return nullptr;
        auto& slot = buffers[handle.index - 1u];
        return slot.alive && slot.generation == handle.generation
            ? &slot : nullptr;
    }

    TextureSlot* texture(TextureHandle handle) noexcept {
        if (!handle.valid() || handle.index > textures.size()) return nullptr;
        auto& slot = textures[handle.index - 1u];
        return slot.alive && slot.generation == handle.generation
            ? &slot : nullptr;
    }

    PipelineSlot* pipeline(PipelineHandle handle) noexcept {
        if (!handle.valid() || handle.index > pipelines.size()) return nullptr;
        auto& slot = pipelines[handle.index - 1u];
        return slot.alive && slot.generation == handle.generation
            ? &slot : nullptr;
    }

    void destroy(BufferSlot& slot) noexcept {
        if (slot.alive && sg_query_buffer_state(slot.resource) ==
                              SG_RESOURCESTATE_VALID) {
            sg_destroy_buffer(slot.resource);
        }
        slot.resource = {};
        slot.description = {};
        slot.alive = false;
        slot.generation = nextGeneration(slot.generation);
    }

    void destroy(TextureSlot& slot) noexcept {
        if (slot.alive) {
            if (sg_query_view_state(slot.view) == SG_RESOURCESTATE_VALID) {
                sg_destroy_view(slot.view);
            }
            if (sg_query_sampler_state(slot.sampler) == SG_RESOURCESTATE_VALID) {
                sg_destroy_sampler(slot.sampler);
            }
            if (sg_query_image_state(slot.image) == SG_RESOURCESTATE_VALID) {
                sg_destroy_image(slot.image);
            }
        }
        slot.image = {};
        slot.view = {};
        slot.sampler = {};
        slot.description = {};
        slot.alive = false;
        slot.generation = nextGeneration(slot.generation);
    }

    void destroy(PipelineSlot& slot) noexcept {
        if (slot.alive) {
            if (sg_query_pipeline_state(slot.pipeline) ==
                    SG_RESOURCESTATE_VALID) {
                sg_destroy_pipeline(slot.pipeline);
            }
            if (sg_query_shader_state(slot.shader) == SG_RESOURCESTATE_VALID) {
                sg_destroy_shader(slot.shader);
            }
        }
        slot.pipeline = {};
        slot.shader = {};
        slot.description = {};
        slot.alive = false;
        slot.generation = nextGeneration(slot.generation);
    }

    void destroyAll() noexcept {
        if (!sg_isvalid()) {
            buffers.clear();
            textures.clear();
            pipelines.clear();
            return;
        }
        if (frameActive) {
            sg_end_pass();
            sg_commit();
            frameActive = false;
        }
        for (auto& slot : pipelines) destroy(slot);
        for (auto& slot : textures) destroy(slot);
        for (auto& slot : buffers) destroy(slot);
    }

    SokolBackendCallbacks callbacks{};
    std::vector<BufferSlot> buffers;
    std::vector<TextureSlot> textures;
    std::vector<PipelineSlot> pipelines;
    std::uint32_t generation{1};
    RenderPassKind lastPass{RenderPassKind::Terrain};
    std::uint64_t lastSortKey{};
    bool hasDraw{};
    bool frameActive{};
    bool ownsSokol{};
    bool ready{};
};

SokolRenderDevice::SokolRenderDevice(SokolBackendCallbacks callbacks)
    : impl_(std::make_unique<Impl>(callbacks)) {}

SokolRenderDevice::~SokolRenderDevice() = default;

bool SokolRenderDevice::valid() const noexcept {
    return impl_ && impl_->ready && sg_isvalid();
}

std::uint32_t SokolRenderDevice::deviceGeneration() const noexcept {
    return impl_ ? impl_->generation : 0u;
}

BufferHandle SokolRenderDevice::createBuffer(
    const BufferDescription& description) {
    if (!valid() || description.sizeBytes == 0) return {};
    const auto index = allocateSlot(impl_->buffers);
    auto& slot = impl_->buffers[index - 1u];
    slot.description = description;

    sg_buffer_desc sokol{};
    sokol.size = description.sizeBytes;
    sokol.usage.immutable = false;
    sokol.usage.dynamic_update = !description.dynamicUpdate;
    sokol.usage.stream_update = description.dynamicUpdate;
    sokol.usage.vertex_buffer = description.usage != BufferUsage::Index;
    sokol.usage.index_buffer = description.usage == BufferUsage::Index;
    slot.resource = sg_make_buffer(&sokol);
    if (sg_query_buffer_state(slot.resource) != SG_RESOURCESTATE_VALID) {
        impl_->destroy(slot);
        return {};
    }
    return {index, slot.generation};
}

TextureHandle SokolRenderDevice::createTexture(
    const TextureDescription& description) {
    if (!valid() || description.width == 0 || description.height == 0 ||
        description.format == TextureFormat::Depth24Stencil8) {
        return {};
    }
    const auto index = allocateSlot(impl_->textures);
    auto& slot = impl_->textures[index - 1u];
    slot.description = description;

    sg_image_desc image{};
    image.width = static_cast<int>(description.width);
    image.height = static_cast<int>(description.height);
    image.pixel_format = toSokol(description.format);
    image.usage.immutable = false;
    image.usage.dynamic_update = !description.dynamicUpdate;
    image.usage.stream_update = description.dynamicUpdate;
    slot.image = sg_make_image(&image);
    if (sg_query_image_state(slot.image) != SG_RESOURCESTATE_VALID) {
        impl_->destroy(slot);
        return {};
    }

    sg_view_desc view{};
    view.texture.image = slot.image;
    slot.view = sg_make_view(&view);
    sg_sampler_desc sampler{};
    sampler.min_filter = toSokol(description.filter);
    sampler.mag_filter = toSokol(description.filter);
    sampler.mipmap_filter = toSokol(description.filter);
    sampler.wrap_u = toSokol(description.addressU);
    sampler.wrap_v = toSokol(description.addressV);
    slot.sampler = sg_make_sampler(&sampler);
    if (sg_query_view_state(slot.view) != SG_RESOURCESTATE_VALID ||
        sg_query_sampler_state(slot.sampler) != SG_RESOURCESTATE_VALID) {
        impl_->destroy(slot);
        return {};
    }
    return {index, slot.generation};
}

PipelineHandle SokolRenderDevice::createPipeline(
    const PipelineDescription& description) {
    if (!valid() || description.shaderKey == 0 ||
        description.attributes.size() > SG_MAX_VERTEX_ATTRIBUTES ||
        description.vertexStride >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const auto index = allocateSlot(impl_->pipelines);
    auto& slot = impl_->pipelines[index - 1u];
    slot.description = description;

    const auto shaderDescription = impl_->callbacks.shaderDescription(
        impl_->callbacks.userData, description.shaderKey);
    slot.shader = sg_make_shader(&shaderDescription);
    if (sg_query_shader_state(slot.shader) != SG_RESOURCESTATE_VALID) {
        impl_->destroy(slot);
        return {};
    }

    sg_pipeline_desc pipeline{};
    pipeline.shader = slot.shader;
    pipeline.primitive_type = toSokol(description.topology);
    pipeline.index_type = toSokol(description.indexType);
    pipeline.layout.buffers[0].stride =
        static_cast<int>(description.vertexStride);
    for (std::size_t attribute = 0;
         attribute < description.attributes.size(); ++attribute) {
        pipeline.layout.attrs[attribute].buffer_index =
            static_cast<int>(description.attributes[attribute].bufferSlot);
        pipeline.layout.attrs[attribute].offset =
            static_cast<int>(description.attributes[attribute].offsetBytes);
        pipeline.layout.attrs[attribute].format =
            toSokol(description.attributes[attribute].format);
    }
    pipeline.depth.compare = description.depthTest
        ? SG_COMPAREFUNC_LESS_EQUAL : SG_COMPAREFUNC_ALWAYS;
    pipeline.depth.write_enabled = description.depthWrite;
    pipeline.colors[0].pixel_format = toSokol(description.colorFormat);
    applyBlend(description.blend, pipeline.colors[0].blend);
    slot.pipeline = sg_make_pipeline(&pipeline);
    if (sg_query_pipeline_state(slot.pipeline) != SG_RESOURCESTATE_VALID) {
        impl_->destroy(slot);
        return {};
    }
    return {index, slot.generation};
}

bool SokolRenderDevice::updateBuffer(BufferHandle handle,
                                     std::size_t offsetBytes,
                                     const void* data,
                                     std::size_t sizeBytes) {
    auto* slot = impl_->buffer(handle);
    if (!valid() || !slot || offsetBytes != 0 || data == nullptr ||
        sizeBytes == 0 || sizeBytes > slot->description.sizeBytes) {
        return false;
    }
    const sg_range range{data, sizeBytes};
    sg_update_buffer(slot->resource, &range);
    return true;
}

bool SokolRenderDevice::updateTexture(TextureHandle handle,
                                      const void* data,
                                      std::size_t sizeBytes) {
    auto* slot = impl_->texture(handle);
    if (!valid() || !slot || data == nullptr || sizeBytes == 0) return false;
    const auto expected = static_cast<std::uint64_t>(slot->description.width) *
                          slot->description.height *
                          TextureBytesPerPixel(slot->description.format);
    if (expected != sizeBytes) return false;
    sg_image_data image{};
    image.mip_levels[0] = {data, sizeBytes};
    sg_update_image(slot->image, &image);
    return true;
}

bool SokolRenderDevice::destroyBuffer(BufferHandle handle) {
    auto* slot = impl_->buffer(handle);
    if (!valid() || !slot) return false;
    impl_->destroy(*slot);
    return true;
}

bool SokolRenderDevice::destroyTexture(TextureHandle handle) {
    auto* slot = impl_->texture(handle);
    if (!valid() || !slot) return false;
    impl_->destroy(*slot);
    return true;
}

bool SokolRenderDevice::destroyPipeline(PipelineHandle handle) {
    auto* slot = impl_->pipeline(handle);
    if (!valid() || !slot) return false;
    impl_->destroy(*slot);
    return true;
}

bool SokolRenderDevice::beginFrame(const FrameDescription& description) {
    if (!valid() || impl_->frameActive || !description.window.valid() ||
        description.framebufferWidth == 0 ||
        description.framebufferHeight == 0) {
        return false;
    }
    auto swapchain = impl_->callbacks.swapchain(
        impl_->callbacks.userData, description.window);
    if (swapchain.invalid) return false;

    sg_pass pass{};
    pass.swapchain = swapchain;
    pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    pass.action.colors[0].store_action = SG_STOREACTION_STORE;
    pass.action.colors[0].clear_value = {
        description.clearRed, description.clearGreen,
        description.clearBlue, description.clearAlpha};
    sg_begin_pass(&pass);
    impl_->frameActive = true;
    impl_->hasDraw = false;
    impl_->lastPass = RenderPassKind::Terrain;
    impl_->lastSortKey = 0;
    return true;
}

bool SokolRenderDevice::submit(const DrawCommand& command) {
    auto* pipeline = impl_->pipeline(command.pipeline);
    auto* vertices = impl_->buffer(command.vertexBuffer);
    auto* indices = command.indexBuffer.valid()
        ? impl_->buffer(command.indexBuffer) : nullptr;
    auto* texture = command.texture.valid()
        ? impl_->texture(command.texture) : nullptr;
    if (!valid() || !impl_->frameActive || !pipeline || !vertices ||
        (command.indexBuffer.valid() && !indices) ||
        (command.texture.valid() && !texture) ||
        command.elementCount == 0 || command.instanceCount == 0 ||
        command.elementCount >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        command.instanceCount >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (impl_->hasDraw &&
        (static_cast<std::uint8_t>(command.pass) <
             static_cast<std::uint8_t>(impl_->lastPass) ||
         (command.pass == impl_->lastPass &&
          command.sortKey < impl_->lastSortKey))) {
        return false;
    }

    sg_bindings bindings{};
    bindings.vertex_buffers[0] = vertices->resource;
    bindings.vertex_buffer_offsets[0] =
        static_cast<int>(command.vertexOffsetBytes);
    if (indices) {
        bindings.index_buffer = indices->resource;
        bindings.index_buffer_offset =
            static_cast<int>(command.indexOffsetBytes);
    }
    if (texture) {
        bindings.views[0] = texture->view;
        bindings.samplers[0] = texture->sampler;
    }
    sg_apply_pipeline(pipeline->pipeline);
    sg_apply_bindings(&bindings);
    sg_draw(static_cast<int>(command.firstElement),
            static_cast<int>(command.elementCount),
            static_cast<int>(command.instanceCount));
    impl_->hasDraw = true;
    impl_->lastPass = command.pass;
    impl_->lastSortKey = command.sortKey;
    return true;
}

bool SokolRenderDevice::endFrame() {
    if (!valid() || !impl_->frameActive) return false;
    sg_end_pass();
    sg_commit();
    impl_->frameActive = false;
    return true;
}

void SokolRenderDevice::reset() noexcept {
    if (!impl_) return;
    impl_->destroyAll();
    ++impl_->generation;
    if (impl_->generation == 0) impl_->generation = 1;
    impl_->ready = sg_isvalid();
}

} // namespace rts::render
