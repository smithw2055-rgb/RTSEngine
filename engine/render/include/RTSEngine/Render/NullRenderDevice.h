#pragma once

#include <RTSEngine/Render/RenderDevice.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts::render {

struct RecordedFrame final {
    FrameDescription description{};
    std::vector<DrawCommand> draws;
};

class NullRenderDevice final : public RenderDevice {
public:
    std::uint32_t deviceGeneration() const noexcept override;

    BufferHandle createBuffer(
        const BufferDescription& description) override;
    TextureHandle createTexture(
        const TextureDescription& description) override;
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

    bool valid(BufferHandle handle) const noexcept;
    bool valid(TextureHandle handle) const noexcept;
    bool valid(PipelineHandle handle) const noexcept;

    const std::vector<std::uint8_t>* bufferBytes(
        BufferHandle handle) const noexcept;
    const std::vector<std::uint8_t>* textureBytes(
        TextureHandle handle) const noexcept;

    std::size_t liveBufferCount() const noexcept;
    std::size_t liveTextureCount() const noexcept;
    std::size_t livePipelineCount() const noexcept;

    bool frameActive() const noexcept;
    std::uint64_t submittedFrames() const noexcept;
    const RecordedFrame& lastFrame() const noexcept;

private:
    template<class Handle, class Description>
    class ResourcePool final {
    public:
        Handle create(const Description& description,
                      std::size_t byteCount);
        bool destroy(Handle handle) noexcept;
        bool update(Handle handle,
                    std::size_t offset,
                    const void* data,
                    std::size_t size);
        bool valid(Handle handle) const noexcept;
        const Description* description(Handle handle) const noexcept;
        const std::vector<std::uint8_t>* bytes(Handle handle) const noexcept;
        void invalidateAll() noexcept;
        std::size_t liveCount() const noexcept;

    private:
        struct Slot final {
            std::uint32_t generation{1};
            bool alive{};
            Description description{};
            std::vector<std::uint8_t> bytes;
        };

        static std::uint32_t nextGeneration(std::uint32_t value) noexcept;
        Slot* find(Handle handle) noexcept;
        const Slot* find(Handle handle) const noexcept;

        std::vector<Slot> slots_;
    };

    static std::size_t textureByteSize(
        const TextureDescription& description) noexcept;
    static bool validPipeline(
        const PipelineDescription& description) noexcept;
    bool drawWithinBuffers(const DrawCommand& command,
                           const PipelineDescription& pipeline) const noexcept;

    ResourcePool<BufferHandle, BufferDescription> buffers_;
    ResourcePool<TextureHandle, TextureDescription> textures_;
    ResourcePool<PipelineHandle, PipelineDescription> pipelines_;
    RecordedFrame current_;
    RecordedFrame lastFrame_;
    std::uint32_t deviceGeneration_{1};
    std::uint64_t submittedFrames_{};
    bool frameActive_{};
};

} // namespace rts::render
