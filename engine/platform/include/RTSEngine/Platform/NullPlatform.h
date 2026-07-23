#pragma once

#include <RTSEngine/Platform/Platform.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::platform {

class NullPlatform final : public Platform {
public:
    bool createWindow(const WindowDescription& description,
                      WindowHandle& output) override {
        if (description.width <= 0 || description.height <= 0) return false;

        std::size_t slotIndex = 0;
        for (; slotIndex < windows_.size(); ++slotIndex) {
            if (!windows_[slotIndex].alive) break;
        }
        if (slotIndex == windows_.size()) windows_.push_back({});

        auto& slot = windows_[slotIndex];
        if (slot.generation == 0) slot.generation = 1;
        slot.alive = true;
        slot.description = description;
        slot.state = {};
        slot.state.handle = {
            static_cast<std::uint32_t>(slotIndex + 1u), slot.generation};
        slot.state.logicalWidth = description.width;
        slot.state.logicalHeight = description.height;
        slot.state.dpiScale = 1.0f;
        updateFramebuffer(slot.state);
        output = slot.state.handle;
        return true;
    }

    bool destroyWindow(WindowHandle window) override {
        auto* slot = find(window);
        if (!slot) return false;
        slot->alive = false;
        slot->state = {};
        slot->description = {};
        slot->generation = nextGeneration(slot->generation);
        return true;
    }

    bool windowState(WindowHandle window,
                     WindowState& output) const override {
        const auto* slot = find(window);
        if (!slot) return false;
        output = slot->state;
        return true;
    }

    void pollEvents(std::vector<PlatformEvent>& output) override {
        output = std::move(events_);
        events_.clear();
    }

    double monotonicSeconds() const noexcept override {
        return monotonicSeconds_;
    }

    bool resize(WindowHandle window,
                std::int32_t width,
                std::int32_t height) {
        auto* slot = find(window);
        if (!slot || width <= 0 || height <= 0) return false;
        slot->state.logicalWidth = width;
        slot->state.logicalHeight = height;
        updateFramebuffer(slot->state);
        events_.push_back(
            {PlatformEventType::WindowResized, window,
             width, height, slot->state.dpiScale, slot->state.focused});
        return true;
    }

    bool setDpiScale(WindowHandle window, float scale) {
        auto* slot = find(window);
        if (!slot || !(scale > 0.0f)) return false;
        slot->state.dpiScale = scale;
        updateFramebuffer(slot->state);
        events_.push_back(
            {PlatformEventType::DpiChanged, window,
             slot->state.logicalWidth, slot->state.logicalHeight,
             scale, slot->state.focused});
        return true;
    }

    bool setFocused(WindowHandle window, bool focused) {
        auto* slot = find(window);
        if (!slot) return false;
        slot->state.focused = focused;
        events_.push_back(
            {PlatformEventType::FocusChanged, window,
             slot->state.logicalWidth, slot->state.logicalHeight,
             slot->state.dpiScale, focused});
        return true;
    }

    bool requestClose(WindowHandle window) {
        auto* slot = find(window);
        if (!slot) return false;
        slot->state.closeRequested = true;
        events_.push_back(
            {PlatformEventType::QuitRequested, window,
             slot->state.logicalWidth, slot->state.logicalHeight,
             slot->state.dpiScale, slot->state.focused});
        return true;
    }

    bool advanceTime(double seconds) noexcept {
        if (!(seconds >= 0.0) ||
            seconds > std::numeric_limits<double>::max() - monotonicSeconds_) {
            return false;
        }
        monotonicSeconds_ += seconds;
        return true;
    }

    std::size_t liveWindowCount() const noexcept {
        return static_cast<std::size_t>(std::count_if(
            windows_.begin(), windows_.end(),
            [](const Slot& slot) { return slot.alive; }));
    }

private:
    struct Slot final {
        std::uint32_t generation{1};
        bool alive{};
        WindowDescription description{};
        WindowState state{};
    };

    static std::uint32_t nextGeneration(std::uint32_t value) noexcept {
        ++value;
        return value == 0 ? 1u : value;
    }

    static void updateFramebuffer(WindowState& state) noexcept {
        const auto scale = std::max(0.001f, state.dpiScale);
        state.framebufferWidth = static_cast<std::int32_t>(
            static_cast<float>(state.logicalWidth) * scale + 0.5f);
        state.framebufferHeight = static_cast<std::int32_t>(
            static_cast<float>(state.logicalHeight) * scale + 0.5f);
    }

    Slot* find(WindowHandle window) noexcept {
        if (!window.valid() || window.index > windows_.size()) return nullptr;
        auto& slot = windows_[window.index - 1u];
        return slot.alive && slot.generation == window.generation
            ? &slot : nullptr;
    }

    const Slot* find(WindowHandle window) const noexcept {
        if (!window.valid() || window.index > windows_.size()) return nullptr;
        const auto& slot = windows_[window.index - 1u];
        return slot.alive && slot.generation == window.generation
            ? &slot : nullptr;
    }

    std::vector<Slot> windows_;
    std::vector<PlatformEvent> events_;
    double monotonicSeconds_{};
};

} // namespace rts::platform
