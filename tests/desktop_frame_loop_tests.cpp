#include <RTSEngine/Platform/NullPlatform.h>
#include <RTSEngine/Runtime/DesktopFrameLoop.h>

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
    std::uint64_t observedTick = 0;
    runtime::DesktopFrameLoop loop(
        platform,
        [&](const sim::TickContext& tick) { observedTick = tick.tick; },
        {{"frame-loop", 800, 450, true, true}, 30.0, 4, 0.25});
    Require(loop.initialize(), "frame loop initializes a window");

    platform.advanceTime(1.0 / 30.0);
    bool beforeCalled = false;
    bool renderCalled = false;
    const auto first = loop.advanceFrame(
        [&](const runtime::DesktopFrameContext& frame) {
            beforeCalled = true;
            Require(frame.targetTick == 1, "input targets next authoritative tick");
            Require(frame.windowState.framebufferWidth == 800,
                    "window state is available before simulation");
        },
        [&](const runtime::DesktopFrameContext& frame,
            const sim::FrameStepPlan& plan) {
            renderCalled = true;
            Require(frame.currentTick == 1, "render observes committed tick");
            Require(plan.tickCount == 1, "one fixed tick is advanced");
            return true;
        });
    Require(beforeCalled && renderCalled && first.rendered,
            "callbacks run in input-step-render order");
    Require(observedTick == 1 && loop.currentTick() == 1,
            "simulation host owns tick advancement");

    platform.key(loop.window(), platform::KeyCode::A, true);
    platform.advanceTime(1.0 / 60.0);
    const auto second = loop.advanceFrame();
    Require(loop.input().keyPressed(platform::KeyCode::A),
            "platform events feed frame input state");
    Require(second.stepPlan.tickCount == 0 || second.stepPlan.tickCount == 1,
            "fractional frame time remains bounded");

    platform.requestClose(loop.window());
    platform.advanceTime(1.0 / 60.0);
    const auto closing = loop.advanceFrame();
    Require(closing.quitRequested && loop.quitRequested(),
            "window close terminates the loop");
    loop.shutdown();
    Require(!loop.initialized(), "shutdown releases the window");
    std::cout << "desktop frame loop tests passed\n";
}
