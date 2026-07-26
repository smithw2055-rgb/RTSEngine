#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Assets/CookedContent.h>
#include <RTSEngine/Assets/Vfs.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace rts;

void check(bool condition) {
    assert(condition);
    if (!condition) std::abort();
}

void writeCooked(assets::MemoryVfs& vfs,
                 const std::string& path,
                 assets::CookedAsset asset) {
    const auto bytes = assets::EncodeCookedAsset(std::move(asset));
    check(!bytes.empty());
    check(vfs.write(path, bytes));
}

void testVirtualFileSystem() {
    std::string normalized;
    check(assets::NormalizeVirtualPath("content\\units//marine.rta", normalized));
    check(normalized == "content/units/marine.rta");
    check(!assets::NormalizeVirtualPath("../escape.rta", normalized));
    check(!assets::NormalizeVirtualPath("C:/escape.rta", normalized));
    check(!assets::NormalizeVirtualPath("/absolute.rta", normalized));

    auto root = std::make_shared<assets::MemoryVfs>();
    auto dlc = std::make_shared<assets::MemoryVfs>();
    check(root->write("common/value.bin", {1u}));
    check(dlc->write("value.bin", {2u}));

    assets::MountedVfs mounted;
    check(mounted.mount("", root));
    check(mounted.mount("dlc", dlc));
    std::vector<std::uint8_t> bytes;
    check(mounted.read("common/value.bin", bytes));
    check(bytes == std::vector<std::uint8_t>({1u}));
    check(mounted.read("dlc/value.bin", bytes));
    check(bytes == std::vector<std::uint8_t>({2u}));
    check(!mounted.read("dlc/../value.bin", bytes));
}

void registerPresentationAssets(assets::AssetManager& manager) {
    check(manager.registerAsset(
        {{assets::AssetType::Texture2D, 10}, "textures/10.rta", 1}));
    check(manager.registerAsset(
        {{assets::AssetType::Sprite, 20}, "sprites/20.rta", 1}));
    check(manager.registerAsset(
        {{assets::AssetType::Sprite, 21}, "sprites/21.rta", 1}));
    check(manager.registerAsset(
        {{assets::AssetType::AnimationClip, 30}, "animations/30.rta", 1}));
    check(manager.registerAsset(
        {{assets::AssetType::Effect, 40}, "effects/40.rta", 1}));
    check(manager.registerAsset(
        {{assets::AssetType::AudioClip, 50}, "audio/50.rta", 1}));
}

void populatePresentationAssets(assets::MemoryVfs& vfs,
                                std::uint8_t textureValue = 7u) {
    assets::Texture2DContent texture;
    texture.width = 2;
    texture.height = 2;
    texture.pixels.assign(16, textureValue);
    writeCooked(vfs, "textures/10.rta",
                assets::CookedContentCodec::textureAsset(10, texture));

    assets::SpriteContent first;
    first.texture = {assets::AssetType::Texture2D, 10};
    first.width = 1;
    first.height = 1;
    first.worldWidthMilli = 1000;
    first.worldHeightMilli = 1000;
    writeCooked(vfs, "sprites/20.rta",
                assets::CookedContentCodec::spriteAsset(20, first));

    auto second = first;
    second.x = 1;
    writeCooked(vfs, "sprites/21.rta",
                assets::CookedContentCodec::spriteAsset(21, second));

    assets::AnimationClipContent animation;
    animation.frames = {{20, 100}, {21, 100}};
    writeCooked(vfs, "animations/30.rta",
                assets::CookedContentCodec::animationAsset(30, animation));

    assets::EffectContent effect;
    effect.animationClipId = 30;
    effect.durationMilliseconds = 250;
    effect.additive = true;
    writeCooked(vfs, "effects/40.rta",
                assets::CookedContentCodec::effectAsset(40, effect));

    assets::AudioClipContent audio;
    audio.sampleRate = 22050;
    audio.channels = 1;
    audio.bitsPerSample = 16;
    audio.pcm = {0, 0, 1, 0};
    writeCooked(vfs, "audio/50.rta",
                assets::CookedContentCodec::audioAsset(50, audio));
}

