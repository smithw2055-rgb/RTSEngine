#include <RTSEngine/Rts/SimulationArchive.h>
#include <RTSEngine/Rts/Vision.h>

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

using namespace rts;
using namespace rts::gameplay;

std::uint32_t countVisible(
    const TeamVisibilityLayer& layer,
    bool explored) {
    const auto& values = explored ? layer.explored : layer.current;
    return static_cast<std::uint32_t>(std::count(
        values.begin(), values.end(), static_cast<std::uint8_t>(1u)));
}

int testLineOfSightAndTeams() {
    NavigationGrid navigation(12, 7);
    CHECK(navigation.setBlocked({5, 3}, true));

    ecs::World world;
    const auto scout = world.create();
    world.emplace<Position>(scout, Position{2, 3});
    world.emplace<Team>(scout, Team{1});
    world.emplace<VisionSource>(scout, VisionSource{6});

    const auto ally = world.create();
    world.emplace<Position>(ally, Position{2, 4});
    world.emplace<Team>(ally, Team{1});
    world.emplace<VisionSource>(ally, VisionSource{2});

    const auto enemy = world.create();
    world.emplace<Position>(enemy, Position{10, 3});
    world.emplace<Team>(enemy, Team{2});
    world.emplace<VisionSource>(enemy, VisionSource{2});

    VisionRuntime vision(12, 7);
    vision.rebuild(world, navigation);

    CHECK(vision.layerCount() == 2u);
    CHECK(vision.visible(1, {2, 3}));
    CHECK(vision.visible(1, {5, 3}));
    CHECK(!vision.visible(1, {6, 3}));
    CHECK(!vision.visible(1, {10, 3}));
    CHECK(vision.visible(2, {10, 3}));
    CHECK(!vision.visible(2, {2, 3}));

    const auto& layers = vision.layers();
    CHECK(layers[0].teamId == 1u);
    CHECK(layers[1].teamId == 2u);
    CHECK(layers[0].currentVisibleCells == countVisible(layers[0], false));
    CHECK(layers[0].exploredCells == countVisible(layers[0], true));
    CHECK(layers[1].currentVisibleCells == countVisible(layers[1], false));
    CHECK(layers[1].exploredCells == countVisible(layers[1], true));

    const auto byteGridCells =
        static_cast<std::size_t>(12 * 7) * vision.layerCount() * 2u;
    CHECK(vision.packedWordCount() * sizeof(std::uint64_t) < byteGridCells);
    CHECK(vision.offsetSetCount() == 2u);
    const auto offsetCapacity = vision.offsetPointCapacity();
    for (std::uint32_t iteration = 0; iteration < 32; ++iteration) {
        vision.rebuild(world, navigation);
        CHECK(vision.offsetSetCount() == 2u);
        CHECK(vision.offsetPointCapacity() == offsetCapacity);
        CHECK(vision.layers()[0].currentVisibleCells ==
              countVisible(vision.layers()[0], false));
    }
    return EXIT_SUCCESS;
}

int testExploredHistoryAndBinaryState() {
    NavigationGrid navigation(12, 6);
    ecs::World world;
    const auto scout = world.create();
    world.emplace<Position>(scout, Position{2, 2});
    world.emplace<Team>(scout, Team{7});
    world.emplace<VisionSource>(scout, VisionSource{2});

    VisionRuntime vision(12, 6);
    vision.rebuild(world, navigation);
    CHECK(vision.visible(7, {4, 2}));
    CHECK(vision.explored(7, {4, 2}));
    const auto firstExplored = vision.exploredCount(7);

    auto* position = world.try_get<Position>(scout);
    CHECK(position != nullptr);
    position->x = 9;
    position->y = 2;
    vision.rebuild(world, navigation);

    CHECK(!vision.visible(7, {4, 2}));
    CHECK(vision.explored(7, {4, 2}));
    CHECK(vision.visible(7, {11, 2}));
    CHECK(vision.exploredCount(7) > firstExplored);

    foundation::BinaryWriter writer;
    CHECK(vision.writeExploredState(writer));
    foundation::BinaryReader reader(writer.bytes());
    VisionRuntime restored(12, 6);
    CHECK(VisionRuntime::readExploredState(reader, 12, 6, restored));
    CHECK(reader.atEnd());
    CHECK(restored.layerCount() == 1u);
    CHECK(restored.explored(7, {4, 2}));
    CHECK(restored.explored(7, {11, 2}));
    CHECK(!restored.visible(7, {11, 2}));
    CHECK(restored.exploredCount(7) == vision.exploredCount(7));
    CHECK(restored.packedWordCount() == vision.packedWordCount());
    return EXIT_SUCCESS;
}

