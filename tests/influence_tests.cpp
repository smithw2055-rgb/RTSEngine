#include <RTSEngine/Rts/Influence.h>
#include <RTSEngine/Rts/SimulationArchive.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            std::cerr << "CHECK failed: " #expression \
                      << " at line " << __LINE__ << '\n'; \
            return EXIT_FAILURE; \
        } \
    } while (false)

using namespace rts::gameplay;

rts::ecs::Entity createCombatEntity(
    rts::ecs::World& world,
    GridPoint point,
    std::uint32_t teamId,
    std::int32_t visionRange,
    std::int32_t health,
    std::int32_t armor,
    std::int32_t damage,
    std::int32_t range,
    std::uint32_t cooldown) {
    const auto entity = world.create();
    world.emplace<Position>(entity, Position{point.x, point.y});
    world.emplace<Team>(entity, Team{teamId});
    world.emplace<VisionSource>(entity, VisionSource{visionRange});
    world.emplace<Health>(entity, Health{health, health});
    world.emplace<Armor>(entity, Armor{armor});
    if (damage > 0) {
        world.emplace<Weapon>(
            entity,
            Weapon{damage, range, std::max<std::uint32_t>(1, cooldown), 0});
    }
    return entity;
}

const TeamInfluenceLayer* findLayer(
    const InfluenceRuntime& influence,
    std::uint32_t teamId) {
    const auto found = std::lower_bound(
        influence.layers().begin(),
        influence.layers().end(),
        teamId,
        [](const TeamInfluenceLayer& layer, std::uint32_t value) {
            return layer.teamId < value;
        });
    return found != influence.layers().end() && found->teamId == teamId
        ? &*found
        : nullptr;
}

int testVisibleThreatAndFalloff() {
    NavigationGrid navigation(16, 8);
    rts::ecs::World world;
    const auto observer = createCombatEntity(
        world, {2, 3}, 1, 9, 100, 1, 12, 2, 2);
    const auto enemy = createCombatEntity(
        world, {8, 3}, 2, 2, 80, 2, 20, 3, 4);
    createCombatEntity(
        world, {15, 7}, 2, 1, 60, 0, 30, 2, 2);

    VisionRuntime vision(16, 8);
    vision.rebuild(world, navigation);
    CHECK(vision.visible(1, {8, 3}));
    CHECK(!vision.visible(1, {15, 7}));

    InfluenceRuntime influence(16, 8);
    influence.rebuild(world, vision);
    CHECK(influence.layerCount() == 2u);
    CHECK(influence.layers()[0].teamId == 1u);
    CHECK(influence.layers()[1].teamId == 2u);
    CHECK(influence.friendly(1, {2, 3}) > 0);
    CHECK(influence.threat(1, {8, 3}) > 0);
    CHECK(influence.threat(1, {8, 3}) > influence.threat(1, {11, 3}));
    CHECK(influence.threat(1, {15, 7}) == 0);
    CHECK(influence.threat(2, {2, 3}) == 0);
    CHECK(influence.net(1, {8, 3}) ==
          influence.friendly(1, {8, 3}) -
              influence.threat(1, {8, 3}));

    const auto* teamOne = findLayer(influence, 1);
    CHECK(teamOne != nullptr);
    CHECK(teamOne->threatActiveCells > 0u);
    CHECK(teamOne->peakThreat == influence.threat(1, {8, 3}));
    CHECK(teamOne->minimumNet < 0);

    auto* observerPosition = world.try_get<Position>(observer);
    CHECK(observerPosition != nullptr);
    observerPosition->x = 0;
    observerPosition->y = 0;
    auto* observerVision = world.try_get<VisionSource>(observer);
    CHECK(observerVision != nullptr);
    observerVision->range = 2;
    vision.rebuild(world, navigation);
    CHECK(vision.explored(1, {8, 3}));
    CHECK(!vision.visible(1, {8, 3}));
    influence.rebuild(world, vision);
    CHECK(influence.threat(1, {8, 3}) == 0);

    auto* enemyHealth = world.try_get<Health>(enemy);
    CHECK(enemyHealth != nullptr);
    enemyHealth->current = 20;
    observerPosition->x = 2;
    observerPosition->y = 3;
    observerVision->range = 9;
    vision.rebuild(world, navigation);
    influence.rebuild(world, vision);
    const auto weakenedThreat = influence.threat(1, {8, 3});
    enemyHealth->current = 80;
    influence.rebuild(world, vision);
    CHECK(influence.threat(1, {8, 3}) > weakenedThreat);
    return EXIT_SUCCESS;
}

