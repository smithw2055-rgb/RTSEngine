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

gameplay::SnapshotEntity makeEntity(
    std::uint32_t index,
    gameplay::SnapshotKind kind,
    std::uint32_t definition,
    std::uint32_t team,
    std::int32_t x,
    std::int32_t y) {
    gameplay::SnapshotEntity result;
    result.entity = {index, 1};
    result.kind = kind;
    result.definitionId = definition;
    result.teamId = team;
    result.x = x;
    result.y = y;
    result.healthCurrent = 100;
    result.healthMaximum = 100;
    return result;
}

void testSceneExtractionAndVisibility() {
    presentation::VisualCatalog catalog;
    check(catalog.upsert(
        {{presentation::SceneEntityKind::Unit, 1},
         101, 201, presentation::RenderLayer::WorldEntity, 0}));
    check(catalog.upsert(
        {{presentation::SceneEntityKind::Building, 2},
         102, 0, presentation::RenderLayer::WorldEntity, -2}));
    check(!catalog.upsert(
        {{presentation::SceneEntityKind::Unit, 0},
         1, 0, presentation::RenderLayer::WorldEntity, 0}));

    gameplay::WorldSnapshot snapshot;
    snapshot.tick = 12;
    snapshot.worldHash = 0xabcdu;
    snapshot.resources.available = 77;
    snapshot.visibilityWidth = 4;
    snapshot.visibilityHeight = 3;

    gameplay::TeamVisibilitySnapshot visibility;
    visibility.teamId = 1;
    visibility.current.assign(12, 0);
    visibility.explored.assign(12, 1);
    visibility.current[5] = 1;
    snapshot.visibility.push_back(visibility);

    auto hiddenEnemy = makeEntity(
        3, gameplay::SnapshotKind::Unit, 1, 2, 3, 2);
    auto friendly = makeEntity(
        1, gameplay::SnapshotKind::Unit, 1, 1, 0, 0);
    friendly.healthCurrent = 50;
    auto visibleEnemy = makeEntity(
        2, gameplay::SnapshotKind::Building, 2, 2, 1, 1);
    visibleEnemy.progressTicks = 2;
    visibleEnemy.requiredTicks = 4;
    snapshot.entities = {hiddenEnemy, friendly, visibleEnemy};

    const auto scene = rts_presentation::RtsPresentationExtractor::extract(
        snapshot, catalog, {1, true, true});
    check(presentation::IsCanonicalScene(scene));
    check(scene.tick == 12);
    check(scene.simulationHash == 0xabcdu);
    check(scene.availableResources == 77);
    check(scene.observedTeam == 1);
    check(scene.visibility.width == 4);
    check(scene.visibility.height == 3);
    check(scene.visibility.current == visibility.current);
    check(scene.entities.size() == 3);

    check(scene.entities[0].viewId == presentation::MakeViewId(1, 1));
    check(scene.entities[0].visible);
    check(scene.entities[0].spriteAsset == 101);
    check(scene.entities[0].animationAsset == 201);

    check(scene.entities[1].viewId == presentation::MakeViewId(2, 1));
    check(scene.entities[1].visible);
    check(scene.entities[1].spriteAsset == 102);
    check(scene.entities[1].sortBias == -2);

    check(scene.entities[2].viewId == presentation::MakeViewId(3, 1));
    check(!scene.entities[2].visible);

    presentation::PresentationSceneBuffer buffer;
    check(buffer.publish(scene));
    const auto interpolated = buffer.sample(0.5f);
    const auto packet = presentation::RenderPacketBuilder::build(interpolated);
    check(packet.currentTick == 12);
    check(packet.simulationHash == 0xabcdu);
    check(packet.sprites.size() == 2);
    check(packet.worldUi.size() == 2);
    check(packet.worldUi[0].viewId == presentation::MakeViewId(1, 1));
    check(packet.worldUi[0].type == presentation::WorldUiType::HealthBar);
    check(packet.worldUi[0].value == 0.5f);
    check(packet.worldUi[1].viewId == presentation::MakeViewId(2, 1));
    check(packet.worldUi[1].type ==
          presentation::WorldUiType::ConstructionProgress);
    check(packet.worldUi[1].value == 0.5f);
}

void testMissingVisibilityHidesEnemies() {
    gameplay::WorldSnapshot snapshot;
    snapshot.tick = 1;
    snapshot.worldHash = 2;
    snapshot.entities.push_back(makeEntity(
        1, gameplay::SnapshotKind::Unit, 9, 2, 0, 0));

    presentation::VisualCatalog catalog;
    check(catalog.upsert(
        {{presentation::SceneEntityKind::Unit, 9},
         900, 0, presentation::RenderLayer::WorldEntity, 0}));

    const auto hidden = rts_presentation::RtsPresentationExtractor::extract(
        snapshot, catalog, {1, true, true});
    check(hidden.entities.size() == 1);
    check(!hidden.entities.front().visible);

    const auto shown = rts_presentation::RtsPresentationExtractor::extract(
        snapshot, catalog, {1, false, true});
    check(shown.entities.front().visible);
}

void testComposedRuntime() {
    rts_presentation::RtsPresentationRuntime runtime({1, false, true});
    check(runtime.registerVisual(
        {{presentation::SceneEntityKind::Unit, 1},
         700, 701, presentation::RenderLayer::WorldEntity, 0}));

    gameplay::WorldSnapshot first;
    first.tick = 1;
    first.worldHash = 10;
    first.entities.push_back(makeEntity(
        1, gameplay::SnapshotKind::Unit, 1, 1, 0, 0));
    check(runtime.publishSnapshot(first));

    auto second = first;
    second.tick = 2;
    second.worldHash = 20;
    second.entities.front().x = 2;
    check(runtime.publishSnapshot(second));

    const auto packet = runtime.buildRenderPacket(
        0.5f, {8.0f, true});
    check(packet.previousTick == 1);
    check(packet.currentTick == 2);
    check(packet.simulationHash == 20);
    check(packet.sprites.size() == 1);
    check(packet.sprites.front().spriteAsset == 700);
    check(packet.sprites.front().animationAsset == 701);
    check(packet.sprites.front().x == 1.0f);
}

} // namespace

int main() {
    testSceneExtractionAndVisibility();
    testMissingVisibilityHidesEnemies();
    testComposedRuntime();
    std::cout << "presentation scene tests passed\n";
    return 0;
}
