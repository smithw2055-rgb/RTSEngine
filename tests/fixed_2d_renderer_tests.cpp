#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Assets/CookedContent.h>
#include <RTSEngine/Assets/Vfs.h>
#include <RTSEngine/Platform/NullPlatform.h>
#include <RTSEngine/Presentation/Fixed2DRenderer.h>
#include <RTSEngine/Presentation/PresentationAssetCache.h>
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

void writeCooked(assets::MemoryVfs& vfs,
                 const char* path,
                 assets::CookedAsset asset) {
    const auto bytes = assets::EncodeCookedAsset(std::move(asset));
    check(!bytes.empty());
    check(vfs.write(path, bytes));
}

void populateAssets(assets::MemoryVfs& vfs,
                    std::uint8_t value) {
    assets::Texture2DContent texture;
    texture.width = 2;
    texture.height = 1;
    texture.pixels.assign(8, value);
    writeCooked(vfs, "textures/1.rta",
                assets::CookedContentCodec::textureAsset(1, texture));

    assets::SpriteContent first;
    first.texture = {assets::AssetType::Texture2D, 1};
    first.width = 1;
    first.height = 1;
    first.worldWidthMilli = 1000;
    first.worldHeightMilli = 1000;
    writeCooked(vfs, "sprites/10.rta",
                assets::CookedContentCodec::spriteAsset(10, first));

    auto second = first;
    second.x = 1;
    second.worldWidthMilli = 2000;
    writeCooked(vfs, "sprites/11.rta",
                assets::CookedContentCodec::spriteAsset(11, second));
}

void configureManager(assets::AssetManager& manager) {
    check(manager.registerAsset(
        {{assets::AssetType::Texture2D, 1}, "textures/1.rta", 1}));
    check(manager.registerAsset(
        {{assets::AssetType::Sprite, 10}, "sprites/10.rta", 1}));
    check(manager.registerAsset(
        {{assets::AssetType::Sprite, 11}, "sprites/11.rta", 1}));
    check(manager.request({assets::AssetType::Sprite, 10}).valid());
    check(manager.request({assets::AssetType::Sprite, 11}).valid());
    check(manager.process() == 2);
}

presentation::RenderPacket makePacket() {
    presentation::RenderPacket packet;
    packet.currentTick = 4;
    packet.sprites = {
        {1, 10, 0, presentation::RenderLayer::WorldEntity,
         1.0f, 1.0f, 1.0f, 0,
         presentation::ViewLifecycle::Stable,
         render::BlendMode::Alpha},
        {2, 11, 0, presentation::RenderLayer::WorldEntity,
         3.0f, 1.0f, 1.0f, 0,
         presentation::ViewLifecycle::Stable,
         render::BlendMode::Alpha},
        {3, 10, 0, presentation::RenderLayer::ProjectileAndEffect,
         2.0f, 2.0f, 0.75f, 0,
         presentation::ViewLifecycle::Stable,
         render::BlendMode::Additive}
    };
    packet.worldUi.push_back(
        {1, presentation::WorldUiType::HealthBar,
         1.0f, 0.5f, 0.5f, 1.0f});
    presentation::RenderPacketBuilder::sort(packet);
    return packet;
}

void checkFrame(const render::NullRenderDevice& device) {
    const auto& draws = device.lastFrame().draws;
    check(draws.size() == 3);
    check(draws[0].pass == render::RenderPassKind::WorldEntity);
    check(draws[0].elementCount == 12);
    check(draws[1].pass == render::RenderPassKind::ProjectileAndEffect);
    check(draws[1].elementCount == 6);
    check(draws[2].pass == render::RenderPassKind::WorldUi);
    check(draws[2].elementCount == 6);
}

void testFixedPassRendererAndAssetCache() {
    assets::MemoryVfs vfs;
    populateAssets(vfs, 10u);
    assets::AssetManager manager(vfs, 4096);
    configureManager(manager);

    platform::NullPlatform platform;
    platform::WindowHandle window;
    check(platform.createWindow({"fixed-2d", 640, 360, true, true}, window));

    render::NullRenderDevice device;
    presentation::PresentationAssetCache cache(manager, device);
    presentation::Fixed2DRenderer renderer(device, cache, 16);
    const auto packet = makePacket();
    const presentation::Camera2D camera{2.0f, 1.0f, 8.0f, 4.0f, true};
    render::FrameDescription frame;
    frame.window = window;
    frame.framebufferWidth = 640;
    frame.framebufferHeight = 360;

    check(renderer.render(frame, packet, camera));
    check(renderer.stats().drawCalls == 3);
    check(renderer.stats().spriteQuads == 3);
    check(renderer.stats().worldUiQuads == 1);
    check(renderer.stats().unresolvedSprites == 0);
    check(renderer.stats().droppedQuads == 0);
    check(renderer.stats().vertexBytes == 16 * sizeof(presentation::SpriteVertex));
    check(renderer.stats().indexBytes == 24 * sizeof(std::uint32_t));
    check(cache.stats().residentTextures == 1);
    check(cache.stats().residentSprites == 2);
    check(cache.stats().textureUploads == 1);
    checkFrame(device);

    const auto firstTexture = device.lastFrame().draws[0].texture;
    check(firstTexture.valid());
    populateAssets(vfs, 77u);
    check(manager.hotReload({assets::AssetType::Texture2D, 1}));
    check(renderer.render(frame, packet, camera));
    const auto secondTexture = device.lastFrame().draws[0].texture;
    check(secondTexture.valid());
    check(secondTexture != firstTexture);
    check(cache.stats().textureRebuilds == 1);
    check(cache.stats().textureUploads == 2);
    checkFrame(device);

    device.reset();
    check(renderer.render(frame, packet, camera));
    check(device.deviceGeneration() == 2);
    check(cache.stats().textureUploads == 3);
    checkFrame(device);

    renderer.shutdown();
    cache.clear();
    check(device.liveBufferCount() == 0);
    check(device.liveTextureCount() == 0);
    check(device.livePipelineCount() == 0);
}

} // namespace

int main() {
    testFixedPassRendererAndAssetCache();
    std::cout << "fixed 2d renderer tests passed\n";
    return 0;
}
