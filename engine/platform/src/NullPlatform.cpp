#include <RTSEngine/Platform/NullPlatform.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace rts::platform {

bool NullPlatform::createWindow(const WindowDescription& description,
                                WindowHandle& output) {
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

bool NullPlatform::destroyWindow(WindowHandle window) {
    auto* slot = find(window);
    if (!slot) return false;
    slot->alive = false;
    slot->state = {};
    slot->description = {};
    slot->generation = nextGeneration(slot->generation);
    return true;
}

bool NullPlatform::windowState(WindowHandle window,
                               WindowState& output) const {
    const auto* slot = find(window);
    if (!slot) return false;
    output = slot->state;
    return true;
}

void NullPlatform::pollEvents(std::vector<PlatformEvent>& output) {
    output = std::move(events_);
    events_.clear();
}

double NullPlatform::monotonicSeconds() const noexcept {
    return monotonicSeconds_;
}

bool NullPlatform::resize(WindowHandle window,
                          std::int32_t width,
                          std::int32_t height) {
    auto* slot = find(window);
    if (!slot || width <= 0 || height <= 0) return false;
    slot->state.logicalWidth = width;
    slot->state.logicalHeight = height;
    updateFramebuffer(slot->state);
    PlatformEvent event;
    event.type = PlatformEventType::WindowResized;
    event.window = window;
    event.width = width;
    event.height = height;
    event.dpiScale = slot->state.dpiScale;
    event.focused = slot->state.focused;
    events_.push_back(std::move(event));
    return true;
}

bool NullPlatform::setDpiScale(WindowHandle window, float scale) {
    auto* slot = find(window);
    if (!slot || !(scale > 0.0f)) return false;
    slot->state.dpiScale = scale;
    updateFramebuffer(slot->state);
    PlatformEvent event;
    event.type = PlatformEventType::DpiChanged;
    event.window = window;
    event.width = slot->state.logicalWidth;
    event.height = slot->state.logicalHeight;
    event.dpiScale = scale;
    event.focused = slot->state.focused;
    events_.push_back(std::move(event));
    return true;
}

bool NullPlatform::setFocused(WindowHandle window, bool focused) {
    auto* slot = find(window);
    if (!slot) return false;
    slot->state.focused = focused;
    PlatformEvent event;
    event.type = PlatformEventType::FocusChanged;
    event.window = window;
    event.width = slot->state.logicalWidth;
    event.height = slot->state.logicalHeight;
    event.dpiScale = slot->state.dpiScale;
    event.focused = focused;
    events_.push_back(std::move(event));
    return true;
}

bool NullPlatform::requestClose(WindowHandle window) {
    auto* slot = find(window);
    if (!slot) return false;
    slot->state.closeRequested = true;
    PlatformEvent event;
    event.type = PlatformEventType::QuitRequested;
    event.window = window;
    event.width = slot->state.logicalWidth;
    event.height = slot->state.logicalHeight;
    event.dpiScale = slot->state.dpiScale;
    event.focused = slot->state.focused;
    events_.push_back(std::move(event));
    return true;
}


bool NullPlatform::key(WindowHandle window,
                       KeyCode keyCode,
                       bool down,
                       std::uint32_t modifiers,
                       bool repeat) {
    if (!find(window) || keyCode == KeyCode::Unknown ||
        keyCode == KeyCode::Count) return false;
    PlatformEvent event;
    event.type = down ? PlatformEventType::KeyDown
                      : PlatformEventType::KeyUp;
    event.window = window;
    event.key = keyCode;
    event.modifiers = modifiers;
    event.repeat = repeat;
    events_.push_back(std::move(event));
    return true;
}

bool NullPlatform::text(WindowHandle window,
                        std::uint32_t codepoint,
                        std::uint32_t modifiers) {
    if (!find(window) || codepoint == 0 || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) return false;
    PlatformEvent event;
    event.type = PlatformEventType::TextInput;
    event.window = window;
    event.codepoint = codepoint;
    event.modifiers = modifiers;
    events_.push_back(std::move(event));
    return true;
}

bool NullPlatform::pointerMove(WindowHandle window,
                               float x,
                               float y,
                               std::uint32_t modifiers) {
    if (!find(window)) return false;
    PlatformEvent event;
    event.type = PlatformEventType::PointerMoved;
    event.window = window;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    events_.push_back(std::move(event));
    return true;
}

bool NullPlatform::pointerButton(WindowHandle window,
                                 PointerButton buttonValue,
                                 bool down,
                                 float x,
                                 float y,
                                 std::uint32_t modifiers) {
    if (!find(window) || buttonValue == PointerButton::Count) return false;
    PlatformEvent event;
    event.type = down ? PlatformEventType::PointerDown
                      : PlatformEventType::PointerUp;
    event.window = window;
    event.button = buttonValue;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    events_.push_back(std::move(event));
    return true;
}

bool NullPlatform::pointerWheel(WindowHandle window,
                                float deltaX,
                                float deltaY,
                                float x,
                                float y,
                                std::uint32_t modifiers) {
    if (!find(window)) return false;
    PlatformEvent event;
    event.type = PlatformEventType::PointerWheel;
    event.window = window;
    event.x = x;
    event.y = y;
    event.deltaX = deltaX;
    event.deltaY = deltaY;
    event.modifiers = modifiers;
    events_.push_back(std::move(event));
    return true;
}

bool NullPlatform::touch(WindowHandle window,
                         std::uint64_t pointerId,
                         PlatformEventType type,
                         float x,
                         float y) {
    if (!find(window) || pointerId == 0 ||
        (type != PlatformEventType::TouchBegan &&
         type != PlatformEventType::TouchMoved &&
         type != PlatformEventType::TouchEnded)) return false;
    PlatformEvent event;
    event.type = type;
    event.window = window;
    event.pointerId = pointerId;
    event.x = x;
    event.y = y;
    events_.push_back(std::move(event));
    return true;
}

bool NullPlatform::dropFiles(WindowHandle window,
                             std::vector<std::string> paths) {
    if (!find(window) || paths.empty()) return false;
    PlatformEvent event;
    event.type = PlatformEventType::FilesDropped;
    event.window = window;
    event.paths = std::move(paths);
    events_.push_back(std::move(event));
    return true;
}

bool NullPlatform::advanceTime(double seconds) noexcept {
    if (!(seconds >= 0.0) ||
        seconds > std::numeric_limits<double>::max() - monotonicSeconds_) {
        return false;
    }
    monotonicSeconds_ += seconds;
    return true;
}

std::size_t NullPlatform::liveWindowCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        windows_.begin(), windows_.end(),
        [](const Slot& slot) { return slot.alive; }));
}

std::uint32_t NullPlatform::nextGeneration(std::uint32_t value) noexcept {
    ++value;
    return value == 0 ? 1u : value;
}

void NullPlatform::updateFramebuffer(WindowState& state) noexcept {
    const auto scale = std::max(0.001f, state.dpiScale);
    state.framebufferWidth = static_cast<std::int32_t>(
        static_cast<float>(state.logicalWidth) * scale + 0.5f);
    state.framebufferHeight = static_cast<std::int32_t>(
        static_cast<float>(state.logicalHeight) * scale + 0.5f);
}

NullPlatform::Slot* NullPlatform::find(WindowHandle window) noexcept {
    if (!window.valid() || window.index > windows_.size()) return nullptr;
    auto& slot = windows_[window.index - 1u];
    return slot.alive && slot.generation == window.generation
        ? &slot : nullptr;
}

const NullPlatform::Slot* NullPlatform::find(WindowHandle window) const noexcept {
    if (!window.valid() || window.index > windows_.size()) return nullptr;
    const auto& slot = windows_[window.index - 1u];
    return slot.alive && slot.generation == window.generation
        ? &slot : nullptr;
}

} // namespace rts::platform
