#include <RTSEngine/Rts/FlowField.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/Simulation.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace rts::gameplay;

NavigationGrid makeGrid(std::int32_t width = 32, std::int32_t height = 24) {
    NavigationGrid grid(width, height);
    const auto wallX = width / 2;
    const auto gapY = height / 2;
    for (std::int32_t y = 0; y < height; ++y) {
        if (y >= gapY - 1 && y <= gapY + 1) continue;
        assert(grid.setBlocked({wallX, y}, true));
    }
    return grid;
}

void testShortestDistanceExtraction() {
    auto grid = makeGrid();
    const GridPoint goal{31, 23};
    GridFlowField field;
    assert(field.build(grid, goal));
    assert(field.ready());
    assert(field.validGoal());
    assert(field.reachableCells() > 0);

    const GridPoint start{0, 0};
    std::vector<GridPoint> path;
    assert(field.extract(start, path));
    assert(!path.empty());
    assert(path.back() == goal);
    assert(path.size() == static_cast<std::size_t>(field.distance(start)));

    auto current = start;
    auto previousDistance = field.distance(start);
    for (const auto point : path) {
        GridPoint sampled;
        assert(field.nextStep(current, sampled));
        assert(sampled == point);
        assert(grid.contains(point));
        assert(!grid.blocked(point));
        const auto nextDistance = field.distance(point);
        assert(nextDistance + 1 == previousDistance);
        previousDistance = nextDistance;
        current = point;
    }
    GridPoint afterGoal;
    assert(!field.nextStep(goal, afterGoal));

    GridPathfinderScratch scratch;
    const auto astar = GridPathfinder::find(
        grid, start, goal, scratch, 10000);
    assert(astar.found);
    assert(astar.points.size() == path.size());
}

void testInvalidGoalAndUnreachableStart() {
    NavigationGrid grid(8, 8);
    assert(grid.setBlocked({7, 7}, true));
    GridFlowField invalid;
    assert(!invalid.build(grid, {7, 7}));
    std::vector<GridPoint> path{{1, 1}};
    assert(!invalid.extract({0, 0}, path));
    assert(path.empty());
    GridPoint next;
    assert(!invalid.nextStep({0, 0}, next));

    NavigationGrid split(8, 8);
    for (std::int32_t y = 0; y < 8; ++y) {
        assert(split.setBlocked({4, y}, true));
    }
    GridFlowField field;
    assert(field.build(split, {7, 7}));
    assert(field.distance({0, 0}) == GridFlowField::kUnreachable);
    assert(!field.extract({0, 0}, path));
    assert(!field.nextStep({0, 0}, next));
}

void testBoundedCacheAndInvalidation() {
    NavigationGrid grid(16, 16);
    GridFlowFieldCache cache({2, 16u * 16u * 2u});

    const auto& first = cache.resolve(grid, {15, 15});
    assert(first.validGoal());
    const auto& second = cache.resolve(grid, {0, 15});
    assert(second.validGoal());
    assert(cache.entryCount() == 2);
    assert(cache.stats().misses == 2);

    cache.resolve(grid, {15, 15});
    assert(cache.stats().hits == 1);
    cache.resolve(grid, {15, 0});
    assert(cache.entryCount() == 2);
    assert(cache.stats().evictions == 1);

    const auto missesBefore = cache.stats().misses;
    cache.resolve(grid, {0, 15});
    assert(cache.stats().misses == missesBefore + 1);

    assert(grid.setBlocked({8, 8}, true));
    cache.resolve(grid, {15, 15});
    assert(cache.stats().invalidations == 1);
    assert(cache.entryCount() == 1);

    GridFlowFieldCache tiny({4, 10});
    const auto& transient = tiny.resolve(grid, {15, 15});
    assert(transient.validGoal());
    assert(tiny.entryCount() == 0);
    assert(tiny.stats().uncachedFields == 1);
}

void testDirectCacheSampling() {
    auto grid = makeGrid();
    GridFlowFieldCache cache;
    const GridPoint start{0, 0};
    const GridPoint goal{31, 23};
    const auto& field = cache.resolve(grid, goal);
    assert(cache.assignDirect(field, start));
    assert(cache.stats().directAssignments == 1);
    assert(cache.stats().pathExtractions == 1);
    assert(cache.stats().extractedPathPoints ==
           static_cast<std::uint64_t>(field.distance(start)));

    GridPoint next;
    assert(cache.sample(grid, goal, start, next));
    assert(field.distance(next) + 1 == field.distance(start));
    assert(cache.stats().directSamples == 1);
    assert(cache.stats().directSampleFailures == 0);
}

struct ScaleRun final {
    std::vector<std::uint64_t> hashes;
    GridFlowFieldStats flowStats;
    GridPathCacheStats pathStats;
    std::size_t intentCapacity{};
    std::size_t rejectedCapacity{};
    std::size_t distinctPositions{};
    bool firstAgentUsesSharedField{};
};