void testDependencyLoadingCancellationAndReload() {
    assets::MemoryVfs vfs;
    populatePresentationAssets(vfs);
    assets::AssetManager manager(vfs, 4096);
    registerPresentationAssets(manager);

    const auto effectRequest = manager.request(
        {assets::AssetType::Effect, 40});
    check(effectRequest.valid());
    check(manager.process(1) == 1);

    assets::AssetRequestStatus status;
    check(manager.requestStatus(effectRequest, status));
    check(status.completed);
    check(status.state == assets::AssetState::Ready);
    check(status.failure == assets::AssetFailure::None);
    check(manager.state({assets::AssetType::Texture2D, 10}) ==
          assets::AssetState::Ready);
    check(manager.state({assets::AssetType::Sprite, 20}) ==
          assets::AssetState::Ready);
    check(manager.state({assets::AssetType::Sprite, 21}) ==
          assets::AssetState::Ready);
    check(manager.state({assets::AssetType::AnimationClip, 30}) ==
          assets::AssetState::Ready);
    check(manager.state({assets::AssetType::Effect, 40}) ==
          assets::AssetState::Ready);

    const auto audioRequest = manager.request(
        {assets::AssetType::AudioClip, 50});
    check(audioRequest.valid());
    check(manager.cancel(audioRequest));
    check(manager.process(1) == 1);
    check(manager.requestStatus(audioRequest, status));
    check(status.completed && status.cancelled);
    check(status.failure == assets::AssetFailure::Cancelled);

    const auto* before = manager.peek(
        {assets::AssetType::Texture2D, 10});
    check(before != nullptr);
    const auto oldGeneration = before->generation;
    populatePresentationAssets(vfs, 99u);
    check(manager.hotReload({assets::AssetType::Texture2D, 10}));
    const auto* after = manager.peek(
        {assets::AssetType::Texture2D, 10});
    check(after != nullptr);
    check(after->generation != oldGeneration);
    assets::Texture2DContent decoded;
    check(assets::CookedContentCodec::decodeTexture(
        after->cooked.payload, decoded));
    check(decoded.pixels.front() == 99u);
    check(manager.stats().hotReloads == 1);

    check(manager.releaseRequest(effectRequest));
    check(manager.releaseRequest(audioRequest));
}

assets::CookedAsset binaryAsset(std::uint64_t id,
                                std::size_t bytes,
                                std::vector<assets::AssetDependency> deps = {}) {
    assets::CookedAsset asset;
    asset.key = {assets::AssetType::Binary, id};
    asset.schemaVersion = 1;
    asset.dependencies = std::move(deps);
    asset.payload.assign(bytes, static_cast<std::uint8_t>(id));
    return asset;
}

void testBudgetEvictionAndDependencyCycle() {
    assets::MemoryVfs vfs;
    writeCooked(vfs, "binary/1.rta", binaryAsset(1, 8));
    writeCooked(vfs, "binary/2.rta", binaryAsset(2, 8));

    assets::AssetManager manager(vfs, 8);
    check(manager.registerAsset(
        {{assets::AssetType::Binary, 1}, "binary/1.rta", 1}));
    check(manager.registerAsset(
        {{assets::AssetType::Binary, 2}, "binary/2.rta", 1}));
    const auto first = manager.request({assets::AssetType::Binary, 1});
    check(manager.process() == 1);
    check(manager.state({assets::AssetType::Binary, 1}) ==
          assets::AssetState::Ready);
    const auto second = manager.request({assets::AssetType::Binary, 2});
    check(manager.process() == 1);
    check(manager.state({assets::AssetType::Binary, 1}) ==
          assets::AssetState::Unloaded);
    check(manager.state({assets::AssetType::Binary, 2}) ==
          assets::AssetState::Ready);
    check(manager.stats().evictedAssets == 1);
    check(manager.releaseRequest(first));
    check(manager.releaseRequest(second));

    assets::MemoryVfs cycleVfs;
    writeCooked(
        cycleVfs, "cycle/3.rta",
        binaryAsset(3, 1, {{{assets::AssetType::Binary, 4}, 1}}));
    writeCooked(
        cycleVfs, "cycle/4.rta",
        binaryAsset(4, 1, {{{assets::AssetType::Binary, 3}, 1}}));
    assets::AssetManager cycle(cycleVfs, 64);
    check(cycle.registerAsset(
        {{assets::AssetType::Binary, 3}, "cycle/3.rta", 1}));
    check(cycle.registerAsset(
        {{assets::AssetType::Binary, 4}, "cycle/4.rta", 1}));
    const auto request = cycle.request({assets::AssetType::Binary, 3});
    check(cycle.process() == 1);
    assets::AssetRequestStatus status;
    check(cycle.requestStatus(request, status));
    check(status.completed);
    check(status.state == assets::AssetState::Failed);
    check(status.failure == assets::AssetFailure::DependencyCycle);
}

} // namespace

int main() {
    testVirtualFileSystem();
    testDependencyLoadingCancellationAndReload();
    testBudgetEvictionAndDependencyCycle();
    std::cout << "assets runtime tests passed\n";
    return 0;
}
