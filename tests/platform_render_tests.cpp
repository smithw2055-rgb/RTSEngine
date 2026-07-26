#include <RTSEngine/Platform/NullPlatform.h>
#include <RTSEngine/Render/NullRenderDevice.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace rts;

void check(bool condition) {
    assert(condition);
    if (!condition) std::abort();
}

void testNullPlatformLifecycle() {
    platform::NullPlatform platform;
    platform::WindowHandle invalid;
    check(!platform.createWindow({"bad", 0, 720, true, true}, invalid));

    platform::WindowHandle window;
    check(platform.createWindow({"test", 800, 600, true, true}, window));
    check(window.valid());
    check(platform.liveWindowCount() == 1);

    platform::WindowState state;
    check(platform.windowState(window, state));
    check(state.logicalWidth == 800);
    check(state.logicalHeight == 600);
    check(state.framebufferWidth == 800);
    check(state.framebufferHeight == 600);

    check(platform.setDpiScale(window, 2.0f));
    check(platform.resize(window, 400, 300));
    check(platform.setFocused(window, false));
    check(platform.requestClose(window));
    check(platform.advanceTime(0.25));
    check(platform.monotonicSeconds() == 0.25);

    check(platform.windowState(window, state));
    check(state.framebufferWidth == 800);
    check(state.framebufferHeight == 600);
    check(!state.focused);
    check(state.closeRequested);

    std::vector<platform::PlatformEvent> events;
    platform.pollEvents(events);
    check(events.size() == 4);
    check(events[0].type == platform::PlatformEventType::DpiChanged);
    check(events[1].type == platform::PlatformEventType::WindowResized);
    check(events[2].type == platform::PlatformEventType::FocusChanged);
    check(events[3].type == platform::PlatformEventType::QuitRequested);
    platform.pollEvents(events);
    check(events.empty());

    const auto old = window;
    check(platform.destroyWindow(window));
    check(!platform.windowState(old, state));
    check(!platform.destroyWindow(old));
    check(platform.liveWindowCount() == 0);

    check(platform.createWindow({"reuse", 320, 200, false, false}, window));
    check(window.index == old.index);
    check(window.generation != old.generation);
}

void testNullRenderDeviceValidation() {
    platform::NullPlatform platform;
    platform::WindowHandle window;
    check(platform.createWindow({"render", 640, 480, true, true}, window));

    render::NullRenderDevice device;
    check(device.deviceGeneration() == 1);
    check(!device.createBuffer({0, render::BufferUsage::Vertex}).valid());
    check(!device.createTexture({0, 1, render::TextureFormat::Rgba8}).valid());

    const auto vertex = device.createBuffer(
        {1024, render::BufferUsage::Vertex});
    const auto index = device.createBuffer(
        {256, render::BufferUsage::Index});
    const auto texture = device.createTexture(
        {64, 64, render::TextureFormat::Rgba8});
    const auto pipeline = device.createPipeline(
        {render::PrimitiveTopology::Triangles,
         render::BlendMode::Alpha, false, false});
    check(vertex.valid() && index.valid() &&
          texture.valid() && pipeline.valid());
    check(device.liveBufferCount() == 2);
    check(device.liveTextureCount() == 1);
    check(device.livePipelineCount() == 1);

    const render::FrameDescription frame{window, 640, 480};
    check(device.beginFrame(frame));
    check(!device.beginFrame(frame));

    render::DrawCommand draw;
    draw.pipeline = pipeline;
    draw.vertexBuffer = vertex;
    draw.indexBuffer = index;
    draw.texture = texture;
    draw.elementCount = 6;
    draw.instanceCount = 4;
    draw.sortKey = 10;
    check(device.submit(draw));

    auto invalidDraw = draw;
    invalidDraw.pipeline = {};
    check(!device.submit(invalidDraw));
    invalidDraw = draw;
    invalidDraw.elementCount = 0;
    check(!device.submit(invalidDraw));

    check(device.endFrame());
    check(!device.endFrame());
    check(device.submittedFrames() == 1);
    check(device.lastFrame().description.window == window);
    check(device.lastFrame().draws.size() == 1);
    check(device.lastFrame().draws.front().instanceCount == 4);

    check(device.destroyBuffer(vertex));
    check(!device.valid(vertex));
    check(!device.destroyBuffer(vertex));
    check(device.beginFrame(frame));
    check(!device.submit(draw));
    check(device.endFrame());

    const auto oldIndex = index;
    const auto oldTexture = texture;
    const auto oldPipeline = pipeline;
    device.reset();
    check(device.deviceGeneration() == 2);
    check(!device.valid(oldIndex));
    check(!device.valid(oldTexture));
    check(!device.valid(oldPipeline));
    check(device.liveBufferCount() == 0);
    check(device.liveTextureCount() == 0);
    check(device.livePipelineCount() == 0);
    check(!device.frameActive());
}

} // namespace

int main() {
    testNullPlatformLifecycle();
    testNullRenderDeviceValidation();
    std::cout << "platform render tests passed\n";
    return 0;
}
