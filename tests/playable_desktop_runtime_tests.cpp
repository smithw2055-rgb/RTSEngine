#include <RTSEngine/Audio/NullAudioDevice.h>
#include <RTSEngine/Platform/NullPlatform.h>
#include <RTSEngine/Render/NullRenderDevice.h>
#include <RTSEngine/RtsDesktop/PlayableDesktopRuntime.h>

#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main() {
    using namespace rts;
    platform::NullPlatform platform;
    render::NullRenderDevice render;
    audio::NullAudioDevice audio;
    rts_desktop::PlayableDesktopRuntime runtime(
        platform, render, audio,
        {{"playable-test", 960, 540, true, true}, 32, 18, 77, 1, 2});
    Require(runtime.initialize(), "playable runtime initializes");

    for (int i = 0; i < 4; ++i) {
        platform.advanceTime(1.0 / 30.0);
        Require(runtime.advanceFrame().rendered, "frame renders through null backend");
    }
    Require(runtime.stats().simulationTicks >= 4,
            "fixed simulation advances");
    Require(render.submittedFrames() >= 4,
            "renderer submits complete frames");
    Require(runtime.stats().renderer.screenUiQuads > 0,
            "HUD contributes screen-space quads");
    Require(runtime.stats().renderer.spriteQuads > 0,
            "simulation snapshots produce world sprites");

    const auto window = runtime.advanceFrame().context.window;
    platform.pointerMove(window, 210.0f, 270.0f);
    platform.pointerButton(window, platform::PointerButton::Left,
                           true, 210.0f, 270.0f);
    platform.advanceTime(1.0 / 30.0);
    runtime.advanceFrame();
    platform.pointerButton(window, platform::PointerButton::Left,
                           false, 210.0f, 270.0f);
    platform.advanceTime(1.0 / 30.0);
    runtime.advanceFrame();
    Require(!runtime.controller().selection().empty(),
            "click selection reaches the desktop controller");

    platform.pointerButton(window, platform::PointerButton::Right,
                           true, 360.0f, 270.0f);
    platform.advanceTime(1.0 / 30.0);
    runtime.advanceFrame();
    platform.pointerButton(window, platform::PointerButton::Right,
                           false, 360.0f, 270.0f);
    platform.advanceTime(1.0 / 30.0);
    runtime.advanceFrame();
    Require(runtime.stats().playerCommands > 0,
            "right click submits authoritative RTS commands");

    Require(runtime.saveToMemory() && runtime.hasMemorySave(),
            "runtime creates an authoritative memory save");
    for (int i = 0; i < 3; ++i) {
        platform.advanceTime(1.0 / 30.0);
        runtime.advanceFrame();
    }
    Require(runtime.restoreFromMemory(), "runtime restores the saved run");
    platform.advanceTime(1.0 / 30.0);
    Require(runtime.advanceFrame().rendered,
            "rendering continues after tick-clock resynchronization");
    Require(runtime.stats().saveCount == 1 && runtime.stats().restoreCount == 1,
            "save/restore telemetry is recorded");

    runtime.shutdown();
    std::cout << "playable desktop runtime tests passed\n";
}