int testOverlappingSourcesAndFootprintVisibility() {
    NavigationGrid navigation(14, 8);
    rts::ecs::World world;
    createCombatEntity(world, {4, 3}, 1, 3, 50, 0, 8, 2, 2);
    createCombatEntity(world, {4, 4}, 1, 3, 50, 0, 8, 2, 2);

    const auto building = createCombatEntity(
        world, {7, 2}, 2, 1, 200, 4, 25, 4, 3);
    world.emplace<BuildingFootprint>(
        building, BuildingFootprint{{7, 2}, 3, 3});
    world.emplace<Building>(building, Building{1, false});

    VisionRuntime vision(14, 8);
    vision.rebuild(world, navigation);
    CHECK(!vision.visible(1, {7, 2}));
    CHECK(vision.visible(1, {7, 3}));

    InfluenceRuntime influence(14, 8);
    influence.rebuild(world, vision);
    CHECK(influence.threat(1, {8, 3}) > 0);

    const auto oneFriendly = influence.friendly(1, {4, 3});
    const auto ally = world.create();
    world.emplace<Position>(ally, Position{4, 3});
    world.emplace<Team>(ally, Team{1});
    world.emplace<VisionSource>(ally, VisionSource{1});
    world.emplace<Health>(ally, Health{50, 50});
    world.emplace<Armor>(ally, Armor{0});
    world.emplace<Weapon>(ally, Weapon{8, 2, 2, 0});
    vision.rebuild(world, navigation);
    influence.rebuild(world, vision);
    CHECK(influence.friendly(1, {4, 3}) > oneFriendly);

    const auto firstLayers = influence.layers();
    influence.rebuild(world, vision);
    CHECK(influence.layers().size() == firstLayers.size());
    for (std::size_t i = 0; i < firstLayers.size(); ++i) {
        CHECK(influence.layers()[i].teamId == firstLayers[i].teamId);
        CHECK(influence.layers()[i].friendly == firstLayers[i].friendly);
        CHECK(influence.layers()[i].threat == firstLayers[i].threat);
        CHECK(influence.layers()[i].net == firstLayers[i].net);
    }
    return EXIT_SUCCESS;
}

CombatStats combat(
    std::int32_t health,
    std::int32_t armor,
    std::int32_t damage,
    std::int32_t range,
    std::uint32_t cooldown) {
    CombatStats value;
    value.maximumHealth = health;
    value.armor = armor;
    value.weaponDamage = damage;
    value.weaponRange = range;
    value.cooldownTicks = cooldown;
    return value;
}

int testSimulationSnapshotAndPersistence() {
    RtsSimulation original(14, 8);
    original.createUnit(
        {2, 3}, {0}, 1, combat(100, 1, 10, 2, 2), 10);
    original.createUnit(
        {9, 3}, {0}, 2, combat(120, 2, 18, 3, 3), 4);
    original.step(0);

    CHECK(original.influence().layerCount() == 2u);
    CHECK(original.influence().threat(1, {9, 3}) > 0);
    CHECK(original.snapshot().influenceWidth == 14);
    CHECK(original.snapshot().influenceHeight == 8);
    CHECK(original.snapshot().influence.size() == 2u);
    CHECK(original.snapshot().influence[0].teamId == 1u);
    CHECK(original.snapshot().influence[0].threat.size() == 14u * 8u);
    CHECK(original.snapshot().influence[0].net.size() == 14u * 8u);

    const auto savedHash = original.snapshot().worldHash;
    const auto originalInfluence = original.snapshot().influence;
    const auto archive = EncodeRtsSimulation(original);
    CHECK(!archive.empty());

    RtsSimulation restored(14, 8);
    CHECK(DecodeRtsSimulation(archive, restored));
    CHECK(restored.snapshot().worldHash == savedHash);
    CHECK(restored.influence().layerCount() == 2u);
    CHECK(restored.snapshot().influence.size() == originalInfluence.size());
    for (std::size_t i = 0; i < originalInfluence.size(); ++i) {
        CHECK(restored.snapshot().influence[i].teamId ==
              originalInfluence[i].teamId);
        CHECK(restored.snapshot().influence[i].friendly ==
              originalInfluence[i].friendly);
        CHECK(restored.snapshot().influence[i].threat ==
              originalInfluence[i].threat);
        CHECK(restored.snapshot().influence[i].net ==
              originalInfluence[i].net);
    }
    CHECK(EncodeRtsSimulation(restored) == archive);

    for (std::uint64_t tick = 1; tick < 6; ++tick) {
        original.step(tick);
        restored.step(tick);
        CHECK(restored.snapshot().worldHash == original.snapshot().worldHash);
        CHECK(restored.snapshot().influence.size() ==
              original.snapshot().influence.size());
        for (std::size_t i = 0;
             i < original.snapshot().influence.size();
             ++i) {
            CHECK(restored.snapshot().influence[i].friendly ==
                  original.snapshot().influence[i].friendly);
            CHECK(restored.snapshot().influence[i].threat ==
                  original.snapshot().influence[i].threat);
            CHECK(restored.snapshot().influence[i].net ==
                  original.snapshot().influence[i].net);
        }
    }
    return EXIT_SUCCESS;
}

std::vector<std::uint64_t> deterministicHashes() {
    RtsSimulation simulation(12, 7);
    simulation.createUnit(
        {2, 2}, {1}, 1, combat(80, 1, 9, 2, 2), 8);
    simulation.createUnit(
        {9, 4}, {1}, 2, combat(90, 0, 12, 3, 3), 6);
    std::vector<std::uint64_t> hashes;
    for (std::uint64_t tick = 0; tick < 8; ++tick) {
        simulation.step(tick);
        hashes.push_back(simulation.snapshot().worldHash);
    }
    return hashes;
}

int testDeterministicHashes() {
    CHECK(deterministicHashes() == deterministicHashes());
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    CHECK(testVisibleThreatAndFalloff() == EXIT_SUCCESS);
    CHECK(testOverlappingSourcesAndFootprintVisibility() == EXIT_SUCCESS);
    CHECK(testSimulationSnapshotAndPersistence() == EXIT_SUCCESS);
    CHECK(testDeterministicHashes() == EXIT_SUCCESS);
    std::cout << "influence map tests passed\n";
    return EXIT_SUCCESS;
}
