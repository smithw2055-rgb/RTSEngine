#include <RTSEngine/Platform/SokolAppPlatform.h>

#include <algorithm>
#include <cmath>
#include <string>

#include <sokol_app.h>

namespace rts::platform {

SokolAppPlatform::SokolAppPlatform() noexcept
    : started_(std::chrono::steady_clock::now()) {}

bool SokolAppPlatform::createWindow(const WindowDescription& description,
                                    WindowHandle& output) {
    if (alive_ || !sapp_isvalid()) return false;
    description_ = description;
    alive_ = true;
    state_.handle = {1, generation_};
    state_.focused = true;
    state_.closeRequested = false;
    refreshState();
    output = state_.handle;
    return true;
}

bool SokolAppPlatform::destroyWindow(WindowHandle window) {
    if (!alive_ || window != state_.handle) return false;
    alive_ = false;
    state_ = {};
    ++generation_;
    if (generation_ == 0) generation_ = 1;
    events_.clear();
    return true;
}

bool SokolAppPlatform::windowState(WindowHandle window,
                                   WindowState& output) const {
    if (!alive_ || window != state_.handle) return false;
    output = state_;
    return true;
}

void SokolAppPlatform::pollEvents(std::vector<PlatformEvent>& output) {
    refreshState();
    output.insert(output.end(), events_.begin(), events_.end());
    events_.clear();
}

double SokolAppPlatform::monotonicSeconds() const noexcept {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started_).count();
}

void SokolAppPlatform::handleEvent(const sapp_event& event) {
    if (!alive_) return;
    PlatformEvent translated;
    translated.window = state_.handle;
    translated.modifiers = mapModifiers(event.modifiers);
    translated.x = event.mouse_x;
    translated.y = event.mouse_y;
    translated.deltaX = event.mouse_dx;
    translated.deltaY = event.mouse_dy;

    switch (event.type) {
    case SAPP_EVENTTYPE_KEY_DOWN:
    case SAPP_EVENTTYPE_KEY_UP:
        translated.type = event.type == SAPP_EVENTTYPE_KEY_DOWN
            ? PlatformEventType::KeyDown : PlatformEventType::KeyUp;
        translated.key = mapKey(static_cast<std::uint32_t>(event.key_code));
        translated.repeat = event.key_repeat;
        events_.push_back(std::move(translated));
        break;
    case SAPP_EVENTTYPE_CHAR:
        translated.type = PlatformEventType::TextInput;
        translated.codepoint = event.char_code;
        translated.repeat = event.key_repeat;
        events_.push_back(std::move(translated));
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        translated.type = PlatformEventType::PointerMoved;
        events_.push_back(std::move(translated));
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
    case SAPP_EVENTTYPE_MOUSE_UP:
        translated.type = event.type == SAPP_EVENTTYPE_MOUSE_DOWN
            ? PlatformEventType::PointerDown : PlatformEventType::PointerUp;
        translated.button = mapButton(
            static_cast<std::uint32_t>(event.mouse_button));
        events_.push_back(std::move(translated));
        break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        translated.type = PlatformEventType::PointerWheel;
        translated.deltaX = event.scroll_x;
        translated.deltaY = event.scroll_y;
        events_.push_back(std::move(translated));
        break;
    case SAPP_EVENTTYPE_TOUCHES_BEGAN:
    case SAPP_EVENTTYPE_TOUCHES_MOVED:
    case SAPP_EVENTTYPE_TOUCHES_ENDED:
    case SAPP_EVENTTYPE_TOUCHES_CANCELLED:
        for (int index = 0; index < event.num_touches; ++index) {
            const auto& touch = event.touches[index];
            if (!touch.changed) continue;
            PlatformEvent touchEvent;
            touchEvent.window = state_.handle;
            touchEvent.pointerId = static_cast<std::uint64_t>(touch.identifier);
            touchEvent.x = touch.pos_x;
            touchEvent.y = touch.pos_y;
            touchEvent.type = event.type == SAPP_EVENTTYPE_TOUCHES_BEGAN
                ? PlatformEventType::TouchBegan
                : event.type == SAPP_EVENTTYPE_TOUCHES_MOVED
                    ? PlatformEventType::TouchMoved
                    : PlatformEventType::TouchEnded;
            events_.push_back(std::move(touchEvent));
        }
        break;
    case SAPP_EVENTTYPE_RESIZED: {
        const auto previousDpi = state_.dpiScale;
        state_.logicalWidth = event.window_width;
        state_.logicalHeight = event.window_height;
        state_.framebufferWidth = event.framebuffer_width;
        state_.framebufferHeight = event.framebuffer_height;
        state_.dpiScale = event.window_width > 0
            ? static_cast<float>(event.framebuffer_width) /
              static_cast<float>(event.window_width) : 1.0f;
        pushWindowEvent(PlatformEventType::WindowResized);
        if (std::abs(state_.dpiScale - previousDpi) > 0.001f) {
            pushWindowEvent(PlatformEventType::DpiChanged);
        }
        break;
    }
    case SAPP_EVENTTYPE_FOCUSED:
    case SAPP_EVENTTYPE_UNFOCUSED:
        state_.focused = event.type == SAPP_EVENTTYPE_FOCUSED;
        pushWindowEvent(PlatformEventType::FocusChanged);
        break;
    case SAPP_EVENTTYPE_QUIT_REQUESTED:
        state_.closeRequested = true;
        pushWindowEvent(PlatformEventType::QuitRequested);
        break;
    case SAPP_EVENTTYPE_FILES_DROPPED: {
        translated.type = PlatformEventType::FilesDropped;
        const auto count = sapp_get_num_dropped_files();
        for (int index = 0; index < count; ++index) {
            const auto* path = sapp_get_dropped_file_path(index);
            if (path && *path) translated.paths.emplace_back(path);
        }
        events_.push_back(std::move(translated));
        break;
    }
    default:
        break;
    }
}

