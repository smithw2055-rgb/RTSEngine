#include <RTSEngine/Platform/NullPlatform.h>
#include <RTSEngine/Presentation/Fixed2DRenderer.h>
#include <RTSEngine/Presentation/ScreenUi.h>
#include <RTSEngine/Render/NullRenderDevice.h>

#include <cassert>
#include <cstdlib>
#include <iostream>

namespace {

using namespace rts;

void check(bool value) {
    assert(value);
    if (!value) std::abort();
}

class EmptyResolver final : public presentation::SpriteResolver {
public:
    bool resolve(presentation::LogicalAssetId,
                 presentation::ResolvedSprite&) override {
        return false;
    }
};

void testBuiltinUiAndBatching() {
    platform::NullPlatform platform;
    platform::WindowHandle window;
    check(platform.createWindow({"ui", 800, 450, true, true}, window));

    render::NullRenderDevice device;
    EmptyResolver resolver;
    presentation::Fixed2DRenderer renderer(device, resolver, 512);
    check(renderer.initialize());

    presentation::MinimalUi ui(device);
    check(ui.initialize());
    ui.begin(800, 450, renderer.whiteTexture(),
             {120.0f, 92.0f, false, true, false});
    ui.panel({16.0f, 16.0f, 260.0f, 120.0f});
    ui.label("RTS ENGINE", 28.0f, 28.0f, 1.0f);
    check(!ui.button(7, {80.0f, 72.0f, 100.0f, 28.0f}, "START"));
    ui.progressBar({28.0f, 110.0f, 220.0f, 10.0f}, 0.65f);
    check(ui.drawList().quads.size() > 20);

    presentation::RenderPacket packet;
    presentation::Camera2D camera{0.0f, 0.0f, 32.0f, 18.0f, true};
    render::FrameDescription frame;
    frame.window = window;
    frame.framebufferWidth = 800;
    frame.framebufferHeight = 450;

    check(renderer.render(frame, packet, camera, &ui.drawList()));
    check(renderer.stats().screenUiQuads == ui.drawList().quads.size());
    check(renderer.stats().drawCalls >= 2);
    check(!device.lastFrame().draws.empty());
    for (const auto& draw : device.lastFrame().draws) {
        check(draw.pass == render::RenderPassKind::ScreenUi);
    }

    ui.begin(800, 450, renderer.whiteTexture(),
             {120.0f, 92.0f, false, false, true});
    check(ui.button(7, {80.0f, 72.0f, 100.0f, 28.0f}, "START"));

    ui.shutdown();
    renderer.shutdown();
    check(device.liveTextureCount() == 0);
}

} // namespace

int main() {
    testBuiltinUiAndBatching();
    std::cout << "screen ui tests passed\n";
    return 0;
}
