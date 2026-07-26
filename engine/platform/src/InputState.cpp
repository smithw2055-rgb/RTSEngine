#include <RTSEngine/Platform/InputState.h>

#include <algorithm>

namespace rts::platform {

void InputState::beginFrame() noexcept {
    keysPressed_.fill(false);
    keysReleased_.fill(false);
    pointer_.pressed.fill(false);
    pointer_.released.fill(false);
    pointer_.deltaX = 0.0f;
    pointer_.deltaY = 0.0f;
    pointer_.wheelX = 0.0f;
    pointer_.wheelY = 0.0f;
    textInput_.clear();
    droppedFiles_.clear();
    quitRequested_ = false;
    touches_.erase(
        std::remove_if(touches_.begin(), touches_.end(),
                       [](const TouchPoint& point) { return !point.active; }),
        touches_.end());
}

void InputState::apply(const PlatformEvent& event) {
    switch (event.type) {
    case PlatformEventType::QuitRequested:
        quitRequested_ = true;
        break;
    case PlatformEventType::FocusChanged:
        focused_ = event.focused;
        if (!focused_) {
            keysDown_.fill(false);
            pointer_.down.fill(false);
        }
        break;
    case PlatformEventType::KeyDown: {
        modifiers_ = event.modifiers;
        const auto index = keyIndex(event.key);
        if (index < keysDown_.size()) {
            if (!keysDown_[index]) keysPressed_[index] = true;
            keysDown_[index] = true;
        }
        break;
    }
    case PlatformEventType::KeyUp: {
        modifiers_ = event.modifiers;
        const auto index = keyIndex(event.key);
        if (index < keysDown_.size()) {
            if (keysDown_[index]) keysReleased_[index] = true;
            keysDown_[index] = false;
        }
        break;
    }
    case PlatformEventType::TextInput:
        modifiers_ = event.modifiers;
        if (event.codepoint != 0 && event.codepoint <= 0x10FFFFu &&
            !(event.codepoint >= 0xD800u && event.codepoint <= 0xDFFFu)) {
            textInput_.push_back(static_cast<char32_t>(event.codepoint));
        }
        break;
    case PlatformEventType::PointerMoved:
        modifiers_ = event.modifiers;
        pointer_.deltaX += event.x - pointer_.x;
        pointer_.deltaY += event.y - pointer_.y;
        pointer_.x = event.x;
        pointer_.y = event.y;
        break;
    case PlatformEventType::PointerDown: {
        modifiers_ = event.modifiers;
        pointer_.x = event.x;
        pointer_.y = event.y;
        const auto index = buttonIndex(event.button);
        if (index < pointer_.down.size()) {
            if (!pointer_.down[index]) pointer_.pressed[index] = true;
            pointer_.down[index] = true;
        }
        break;
    }
    case PlatformEventType::PointerUp: {
        modifiers_ = event.modifiers;
        pointer_.x = event.x;
        pointer_.y = event.y;
        const auto index = buttonIndex(event.button);
        if (index < pointer_.down.size()) {
            if (pointer_.down[index]) pointer_.released[index] = true;
            pointer_.down[index] = false;
        }
        break;
    }
    case PlatformEventType::PointerWheel:
        modifiers_ = event.modifiers;
        pointer_.wheelX += event.deltaX;
        pointer_.wheelY += event.deltaY;
        break;
    case PlatformEventType::TouchBegan:
    case PlatformEventType::TouchMoved:
    case PlatformEventType::TouchEnded: {
        auto* point = findTouch(event.pointerId);
        if (!point) {
            touches_.push_back({event.pointerId, event.x, event.y, true});
            point = &touches_.back();
        }
        point->x = event.x;
        point->y = event.y;
        point->active = event.type != PlatformEventType::TouchEnded;
        break;
    }
    case PlatformEventType::FilesDropped:
        droppedFiles_.insert(
            droppedFiles_.end(), event.paths.begin(), event.paths.end());
        break;
    case PlatformEventType::WindowResized:
    case PlatformEventType::DpiChanged:
        break;
    }
}

void InputState::apply(const std::vector<PlatformEvent>& events) {
    for (const auto& event : events) apply(event);
}

bool InputState::keyDown(KeyCode key) const noexcept {
    const auto index = keyIndex(key);
    return index < keysDown_.size() && keysDown_[index];
}

bool InputState::keyPressed(KeyCode key) const noexcept {
    const auto index = keyIndex(key);
    return index < keysPressed_.size() && keysPressed_[index];
}

bool InputState::keyReleased(KeyCode key) const noexcept {
    const auto index = keyIndex(key);
    return index < keysReleased_.size() && keysReleased_[index];
}

const PointerState& InputState::pointer() const noexcept { return pointer_; }
const std::u32string& InputState::textInput() const noexcept { return textInput_; }
const std::vector<TouchPoint>& InputState::touches() const noexcept { return touches_; }
const std::vector<std::string>& InputState::droppedFiles() const noexcept {
    return droppedFiles_;
}
bool InputState::quitRequested() const noexcept { return quitRequested_; }
bool InputState::focused() const noexcept { return focused_; }
std::uint32_t InputState::modifiers() const noexcept { return modifiers_; }

std::size_t InputState::keyIndex(KeyCode key) noexcept {
    return static_cast<std::size_t>(key);
}

std::size_t InputState::buttonIndex(PointerButton button) noexcept {
    return static_cast<std::size_t>(button);
}

TouchPoint* InputState::findTouch(std::uint64_t id) noexcept {
    const auto iterator = std::find_if(
        touches_.begin(), touches_.end(),
        [id](const TouchPoint& point) { return point.id == id; });
    return iterator == touches_.end() ? nullptr : &*iterator;
}

} // namespace rts::platform