KeyCode SokolAppPlatform::mapKey(std::uint32_t key) noexcept {
    if (key >= SAPP_KEYCODE_A && key <= SAPP_KEYCODE_Z) {
        return static_cast<KeyCode>(
            static_cast<std::uint16_t>(KeyCode::A) + key - SAPP_KEYCODE_A);
    }
    if (key >= SAPP_KEYCODE_0 && key <= SAPP_KEYCODE_9) {
        return static_cast<KeyCode>(
            static_cast<std::uint16_t>(KeyCode::Digit0) + key - SAPP_KEYCODE_0);
    }
    if (key >= SAPP_KEYCODE_F1 && key <= SAPP_KEYCODE_F12) {
        return static_cast<KeyCode>(
            static_cast<std::uint16_t>(KeyCode::F1) + key - SAPP_KEYCODE_F1);
    }
    switch (key) {
    case SAPP_KEYCODE_ESCAPE: return KeyCode::Escape;
    case SAPP_KEYCODE_ENTER: return KeyCode::Enter;
    case SAPP_KEYCODE_SPACE: return KeyCode::Space;
    case SAPP_KEYCODE_TAB: return KeyCode::Tab;
    case SAPP_KEYCODE_BACKSPACE: return KeyCode::Backspace;
    case SAPP_KEYCODE_DELETE: return KeyCode::DeleteKey;
    case SAPP_KEYCODE_LEFT: return KeyCode::Left;
    case SAPP_KEYCODE_RIGHT: return KeyCode::Right;
    case SAPP_KEYCODE_UP: return KeyCode::Up;
    case SAPP_KEYCODE_DOWN: return KeyCode::Down;
    case SAPP_KEYCODE_HOME: return KeyCode::Home;
    case SAPP_KEYCODE_END: return KeyCode::End;
    case SAPP_KEYCODE_PAGE_UP: return KeyCode::PageUp;
    case SAPP_KEYCODE_PAGE_DOWN: return KeyCode::PageDown;
    case SAPP_KEYCODE_LEFT_SHIFT: return KeyCode::LeftShift;
    case SAPP_KEYCODE_RIGHT_SHIFT: return KeyCode::RightShift;
    case SAPP_KEYCODE_LEFT_CONTROL: return KeyCode::LeftControl;
    case SAPP_KEYCODE_RIGHT_CONTROL: return KeyCode::RightControl;
    case SAPP_KEYCODE_LEFT_ALT: return KeyCode::LeftAlt;
    case SAPP_KEYCODE_RIGHT_ALT: return KeyCode::RightAlt;
    default: return KeyCode::Unknown;
    }
}

PointerButton SokolAppPlatform::mapButton(std::uint32_t button) noexcept {
    switch (button) {
    case SAPP_MOUSEBUTTON_LEFT: return PointerButton::Left;
    case SAPP_MOUSEBUTTON_RIGHT: return PointerButton::Right;
    case SAPP_MOUSEBUTTON_MIDDLE: return PointerButton::Middle;
    default: return PointerButton::Auxiliary1;
    }
}

std::uint32_t SokolAppPlatform::mapModifiers(
    std::uint32_t modifiers) noexcept {
    std::uint32_t output = PlatformModifierNone;
    if (modifiers & SAPP_MODIFIER_SHIFT) output |= PlatformModifierShift;
    if (modifiers & SAPP_MODIFIER_CTRL) output |= PlatformModifierControl;
    if (modifiers & SAPP_MODIFIER_ALT) output |= PlatformModifierAlt;
    if (modifiers & SAPP_MODIFIER_SUPER) output |= PlatformModifierSuper;
    return output;
}

void SokolAppPlatform::refreshState() noexcept {
    if (!alive_ || !sapp_isvalid()) return;
    const auto framebufferWidth = std::max(0, sapp_width());
    const auto framebufferHeight = std::max(0, sapp_height());
    const auto dpi = std::max(0.1f, sapp_dpi_scale());
    state_.framebufferWidth = framebufferWidth;
    state_.framebufferHeight = framebufferHeight;
    state_.logicalWidth = static_cast<std::int32_t>(
        std::lround(static_cast<double>(framebufferWidth) / dpi));
    state_.logicalHeight = static_cast<std::int32_t>(
        std::lround(static_cast<double>(framebufferHeight) / dpi));
    state_.dpiScale = dpi;
}

void SokolAppPlatform::pushWindowEvent(PlatformEventType type) {
    PlatformEvent event;
    event.type = type;
    event.window = state_.handle;
    event.width = state_.logicalWidth;
    event.height = state_.logicalHeight;
    event.dpiScale = state_.dpiScale;
    event.focused = state_.focused;
    events_.push_back(std::move(event));
}

} // namespace rts::platform