ScaleRun runScaleScenario() {
    constexpr std::int32_t width = 64;
    constexpr std::int32_t height = 64;
    constexpr std::uint32_t unitCount = 1000;
    constexpr std::uint64_t warmupTicks = 56;
    constexpr std::uint64_t ticks = 64;

    RtsSimulation simulation(width, height);
    for (std::int32_t y = 0; y < height; ++y) {
        if (y >= 30 && y <= 34) continue;
        assert(simulation.setBlocked({48, y}, true));
    }

    std::uint32_t sequence = 1;
    rts::ecs::Entity firstUnit;
    for (std::int32_t y = 0; y < 25; ++y) {
        for (std::int32_t x = 0; x < 40; ++x) {
            const auto unit = simulation.createUnit({x, y}, {1});
            if (!firstUnit.valid()) firstUnit = unit;
            TickCommand move;
            move.targetTick = 0;
            move.issuer = 1;
            move.sequence = sequence++;
            move.type = CommandType::Move;
            move.subject = unit;
            move.targetX = 63;
            move.targetY = 63;
            assert(simulation.submit(move));
        }
    }
    assert(sequence == unitCount + 1u);

    ScaleRun run;
    run.hashes.reserve(ticks);
    std::size_t warmedIntentCapacity = 0;
    std::size_t warmedRejectedCapacity = 0;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        simulation.step(tick);
        run.hashes.push_back(simulation.snapshot().worldHash);
        if (tick == 0) {
            assert(simulation.flowFields().stats().builds == 1);
            assert(simulation.flowFields().stats().misses == 1);
            assert(simulation.flowFields().stats().pathExtractions == unitCount);
            assert(simulation.flowFields().stats().directAssignments == unitCount);
            assert(simulation.pathCache().stats().misses == 0);
            assert(simulation.movementReservations().intentCapacity() >=
                   unitCount);
            const auto* agent =
                simulation.world().try_get<MovementAgent>(firstUnit);
            run.firstAgentUsesSharedField =
                agent && agent->flowFieldPath && agent->path.empty() &&
                agent->flowContext && agent->flowSample;
            assert(run.firstAgentUsesSharedField);
        }
        if (tick + 1 == warmupTicks) {
            warmedIntentCapacity =
                simulation.movementReservations().intentCapacity();
            warmedRejectedCapacity =
                simulation.movementReservations().rejectedCapacity();
        } else if (tick + 1 > warmupTicks) {
            assert(simulation.movementReservations().intentCapacity() ==
                   warmedIntentCapacity);
            assert(simulation.movementReservations().rejectedCapacity() ==
                   warmedRejectedCapacity);
        }
    }

    run.flowStats = simulation.flowFields().stats();
    run.pathStats = simulation.pathCache().stats();
    run.intentCapacity = warmedIntentCapacity;
    run.rejectedCapacity = warmedRejectedCapacity;

    std::vector<std::uint64_t> positions;
    positions.reserve(simulation.snapshot().entities.size());
    for (const auto& entity : simulation.snapshot().entities) {
        if (entity.kind != SnapshotKind::Unit) continue;
        positions.push_back(
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(entity.y)) *
                static_cast<std::uint64_t>(width) +
            static_cast<std::uint32_t>(entity.x));
    }
    std::sort(positions.begin(), positions.end());
    run.distinctPositions = static_cast<std::size_t>(
        std::unique(positions.begin(), positions.end()) - positions.begin());
    assert(positions.size() == unitCount);
    assert(run.distinctPositions == unitCount);
    return run;
}

void testSimulationSharedFieldAndScaleDeterminism() {
    const auto first = runScaleScenario();
    const auto second = runScaleScenario();

    assert(first.hashes == second.hashes);
    assert(!first.hashes.empty());
    assert(first.hashes.back() != 0);
    assert(first.flowStats.builds == 1);
    assert(first.flowStats.misses == 1);
    assert(first.flowStats.pathExtractions >= 1000);
    assert(first.flowStats.directAssignments >= 1000);
    assert(first.flowStats.directAssignments ==
           second.flowStats.directAssignments);
    assert(first.flowStats.directSamples > 0);
    assert(first.flowStats.directSamples == second.flowStats.directSamples);
    assert(first.flowStats.directSampleFailures == 0);
    assert(first.flowStats.directSampleFailures ==
           second.flowStats.directSampleFailures);
    assert(first.flowStats.extractionFailures == 0);
    assert(first.pathStats.misses < 1000);
    assert(first.intentCapacity == second.intentCapacity);
    assert(first.rejectedCapacity == second.rejectedCapacity);
    assert(first.distinctPositions == 1000);
    assert(first.firstAgentUsesSharedField);
    assert(second.firstAgentUsesSharedField);
}

} // namespace

int main() {
    testShortestDistanceExtraction();
    testInvalidGoalAndUnreachableStart();
    testBoundedCacheAndInvalidation();
    testDirectCacheSampling();
    testSimulationSharedFieldAndScaleDeterminism();
    std::cout << "flow field and 1000-agent tests passed\n";
    return 0;
}
