#include <RTSEngine/Presentation/RenderPacket.h>

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using namespace rts;

void check(bool condition) {
    assert(condition);
    if (!condition) std::abort();
}

bool near(float a, float b) {
    return std::fabs(a - b) < 0.0001f;
}

presentation::PresentationEntity entity(
    presentation::ViewId id,
    float x,
    float y,
    presentation::LogicalAssetId sprite) {
    presentation::PresentationEntity result;
    result.viewId = id;
    result.sourceIndex = static_cast<std::uint32_t>(id);
    result.sourceGeneration = 1;
    result.kind = presentation::SceneEntityKind::Unit;
    result.definitionId = static_cast<std::uint32_t>(id);
    result.teamId = 1;
    result.x = x;
    result.y = y;
    result.healthCurrent = 100;
    result.healthMaximum = 100;
    result.visible = true;
    result.spriteAsset = sprite;
    result.layer = presentation::RenderLayer::WorldEntity;
    return result;
}

presentation::PresentationScene scene(
    std::uint64_t tick,
    std::initializer_list<presentation::PresentationEntity> entities) {
    presentation::PresentationScene result;
    result.tick = tick;
    result.simulationHash = tick * 10u;
    result.entities.assign(entities.begin(), entities.end());
    return result;
}

void testBufferedInterpolationLifecycle() {
    auto first = scene(
        10,
        {entity(1, 0.0f, 0.0f, 101),
         entity(2, 4.0f, 2.0f, 102)});
    first.entities[0].healthCurrent = 50;

    auto second = scene(
        11,
        {entity(1, 10.0f, 0.0f, 101),
         entity(3, 8.0f, 1.0f, 103)});
    second.entities[0].healthCurrent = 40;
    second.entities[1].kind = presentation::SceneEntityKind::Construction;
    second.entities[1].progressTicks = 2;
    second.entities[1].requiredTicks = 4;

    presentation::PresentationSceneBuffer buffer;
    check(buffer.publish(first));
    check(buffer.ready());
    check(buffer.previous().tick == 10);
    check(buffer.current().tick == 10);
    check(!buffer.publish(first));
    check(buffer.publish(second));

    auto duplicate = second;
    duplicate.tick = 11;
    check(!buffer.publish(duplicate));
    auto invalid = scene(
        12,
        {entity(2, 0.0f, 0.0f, 1),
         entity(1, 0.0f, 0.0f, 2)});
    check(!buffer.publish(invalid));

    const auto sampled = buffer.sample(
        0.5f, {20.0f, true});
    check(sampled.previousTick == 10);
    check(sampled.currentTick == 11);
    check(sampled.simulationHash == 110);
    check(near(sampled.alpha, 0.5f));
    check(sampled.entities.size() == 3);

    check(sampled.entities[0].current.viewId == 1);
    check(sampled.entities[0].lifecycle ==
          presentation::ViewLifecycle::Stable);
    check(near(sampled.entities[0].x, 5.0f));
    check(near(sampled.entities[0].opacity, 1.0f));

    check(sampled.entities[1].current.viewId == 2);
    check(sampled.entities[1].lifecycle ==
          presentation::ViewLifecycle::Despawned);
    check(near(sampled.entities[1].x, 4.0f));
    check(near(sampled.entities[1].opacity, 0.5f));

    check(sampled.entities[2].current.viewId == 3);
    check(sampled.entities[2].lifecycle ==
          presentation::ViewLifecycle::Spawned);
    check(near(sampled.entities[2].x, 8.0f));
    check(near(sampled.entities[2].opacity, 0.5f));

    const auto packet = presentation::RenderPacketBuilder::build(sampled);
    check(packet.sprites.size() == 3);
    check(packet.worldUi.size() == 2);
    check(packet.worldUi[0].viewId == 1);
    check(packet.worldUi[0].type == presentation::WorldUiType::HealthBar);
    check(near(packet.worldUi[0].value, 0.4f));
    check(packet.worldUi[1].viewId == 3);
    check(packet.worldUi[1].type ==
          presentation::WorldUiType::ConstructionProgress);
    check(near(packet.worldUi[1].value, 0.5f));
}

void testTeleportAndAlphaClamp() {
    const auto previous = scene(1, {entity(1, 0.0f, 0.0f, 1)});
    const auto current = scene(2, {entity(1, 10.0f, 0.0f, 1)});

    const auto teleported = presentation::PresentationSceneBuffer::Interpolate(
        previous, current, 0.25f, {1.0f, false});
    check(teleported.entities.size() == 1);
    check(near(teleported.entities.front().x, 10.0f));

    const auto clamped = presentation::PresentationSceneBuffer::Interpolate(
        previous, current, 2.0f, {20.0f, false});
    check(near(clamped.alpha, 1.0f));
    check(near(clamped.entities.front().x, 10.0f));

    const auto nanAlpha = presentation::PresentationSceneBuffer::Interpolate(
        previous, current, std::nanf(""), {20.0f, false});
    check(near(nanAlpha.alpha, 0.0f));
    check(near(nanAlpha.entities.front().x, 0.0f));
}

void testRenderPacketSortOrder() {
    presentation::InterpolatedScene scene;
    scene.currentTick = 1;

    auto high = entity(3, 0.0f, 4.0f, 3);
    high.layer = presentation::RenderLayer::WorldUi;
    auto later = entity(2, 0.0f, 3.0f, 2);
    later.sortBias = 1;
    auto earlier = entity(1, 0.0f, 3.0f, 1);
    earlier.sortBias = -1;
    scene.entities = {
        {high, high.x, high.y, 1.0f, presentation::ViewLifecycle::Stable},
        {later, later.x, later.y, 1.0f, presentation::ViewLifecycle::Stable},
        {earlier, earlier.x, earlier.y, 1.0f, presentation::ViewLifecycle::Stable}
    };

    const auto packet = presentation::RenderPacketBuilder::build(scene);
    check(packet.sprites.size() == 3);
    check(packet.sprites[0].viewId == 1);
    check(packet.sprites[1].viewId == 2);
    check(packet.sprites[2].viewId == 3);
}

} // namespace

int main() {
    testBufferedInterpolationLifecycle();
    testTeleportAndAlphaClamp();
    testRenderPacketSortOrder();
    std::cout << "snapshot interpolation tests passed\n";
    return 0;
}
