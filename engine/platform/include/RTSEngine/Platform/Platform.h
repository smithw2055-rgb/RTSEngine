#pragma once

#include <cstddef>
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

enum class KeyCode : std::uint16_t {
    Unknown,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Digit0, Digit1, Digit2, Digit3, Digit4,
    Digit5, Digit6, Digit7, Digit8, Digit9,
    Escape, Enter, Space, Tab, Backspace, DeleteKey,
    Left, Right, Up, Down,
    Home, End, PageUp, PageDown,
    LeftShift, RightShift, LeftControl, RightControl,
    LeftAlt, RightAlt,
    F1, F2, F3, F4, F5, F6,
    F7, F8, F9, F10, F11, F12,
    Count
};

enum class PointerButton : std::uint8_t {
    Left,
    Right,
    Middle,
    Auxiliary1,
    Auxiliary2,
    Count
};

enum PlatformModifier : std::uint32_t {
    PlatformModifierNone = 0,
    PlatformModifierShift = 1u << 0u,
    PlatformModifierControl = 1u << 1u,
    PlatformModifierAlt = 1u << 2u,
    PlatformModifierSuper = 1u << 3u,
    PlatformModifierCapsLock = 1u << 4u,
    PlatformModifierNumLock = 1u << 5u
};

enum class PlatformEventType : std::uint8_t {
    QuitRequested,
    WindowResized,
    DpiChanged,
    FocusChanged,
    KeyDown,
    KeyUp,
    TextInput,
    PointerMoved,
    PointerDown,
    PointerUp,
    PointerWheel,
    TouchBegan,
    TouchMoved,
    TouchEnded,
    FilesDropped
};

struct PlatformEvent final {
    // The first six fields intentionally preserve the original aggregate layout
    // used by older tests and NullPlatform window events.
    PlatformEventType type{PlatformEventType::QuitRequested};
    WindowHandle window{};
    std::int32_t width{};
    std::int32_t height{};
    float dpiScale{1.0f};
    bool focused{};

    KeyCode key{KeyCode::Unknown};
    PointerButton button{PointerButton::Left};
    float x{};
    float y{};
    float deltaX{};
    float deltaY{};
    std::uint32_t codepoint{};
    std::uint32_t modifiers{PlatformModifierNone};
    std::uint64_t pointerId{};
    bool repeat{};
    std::vector<std::string> paths;
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

constexpr std::size_t KeyCodeCount() noexcept {
    return static_cast<std::size_t>(KeyCode::Count);
}

constexpr std::size_t PointerButtonCount() noexcept {
    return static_cast<std::size_t>(PointerButton::Count);
}

} // namespace rts::platform
