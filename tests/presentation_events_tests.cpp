#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Assets/CookedContent.h>
#include <RTSEngine/Assets/Vfs.h>
#include <RTSEngine/Audio/NullAudioDevice.h>
#include <RTSEngine/Presentation/PresentationPlayback.h>
#include <RTSEngine/RtsPresentation/RtsPresentationRuntime.h>

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

void populateAssets(assets::MemoryVfs& vfs) {
    assets::Texture2DContent texture;
    texture.width = 2;
    texture.height = 1;
    texture.pixels.assign(8, 200u);
    writeCooked(vfs, "textures/10.rta",
                assets::CookedContentCodec::textureAsset(10, texture));

    assets::SpriteContent first;
    first.texture = {assets::AssetType::Texture2D, 10};
    first.width = 1;
    first.height = 1;
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

void loadAssets(assets::AssetManager& manager) {
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
    check(manager.request({assets::AssetType::Effect, 40}).valid());
    check(manager.request({assets::AssetType::AudioClip, 50}).valid());
    check(manager.process() == 2);
}

gameplay::WorldSnapshot makeSnapshot(std::uint64_t tick,
                                     std::int32_t x) {
    gameplay::WorldSnapshot snapshot;
    snapshot.tick = tick;
    snapshot.worldHash = tick * 100;
    gameplay::SnapshotEntity entity;
    entity.entity = {1, 1};
    entity.kind = gameplay::SnapshotKind::Unit;
    entity.definitionId = 7;
    entity.teamId = 1;
    entity.x = x;
    entity.y = 2;
    entity.healthCurrent = 100;
    entity.healthMaximum = 100;
    snapshot.entities.push_back(entity);
    return snapshot;
}

gameplay::DomainEvent makeWeaponEvent() {
    gameplay::DomainEvent event;
    event.tick = 5;
    event.type = gameplay::DomainEventType::WeaponFired;
    event.entity = {1, 1};
    event.objectId = 9;
    event.value = 12;
    return event;
}

void testStableCueExtractionAndPlayback() {
    assets::MemoryVfs vfs;
    populateAssets(vfs);
    assets::AssetManager manager(vfs, 4096);
    loadAssets(manager);
    audio::NullAudioDevice audio;
    presentation::PresentationPlaybackRuntime playback(manager, audio);

    rts_presentation::RtsPresentationRuntime runtime;
    check(runtime.registerVisual(
        {{presentation::SceneEntityKind::Unit, 7},
         20, 30, presentation::RenderLayer::WorldEntity, 0}));
    const auto eventType = rts_presentation::RtsEventTypeCode(
        gameplay::DomainEventType::WeaponFired);
    check(runtime.registerEventBinding(
        {{static_cast<std::uint32_t>(
              presentation::PresentationEventDomain::Rts),
          eventType, 0},
         30, 40, 50, 0.75f, true}));

    const auto weapon = makeWeaponEvent();
    check(runtime.publishSnapshot(makeSnapshot(5, 2), {weapon}));
    check(runtime.pendingEventCount() == 1);
    const auto firstCues = runtime.consumeCues();
    check(firstCues.animations.size() == 1);
    check(firstCues.effects.size() == 1);
    check(firstCues.audio.size() == 1);
    check(firstCues.animations.front().viewId ==
          presentation::MakeViewId(1, 1));
    check(firstCues.effects.front().x == 2.0f);
    check(firstCues.effects.front().y == 2.0f);

    playback.consume(firstCues, 1000);
    check(audio.recordedPlays().size() == 1);
    check(audio.recordedPlays().front().command.clipId == 50);
    check(audio.recordedPlays().front().command.volume == 0.75f);

    auto packet = runtime.buildRenderPacket(1.0f);
    playback.apply(packet, 1050);
    check(packet.sprites.size() == 2);
    check(packet.sprites[0].viewId == presentation::MakeViewId(1, 1));
    check(packet.sprites[0].spriteAsset == 20);
    check(packet.sprites[1].layer ==
          presentation::RenderLayer::ProjectileAndEffect);
    check(packet.sprites[1].spriteAsset == 20);
    check(packet.sprites[1].blend == render::BlendMode::Additive);

    packet = runtime.buildRenderPacket(1.0f);
    playback.apply(packet, 1150);
    check(packet.sprites.size() == 2);
    check(packet.sprites[0].spriteAsset == 21);
    check(packet.sprites[1].spriteAsset == 21);

    check(runtime.publishSnapshot(makeSnapshot(6, 3), {weapon}));
    const auto replayedCues = runtime.consumeCues();
    check(replayedCues.animations.empty());
    check(replayedCues.effects.empty());
    check(replayedCues.audio.empty());
    check(audio.recordedPlays().size() == 1);

    packet = runtime.buildRenderPacket(1.0f);
    playback.apply(packet, 1300);
    check(packet.sprites.size() == 1);
    check(playback.stats().activeEffects == 0);
    check(playback.stats().activeAnimations == 1);
    check(playback.stats().animationCues == 1);
    check(playback.stats().effectCues == 1);
    check(playback.stats().audioCues == 1);
    check(playback.stats().droppedCues == 0);

    runtime.clearEventHistory();
    playback.clear();
    audio.stopAll();
    check(playback.stats().activeAnimations == 0);
    check(audio.activeVoiceCount() == 0);
}

void testPresentationEventIdIncludesPayload() {
    const auto first = presentation::MakePresentationEventId(
        1, 4, 2, 3, 10, 11, 12, 13);
    const auto same = presentation::MakePresentationEventId(
        1, 4, 2, 3, 10, 11, 12, 13);
    const auto changed = presentation::MakePresentationEventId(
        1, 4, 2, 3, 10, 11, 12, 14);
    check(first != 0);
    check(first == same);
    check(first != changed);
}

} // namespace

int main() {
    testStableCueExtractionAndPlayback();
    testPresentationEventIdIncludesPayload();
    std::cout << "presentation events tests passed\n";
    return 0;
}
