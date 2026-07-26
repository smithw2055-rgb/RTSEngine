#pragma once

#include <RTSEngine/Render/RenderDevice.h>

#include <memory>

#include <sokol_gfx.h>

namespace rts::render {

struct SokolBackendCallbacks final {
    void* userData{};
    sg_environment (*environment)(void* userData){};
    sg_swapchain (*swapchain)(void* userData,
                              platform::WindowHandle window){};
    sg_shader_desc (*shaderDescription)(void* userData,
                                        ShaderKey shaderKey){};
    bool manageSokolLifecycle{};
};

class SokolRenderDevice final : public RenderDevice {
public:
    explicit SokolRenderDevice(SokolBackendCallbacks callbacks);
    ~SokolRenderDevice() override;

    SokolRenderDevice(const SokolRenderDevice&) = delete;
    SokolRenderDevice& operator=(const SokolRenderDevice&) = delete;

    bool valid() const noexcept;
    std::uint32_t deviceGeneration() const noexcept override;

    BufferHandle createBuffer(const BufferDescription& description) override;
    TextureHandle createTexture(const TextureDescription& description) override;
    PipelineHandle createPipeline(
        const PipelineDescription& description) override;

    bool updateBuffer(BufferHandle handle,
                      std::size_t offsetBytes,
                      const void* data,
                      std::size_t sizeBytes) override;
    bool updateTexture(TextureHandle handle,
                       const void* data,
                       std::size_t sizeBytes) override;

    bool destroyBuffer(BufferHandle handle) override;
    bool destroyTexture(TextureHandle handle) override;
    bool destroyPipeline(PipelineHandle handle) override;

    bool beginFrame(const FrameDescription& description) override;
    bool submit(const DrawCommand& command) override;
    bool endFrame() override;

    void reset() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rts::render
