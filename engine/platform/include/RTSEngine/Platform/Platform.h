#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rts::platform {

struct WindowHandle final {
    std::uint32_t index{};
    std::uint32_t generation{};

    constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }

    friend constexpr bool operator==(WindowHandle a,
                                     WindowHandle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }

    friend constexpr bool operator!=(WindowHandle a,
                                     WindowHandle b) noexcept {
        return !(a == b);
    }
};

struct WindowDescription final {
    std::string title{"RTSEngine"};
    std::int32_t width{1280};
    std::int32_t height{720};
    bool resizable{true};
    bool highDpi{true};
};

struct WindowState final {
    WindowHandle handle{};
    std::int32_t logicalWidth{};
    std::int32_t logicalHeight{};
    std::int32_t framebufferWidth{};
    std::int32_t framebufferHeight{};
    float dpiScale{1.0f};
    bool focused{true};
    bool closeRequested{};
};

enum class PlatformEventType : std::uint8_t {
    QuitRequested,
    WindowResized,
    DpiChanged,
    FocusChanged
};

struct PlatformEvent final {
    PlatformEventType type{PlatformEventType::QuitRequested};
    WindowHandle window{};
    std::int32_t width{};
    std::int32_t height{};
    float dpiScale{1.0f};
    bool focused{};
};

class Platform {
public:
    virtual ~Platform() = default;

    virtual bool createWindow(const WindowDescription& description,
                              WindowHandle& output) = 0;
    virtual bool destroyWindow(WindowHandle window) = 0;
    virtual bool windowState(WindowHandle window,
                             WindowState& output) const = 0;
    virtual void pollEvents(std::vector<PlatformEvent>& output) = 0;

    // Presentation-only wall clock. Authoritative simulation must never read it.
    virtual double monotonicSeconds() const noexcept = 0;
};

} // namespace rts::platform
