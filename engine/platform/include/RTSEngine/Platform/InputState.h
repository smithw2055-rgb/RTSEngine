#pragma once

#include <RTSEngine/Platform/Platform.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rts::platform {

struct PointerState final {
    float x{};
    float y{};
    float deltaX{};
    float deltaY{};
    float wheelX{};
    float wheelY{};
    std::array<bool, PointerButtonCount()> down{};
    std::array<bool, PointerButtonCount()> pressed{};
    std::array<bool, PointerButtonCount()> released{};
};

struct TouchPoint final {
    std::uint64_t id{};
    float x{};
    float y{};
    bool active{};
};

class InputState final {
public:
    void beginFrame() noexcept;
    void apply(const PlatformEvent& event);
    void apply(const std::vector<PlatformEvent>& events);

    bool keyDown(KeyCode key) const noexcept;
    bool keyPressed(KeyCode key) const noexcept;
    bool keyReleased(KeyCode key) const noexcept;
    const PointerState& pointer() const noexcept;
    const std::u32string& textInput() const noexcept;
    const std::vector<TouchPoint>& touches() const noexcept;
    const std::vector<std::string>& droppedFiles() const noexcept;
    bool quitRequested() const noexcept;
    bool focused() const noexcept;
    std::uint32_t modifiers() const noexcept;

private:
    static std::size_t keyIndex(KeyCode key) noexcept;
    static std::size_t buttonIndex(PointerButton button) noexcept;
    TouchPoint* findTouch(std::uint64_t id) noexcept;

    std::array<bool, KeyCodeCount()> keysDown_{};
    std::array<bool, KeyCodeCount()> keysPressed_{};
    std::array<bool, KeyCodeCount()> keysReleased_{};
    PointerState pointer_{};
    std::u32string textInput_;
    std::vector<TouchPoint> touches_;
    std::vector<std::string> droppedFiles_;
    bool quitRequested_{};
    bool focused_{true};
    std::uint32_t modifiers_{};
};

} // namespace rts::platform
