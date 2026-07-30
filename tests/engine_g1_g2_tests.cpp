#include <RTSEngine/Audio/AudioMixer.h>
#include <RTSEngine/Navigation/NavigationWorld.h>
#include <RTSEngine/Presentation/AnimationController2D.h>
#include <RTSEngine/Presentation/FogOfWarSurface.h>
#include <RTSEngine/Presentation/VfxRuntime.h>
#include <RTSEngine/Rts/WorldMap.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

class RecordingAudioDevice final : public rts::audio::AudioDevice {
public:
    std::uint32_t deviceGeneration() const noexcept override {
        return generation_;
    }

    rts::audio::VoiceHandle play(
        const rts::audio::AudioPlayCommand& command) override {
        commands.push_back(command);
        return {++nextVoice_, generation_};
    }

    bool stop(rts::audio::VoiceHandle voice) override {
        stopped.push_back(voice);
        return voice.valid();
    }

    void stopAll() noexcept override { ++stopAllCalls; }

    void reset() noexcept override { ++generation_; }

    std::vector<rts::audio::AudioPlayCommand> commands;
    std::vector<rts::audio::VoiceHandle> stopped;
    std::uint32_t stopAllCalls{};

private:
    std::uint32_t generation_{1};
    std::uint32_t nextVoice_{};
};

rts::gameplay::WorldMapDefinition makeMap() {
    using namespace rts::gameplay;
    using namespace rts::navigation;
    WorldMapDefinition map;
    map.id = 0x1001;
    map.name = "g1-validation-map";
    map.width = 12;
    map.height = 8;
    map.cells.resize(96);
    for (std::int32_t x = 1; x < 11; ++x) {
        map.cells[static_cast<std::size_t>(3 * map.width + x)].terrainType = 1;
    }
    map.cells[static_cast<std::size_t>(3 * map.width + 6)]
        .staticBlockedMask = DomainBit(MovementDomain::Ground);
    map.spawns.push_back(
        {40, WorldSpawnKind::Unit, 7, 2, {10, 6}, 4});
    map.spawns.push_back(
        {10, WorldSpawnKind::Building, 3, 1, {1, 1}, 0});
    map.resources.push_back({20, 1, 1500, {2, 6}});
    map.routes.push_back({30, 1, {0, 0}, {11, 7}, true});
    map.lanes.push_back({50, {{0, 4}, {5, 4}, {11, 4}}});
    map.triggerZones.push_back({60, {4, 2}, {7, 5}, 0x9001});
    return map;
}

void testWorldMapAndLayeredNavigation() {
    using namespace rts::gameplay;
    using namespace rts::navigation;
    auto map = makeMap();
    assert(WorldMap::validate(map));

    std::vector<std::uint8_t> bytes;
    assert(WorldMap::encode(map, bytes));
    WorldMapDefinition decoded;
    assert(WorldMap::decode(bytes, decoded));
    assert(WorldMap::contentHash(map) == WorldMap::contentHash(decoded));
    assert(decoded.spawns.front().id == 10);

    rts::assets::CookedAsset cooked;
    assert(WorldMapAssetCodec::cook(decoded, cooked));
    WorldMapDefinition fromAsset;
    assert(WorldMapAssetCodec::decode(cooked, fromAsset));
    assert(fromAsset.id == decoded.id);

    WorldBootstrapPlan plan;
    assert(BuildWorldBootstrapPlan(fromAsset, plan));
    assert(plan.contentHash != 0);
    assert(plan.requiredRoutes.size() == 1);

    NavProfile ground;
    ground.id = 1;
    ground.terrainCosts[1] = 80;
    const auto path = WeightedGridPathfinder::find(
        plan.navigation, {0, 0}, {11, 7}, ground, 4096);
    assert(path.found);
    assert(!path.points.empty());
    assert(std::find(path.points.begin(), path.points.end(), GridPoint{6, 3}) ==
           path.points.end());

    NavProfile air;
    air.id = 2;
    air.domain = MovementDomain::Air;
    const auto airPath = WeightedGridPathfinder::find(
        plan.navigation, {0, 0}, {11, 7}, air, 4096);
    assert(airPath.found);
    assert(!plan.navigation.passable({6, 3}, ground));
    assert(plan.navigation.passable({6, 3}, air));

    NavigationRequestQueue requests;
    assert(requests.submit({2, 10, 1, {0, 0}, {11, 7}, 4096}));
    assert(requests.submit({1, 10, 2, {0, 0}, {11, 7}, 4096}));
    std::vector<NavProfile> profiles{ground, air};
    std::sort(profiles.begin(), profiles.end(),
              [](const NavProfile& a, const NavProfile& b) {
                  return a.id < b.id;
              });
    requests.solvePending(plan.navigation, profiles);
    const auto completions = requests.commitReady(plan.navigation, 10);
    assert(completions.size() == 2);
    assert(completions[0].request.requestId == 1);
    assert(completions[1].request.requestId == 2);

    const auto slots = FormationPlanner::assign(
        FormationKind::Box, {9, 3, 7, 3, 1});
    assert(slots.size() == 4);
    assert(slots.front().entityId == 1);

    const auto moved = FixedMover::advanceToward(
        FixedPosition2D::fromCell({0, 0}),
        FixedPosition2D::fromCell({1, 1}),
        FixedPosition2D::kOne / 4);
    assert(!moved.arrived);
    assert(moved.position.x > 0 && moved.position.y > 0);
}