TickCommand moveCommand(
    std::uint64_t tick,
    std::uint32_t sequence,
    ecs::Entity entity,
    GridPoint target) {
    TickCommand command;
    command.targetTick = tick;
    command.issuer = 1;
    command.sequence = sequence;
    command.type = CommandType::Move;
    command.subject = entity;
    command.targetX = target.x;
    command.targetY = target.y;
    return command;
}

int testUnsteppedArchiveRoundTrip() {
    RtsSimulation original(8, 6);
    original.createUnit({2, 2}, {1}, 1, {}, 4);
    CHECK(original.vision().layerCount() == 0u);
    const auto archive = EncodeRtsSimulation(original);
    CHECK(!archive.empty());

    RtsSimulation restored(8, 6);
    CHECK(DecodeRtsSimulation(archive, restored));
    CHECK(restored.vision().layerCount() == 0u);
    CHECK(restored.snapshot().worldHash == 0u);
    CHECK(EncodeRtsSimulation(restored) == archive);
    return EXIT_SUCCESS;
}

int testSimulationSnapshotAndPersistence() {
    RtsSimulation original(12, 8);
    CHECK(original.setBlocked({6, 2}, true));
    const auto scout = original.createUnit(
        {2, 2}, {1}, 1, {}, 6);
    original.createUnit({9, 5}, {0}, 2, {}, 2);

    original.step(0);
    CHECK(original.snapshot().visibilityWidth == 12);
    CHECK(original.snapshot().visibilityHeight == 8);
    CHECK(original.snapshot().visibility.size() == 2u);
    CHECK(original.vision().visible(1, {5, 2}));
    CHECK(original.vision().visible(1, {6, 2}));
    CHECK(!original.vision().visible(1, {7, 2}));

    const auto scoutSnapshot = std::find_if(
        original.snapshot().entities.begin(),
        original.snapshot().entities.end(),
        [scout](const SnapshotEntity& entity) {
            return entity.entity == scout;
        });
    CHECK(scoutSnapshot != original.snapshot().entities.end());
    CHECK(scoutSnapshot->visionRange == 6);

    CHECK(original.submit(moveCommand(1, 1, scout, {4, 5})));
    original.step(1);
    original.step(2);
    CHECK(original.vision().explored(1, {2, 2}));
    const auto savedHash = original.snapshot().worldHash;
    const auto savedExplored = original.vision().exploredCount(1);
    const auto archive = EncodeRtsSimulation(original);
    CHECK(!archive.empty());

    RtsSimulation restored(12, 8);
    CHECK(DecodeRtsSimulation(archive, restored));
    CHECK(restored.snapshot().worldHash == savedHash);
    CHECK(restored.vision().exploredCount(1) == savedExplored);
    CHECK(restored.vision().explored(1, {2, 2}));
    CHECK(restored.vision().currentVisibleCount(1) ==
          original.vision().currentVisibleCount(1));
    CHECK(EncodeRtsSimulation(restored) == archive);

    for (std::uint64_t tick = 3; tick < 8; ++tick) {
        original.step(tick);
        restored.step(tick);
        CHECK(restored.snapshot().worldHash == original.snapshot().worldHash);
        CHECK(restored.vision().exploredCount(1) ==
              original.vision().exploredCount(1));
    }
    return EXIT_SUCCESS;
}

std::vector<std::uint64_t> runDeterministicScenario() {
    RtsSimulation simulation(10, 6);
    const auto first = simulation.createUnit({1, 1}, {1}, 1, {}, 3);
    const auto second = simulation.createUnit({1, 4}, {1}, 1, {}, 2);
    simulation.setBlocked({5, 2}, true);
    simulation.setBlocked({5, 3}, true);
    simulation.submit(moveCommand(0, 1, first, {8, 1}));
    simulation.submit(moveCommand(0, 2, second, {8, 4}));

    std::vector<std::uint64_t> hashes;
    for (std::uint64_t tick = 0; tick < 10; ++tick) {
        simulation.step(tick);
        hashes.push_back(simulation.snapshot().worldHash);
    }
    return hashes;
}

int testDeterministicHashes() {
    const auto first = runDeterministicScenario();
    const auto second = runDeterministicScenario();
    CHECK(first == second);
    CHECK(!first.empty());
    CHECK(first.back() != 0u);
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    CHECK(testLineOfSightAndTeams() == EXIT_SUCCESS);
    CHECK(testExploredHistoryAndBinaryState() == EXIT_SUCCESS);
    CHECK(testUnsteppedArchiveRoundTrip() == EXIT_SUCCESS);
    CHECK(testSimulationSnapshotAndPersistence() == EXIT_SUCCESS);
    CHECK(testDeterministicHashes() == EXIT_SUCCESS);
    std::cout << "vision and fog-of-war tests passed\n";
    return EXIT_SUCCESS;
}
