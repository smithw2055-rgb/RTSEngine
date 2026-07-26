#include "SokolComposition.h"

#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_log.h>

#include <RTSEngine/Audio/NullAudioDevice.h>
#include <RTSEngine/Platform/SokolAppPlatform.h>
#include <RTSEngine/Presentation/SokolFixed2DShader.h>
#include <RTSEngine/Render/SokolRenderDevice.h>
#include <RTSEngine/RtsDesktop/PlayableDesktopRuntime.h>

#include <memory>

namespace rts::desktop_demo {
namespace {

struct AppState final {
    platform::SokolAppPlatform platform;
    audio::NullAudioDevice audio;
    std::unique_ptr<render::SokolRenderDevice> render;
    std::unique_ptr<rts_desktop::PlayableDesktopRuntime> runtime;
    bool ready{};
};

AppState state;

sg_environment environment(void*) { return sglue_environment(); }

sg_swapchain swapchain(void*, platform::WindowHandle) {
    return sglue_swapchain();
}

sg_shader_desc spriteShader(void*, render::ShaderKey shaderKey) {
    return presentation::MakeSokolFixed2DShaderDescription(shaderKey);
}

void initialize(void*) {
    render::SokolBackendCallbacks callbacks;
    callbacks.environment = environment;
    callbacks.swapchain = swapchain;
    callbacks.shaderDescription = spriteShader;
    callbacks.manageSokolLifecycle = true;
    state.render = std::make_unique<render::SokolRenderDevice>(callbacks);
    if (!state.render->valid()) return;
    state.runtime = std::make_unique<rts_desktop::PlayableDesktopRuntime>(
        state.platform, *state.render, state.audio);
    state.ready = state.runtime->initialize();
}

void frame(void*) {
    if (!state.ready || !state.runtime) {
        sapp_request_quit();
        return;
    }
    const auto result = state.runtime->advanceFrame();
    if (result.quitRequested) sapp_request_quit();
}

void cleanup(void*) {
    if (state.runtime) state.runtime->shutdown();
    state.runtime.reset();
    state.render.reset();
    state.ready = false;
}

void event(const sapp_event* value, void*) {
    if (value) state.platform.handleEvent(*value);
}

} // namespace

sapp_desc MakePlayableDesktopDescription() noexcept {
    sapp_desc description{};
    description.user_data = &state;
    description.init_userdata_cb = initialize;
    description.frame_userdata_cb = frame;
    description.cleanup_userdata_cb = cleanup;
    description.event_userdata_cb = event;
    description.width = 1280;
    description.height = 720;
    description.sample_count = 1;
    description.high_dpi = true;
    description.window_title = "RTSEngine Playable Desktop";
    description.enable_dragndrop = true;
    description.max_dropped_files = 8;
    description.logger.func = slog_func;
    return description;
}

} // namespace rts::desktop_demo
