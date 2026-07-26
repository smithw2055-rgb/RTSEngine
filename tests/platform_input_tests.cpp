#include <RTSEngine/Platform/InputState.h>
#include <RTSEngine/Platform/NullPlatform.h>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    using namespace rts::platform;

    NullPlatform platform;
    WindowHandle window;
    require(platform.createWindow({"input", 800, 600, true, true}, window),
            "window creation failed");

    require(platform.pointerMove(window, 100.0f, 80.0f),
            "pointer move injection failed");
    require(platform.pointerButton(window, PointerButton::Left, true,
                                   100.0f, 80.0f),
            "pointer down injection failed");
    require(platform.key(window, KeyCode::B, true,
                         PlatformModifierControl),
            "key down injection failed");
    require(platform.text(window, U'\u5854', PlatformModifierControl),
            "text injection failed");
    require(platform.pointerWheel(window, 0.0f, 2.0f, 100.0f, 80.0f,
                                  PlatformModifierControl),
            "wheel injection failed");
    require(platform.touch(window, 42, PlatformEventType::TouchBegan,
                           20.0f, 30.0f),
            "touch injection failed");
    require(platform.dropFiles(window, {"map.rtsmap", "units.json"}),
            "drop-file injection failed");

    std::vector<PlatformEvent> events;
    platform.pollEvents(events);
    require(events.size() == 7, "unexpected event count");

    InputState input;
    input.beginFrame();
    input.apply(events);
    require(input.keyDown(KeyCode::B), "key should be down");
    require(input.keyPressed(KeyCode::B), "key should be pressed");
    require(input.modifiers() == PlatformModifierControl,
            "modifiers were not preserved");
    require(input.pointer().x == 100.0f && input.pointer().y == 80.0f,
            "pointer position mismatch");
    require(input.pointer().pressed[0], "left button should be pressed");
    require(input.pointer().wheelY == 2.0f, "wheel mismatch");
    require(input.textInput() == U"\u5854", "text input mismatch");
    require(input.touches().size() == 1 && input.touches().front().active,
            "touch was not tracked");
    require(input.droppedFiles().size() == 2,
            "dropped files were not tracked");

    input.beginFrame();
    require(input.keyDown(KeyCode::B), "held key should remain down");
    require(!input.keyPressed(KeyCode::B), "pressed edge must clear");
    require(input.textInput().empty(), "text must clear each frame");
    require(input.droppedFiles().empty(),
            "dropped files must clear each frame");

    require(platform.key(window, KeyCode::B, false),
            "key up injection failed");
    require(platform.pointerButton(window, PointerButton::Left, false,
                                   105.0f, 85.0f),
            "pointer up injection failed");
    require(platform.touch(window, 42, PlatformEventType::TouchEnded,
                           20.0f, 30.0f),
            "touch end injection failed");
    platform.pollEvents(events);
    input.apply(events);
    require(input.keyReleased(KeyCode::B) && !input.keyDown(KeyCode::B),
            "key release mismatch");
    require(input.pointer().released[0] && !input.pointer().down[0],
            "pointer release mismatch");
    require(!input.touches().front().active, "touch should be inactive");

    return 0;
}
