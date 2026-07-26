#pragma once

#include <RTSEngine/Platform/Platform.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts::platform {

class NullPlatform final : public Platform {
public:
    bool createWindow(const WindowDescription& description,
                      WindowHandle& output) override;
    bool destroyWindow(WindowHandle window) override;
    bool windowState(WindowHandle window,
                     WindowState& output) const override;
    void pollEvents(std::vector<PlatformEvent>& output) override;
    double monotonicSeconds() const noexcept override;

    bool resize(WindowHandle window,
                std::int32_t width,
                std::int32_t height);
    bool setDpiScale(WindowHandle window, float scale);
    bool setFocused(WindowHandle window, bool focused);
    bool requestClose(WindowHandle window);
    bool key(WindowHandle window, KeyCode key, bool down,
             std::uint32_t modifiers = PlatformModifierNone,
             bool repeat = false);
    bool text(WindowHandle window, std::uint32_t codepoint,
              std::uint32_t modifiers = PlatformModifierNone);
    bool pointerMove(WindowHandle window, float x, float y,
                     std::uint32_t modifiers = PlatformModifierNone);
    bool pointerButton(WindowHandle window, PointerButton button, bool down,
                       float x, float y,
                       std::uint32_t modifiers = PlatformModifierNone);
    bool pointerWheel(WindowHandle window, float deltaX, float deltaY,
                      float x = 0.0f, float y = 0.0f,
                      std::uint32_t modifiers = PlatformModifierNone);
    bool touch(WindowHandle window, std::uint64_t pointerId,
               PlatformEventType type, float x, float y);
    bool dropFiles(WindowHandle window, std::vector<std::string> paths);
    bool advanceTime(double seconds) noexcept;

    std::size_t liveWindowCount() const noexcept;

private:
    struct Slot final {
        std::uint32_t generation{1};
        bool alive{};
        WindowDescription description{};
        WindowState state{};
    };

    static std::uint32_t nextGeneration(std::uint32_t value) noexcept;
    static void updateFramebuffer(WindowState& state) noexcept;

    Slot* find(WindowHandle window) noexcept;
    const Slot* find(WindowHandle window) const noexcept;

    std::vector<Slot> windows_;
    std::vector<PlatformEvent> events_;
    double monotonicSeconds_{};
};

} // namespace rts::platform
