#pragma once

#include <RTSEngine/Render/RenderDevice.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rts::render {

struct RecordedFrame final {
    FrameDescription description{};
    std::vector<DrawCommand> draws;
};

class NullRenderDevice final : public RenderDevice {
public:
    std::uint32_t deviceGeneration() const noexcept override {
        return deviceGeneration_;
    }

    BufferHandle createBuffer(
        const BufferDescription& description) override {
        if (description.sizeBytes == 0) return {};
        return buffers_.create(description);
    }

    TextureHandle createTexture(
        const TextureDescription& description) override {
        if (description.width == 0 || description.height == 0) return {};
        return textures_.create(description);
    }

    PipelineHandle createPipeline(
        const PipelineDescription& description) override {
        return pipelines_.create(description);
    }

    bool destroyBuffer(BufferHandle handle) override {
        return buffers_.destroy(handle);
    }

    bool destroyTexture(TextureHandle handle) override {
        return textures_.destroy(handle);
    }

    bool destroyPipeline(PipelineHandle handle) override {
        return pipelines_.destroy(handle);
    }

    bool beginFrame(const FrameDescription& description) override {
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

    bool submit(const DrawCommand& command) override {
        if (!frameActive_ || !pipelines_.valid(command.pipeline) ||
            !buffers_.valid(command.vertexBuffer) ||
            command.elementCount == 0 || command.instanceCount == 0 ||
            (command.indexBuffer.valid() &&
             !buffers_.valid(command.indexBuffer)) ||
            (command.texture.valid() &&
             !textures_.valid(command.texture))) {
            return false;
        }
        current_.draws.push_back(command);
        return true;
    }

    bool endFrame() override {
        if (!frameActive_) return false;
        frameActive_ = false;
        lastFrame_ = std::move(current_);
        current_ = {};
        ++submittedFrames_;
        return true;
    }

    void reset() noexcept override {
        frameActive_ = false;
        current_ = {};
        lastFrame_ = {};
        buffers_.invalidateAll();
        textures_.invalidateAll();
        pipelines_.invalidateAll();
        ++deviceGeneration_;
        if (deviceGeneration_ == 0) deviceGeneration_ = 1;
    }

    bool valid(BufferHandle handle) const noexcept {
        return buffers_.valid(handle);
    }

    bool valid(TextureHandle handle) const noexcept {
        return textures_.valid(handle);
    }

    bool valid(PipelineHandle handle) const noexcept {
        return pipelines_.valid(handle);
    }

    std::size_t liveBufferCount() const noexcept {
        return buffers_.liveCount();
    }

    std::size_t liveTextureCount() const noexcept {
        return textures_.liveCount();
    }

    std::size_t livePipelineCount() const noexcept {
        return pipelines_.liveCount();
    }

    bool frameActive() const noexcept { return frameActive_; }
    std::uint64_t submittedFrames() const noexcept { return submittedFrames_; }
    const RecordedFrame& lastFrame() const noexcept { return lastFrame_; }

private:
    template<class Handle, class Description>
    class ResourcePool final {
    public:
        Handle create(const Description& description) {
            std::size_t index = 0;
            for (; index < slots_.size(); ++index) {
                if (!slots_[index].alive) break;
            }
            if (index == slots_.size()) slots_.push_back({});
            auto& slot = slots_[index];
            if (slot.generation == 0) slot.generation = 1;
            slot.alive = true;
            slot.description = description;
            return {static_cast<std::uint32_t>(index + 1u), slot.generation};
        }

        bool destroy(Handle handle) noexcept {
            auto* slot = find(handle);
            if (!slot) return false;
            slot->alive = false;
            slot->description = {};
            slot->generation = nextGeneration(slot->generation);
            return true;
        }

        bool valid(Handle handle) const noexcept {
            return find(handle) != nullptr;
        }

        void invalidateAll() noexcept {
            for (auto& slot : slots_) {
                slot.alive = false;
                slot.description = {};
                slot.generation = nextGeneration(slot.generation);
            }
        }

        std::size_t liveCount() const noexcept {
            return static_cast<std::size_t>(std::count_if(
                slots_.begin(), slots_.end(),
                [](const Slot& slot) { return slot.alive; }));
        }

    private:
        struct Slot final {
            std::uint32_t generation{1};
            bool alive{};
            Description description{};
        };

        static std::uint32_t nextGeneration(std::uint32_t value) noexcept {
            ++value;
            return value == 0 ? 1u : value;
        }

        Slot* find(Handle handle) noexcept {
            if (!handle.valid() || handle.index > slots_.size()) return nullptr;
            auto& slot = slots_[handle.index - 1u];
            return slot.alive && slot.generation == handle.generation
                ? &slot : nullptr;
        }

        const Slot* find(Handle handle) const noexcept {
            if (!handle.valid() || handle.index > slots_.size()) return nullptr;
            const auto& slot = slots_[handle.index - 1u];
            return slot.alive && slot.generation == handle.generation
                ? &slot : nullptr;
        }

        std::vector<Slot> slots_;
    };

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