void testAnimationFogAndVfx() {
    using namespace rts::presentation;
    AnimationSet2D animations;
    assert(animations.set({
        AnimationAction::Idle, Direction8::South, 100,
        AnimationAction::Idle, 0, true, true}));
    assert(animations.set({
        AnimationAction::Move, Direction8::East, 101,
        AnimationAction::Idle, 1, true, true}));
    assert(animations.set({
        AnimationAction::Attack, Direction8::East, 102,
        AnimationAction::Idle, 10, false, false}));

    Animator2D animator(42);
    assert(animator.setBaseState(
        animations, AnimationAction::Move, Direction8::East, 0));
    assert(animator.state().clipId == 101);
    assert(animator.play(
        animations, AnimationAction::Attack, Direction8::East, 50));
    assert(animator.state().clipId == 102);
    assert(animator.completeOneShot(animations, 120));
    assert(animator.state().clipId == 101);

    FogOfWarSurface fog(8, 8);
    std::vector<std::uint8_t> visible(64);
    visible[3 * 8 + 3] = 1;
    visible[3 * 8 + 4] = 1;
    assert(fog.updateVisibility(visible));
    assert(fog.state(3, 3) == FogCellState::Visible);
    std::fill(visible.begin(), visible.end(), 0);
    assert(fog.updateVisibility(visible));
    assert(fog.state(3, 3) == FogCellState::Explored);
    const auto dirty = fog.consumeDirtyRect();
    assert(dirty.valid());
    const auto alpha = fog.buildAlphaTexture();
    assert(alpha[3 * 8 + 3] > 0 && alpha[3 * 8 + 3] < 255);

    VfxRuntime vfx;
    VfxDefinition explosion;
    explosion.id = 200;
    explosion.spriteId = 300;
    explosion.burstParticles = 12;
    explosion.maximumParticles = 16;
    explosion.durationMilliseconds = 1000;
    explosion.particleLifetimeMilliseconds = 400;
    explosion.speed = 2.0f;
    explosion.gravity = 0.5f;
    assert(vfx.registerDefinition(explosion));
    assert(vfx.spawn({900, 200, 5.0f, 6.0f, 0.0f}, 0));
    assert(!vfx.spawn({900, 200, 5.0f, 6.0f, 0.0f}, 0));
    vfx.update(16, 16);
    std::vector<VfxParticleView> views;
    vfx.buildViews(views);
    assert(views.size() == 12);
    assert(views.front().spriteId == 300);
}

void testAudioMixer() {
    using namespace rts::audio;
    RecordingAudioDevice device;
    AudioMixer mixer(device, 2);
    mixer.setListener({0, 0, 10});
    mixer.setBusVolume(AudioBus::Sfx, 0.5f);

    MixedAudioPlayCommand low;
    low.command.eventId = 1;
    low.command.clipId = 10;
    low.command.volume = 1.0f;
    low.command.spatial = true;
    low.command.x = 2.0f;
    low.bus = AudioBus::Sfx;
    low.priority = 10;
    assert(mixer.play(low).valid());
    low.command.eventId = 2;
    assert(mixer.play(low).valid());

    MixedAudioPlayCommand high = low;
    high.command.eventId = 3;
    high.priority = 200;
    assert(mixer.play(high).valid());
    assert(mixer.stats().stolenVoices == 1);
    assert(device.stopped.size() == 1);
    assert(device.commands.front().volume < 0.5f);

    mixer.setBusMuted(AudioBus::Sfx, true);
    assert(!mixer.play(high).valid());
    assert(mixer.stats().rejectedVoices == 1);
}

} // namespace

int main() {
    testWorldMapAndLayeredNavigation();
    testAnimationFogAndVfx();
    testAudioMixer();
    std::cout << "Engine G1/G2 tests passed\n";
    return 0;
}
