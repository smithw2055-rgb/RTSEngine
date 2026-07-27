#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/PathCache.h>
#include <RTSEngine/Rts/Simulation.h>

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using namespace rts::gameplay;

NavigationGrid makeGrid() {
    NavigationGrid grid(64, 64);
    for (std::int32_t y = 0; y < 64; ++y) {
        if (y == 32) continue;
        assert(grid.setBlocked({31, y}, true));
    }
    for (std::int32_t x = 8; x < 56; ++x) {
        if (x == 24 || x == 40) continue;
        assert(grid.setBlocked({x, 20}, true));
        assert(grid.setBlocked({x, 44}, true));
    }
    return grid;
}

bool equalResult(const PathResult& a, const PathResult& b) {
    return a.found == b.found &&
           a.budgetExceeded == b.budgetExceeded &&
           a.expandedNodes == b.expandedNodes &&
           a.points == b.points;
}

void testStableHeapPath() {
    auto grid = makeGrid();
    GridPathfinderScratch scratch;

    const auto first = GridPathfinder::find(
        grid, {0, 0}, {63, 63}, scratch, 20000);
    const auto second = GridPathfinder::find(
        grid, {0, 0}, {63, 63}, scratch, 20000);

    assert(first.found && second.found);
    assert(!first.budgetExceeded && !second.budgetExceeded);
    assert(first.points == second.points);
    assert(first.expandedNodes == second.expandedNodes);
    assert(!first.points.empty());
    assert((first.points.back() == GridPoint{63, 63}));
    for (const auto point : first.points) {
        assert(!grid.blocked(point));
    }

    const auto defaultApi = GridPathfinder::find(
        grid, {0, 0}, {63, 63}, 20000);
    assert(defaultApi.found);
    assert(defaultApi.points == first.points);
}

void testWorkspaceStopsGrowingAfterWarmup() {
    auto grid = makeGrid();
    GridPathfinderScratch scratch;

    const auto warm = GridPathfinder::find(
        grid, {0, 0}, {63, 63}, scratch, 20000);
    assert(warm.found);

    const auto cellCapacity = scratch.cellCapacity();
    const auto heapCapacity = scratch.heapCapacity();
    const auto reverseCapacity = scratch.reverseCapacity();
    assert(cellCapacity >= 64u * 64u);
    assert(heapCapacity >= 64u * 64u);
    assert(reverseCapacity >= 64u * 64u);

    for (std::uint32_t request = 0; request < 512; ++request) {
        const GridPoint start{
            static_cast<std::int32_t>(request % 16u),
            static_cast<std::int32_t>((request * 7u) % 16u)};
        const GridPoint goal{
            63 - static_cast<std::int32_t>(request % 16u),
            63 - static_cast<std::int32_t>((request * 11u) % 16u)};
        const auto path = GridPathfinder::find(
            grid, start, goal, scratch, 20000);
        assert(path.found);
        assert(!path.budgetExceeded);
        assert(path.expandedNodes <= 20000u);
        assert(scratch.cellCapacity() == cellCapacity);
        assert(scratch.heapCapacity() == heapCapacity);
        assert(scratch.reverseCapacity() == reverseCapacity);
    }
}

void testBudgetAndInvalidRequests() {
    auto grid = makeGrid();
    GridPathfinderScratch scratch;

    const auto budgeted = GridPathfinder::find(
        grid, {0, 0}, {63, 63}, scratch, 1);
    assert(!budgeted.found);
    assert(budgeted.budgetExceeded);
    assert(budgeted.expandedNodes == 2);

    const auto invalid = GridPathfinder::find(
        grid, {-1, 0}, {63, 63}, scratch, 20000);
    assert(!invalid.found);
    assert(!invalid.budgetExceeded);
    assert(invalid.expandedNodes == 0);

    const auto same = GridPathfinder::find(
        grid, {4, 4}, {4, 4}, scratch, 0);
    assert(same.found);
    assert(same.points.empty());
    assert(same.expandedNodes == 0);
}

void testCacheHitMatchesColdSearch() {
    auto grid = makeGrid();
    GridPathfinderScratch scratch;
    GridPathCache cache;

    const auto cold = GridPathfinder::find(
        grid, {0, 0}, {63, 63}, scratch, 20000);
    const PathResult first = cache.resolve(
        grid, {0, 0}, {63, 63}, scratch, 20000);
    const PathResult second = cache.resolve(
        grid, {0, 0}, {63, 63}, scratch, 20000);

    assert(equalResult(cold, first));
    assert(equalResult(first, second));
    assert(cache.entryCount() == 1);
    assert(cache.pointCount() == first.points.size());
    assert(cache.stats().misses == 1);
    assert(cache.stats().hits == 1);
    assert(cache.stats().insertions == 1);
    assert(cache.stats().evictions == 0);
}

void testCacheStoresNegativeAndBudgetResults() {
    NavigationGrid grid(8, 8);
    GridPathfinderScratch scratch;
    GridPathCache cache;

    const PathResult invalidFirst = cache.resolve(
        grid, {-1, 0}, {7, 7}, scratch, 20000);
    const PathResult invalidSecond = cache.resolve(
        grid, {-1, 0}, {7, 7}, scratch, 20000);
    assert(equalResult(invalidFirst, invalidSecond));
    assert(!invalidFirst.found && !invalidFirst.budgetExceeded);

    const PathResult budgetFirst = cache.resolve(
        grid, {0, 0}, {7, 7}, scratch, 1);
    const PathResult budgetSecond = cache.resolve(
        grid, {0, 0}, {7, 7}, scratch, 1);
    assert(equalResult(budgetFirst, budgetSecond));
    assert(!budgetFirst.found && budgetFirst.budgetExceeded);

    const PathResult fullBudget = cache.resolve(
        grid, {0, 0}, {7, 7}, scratch, 20000);
    assert(fullBudget.found);
    assert(cache.entryCount() == 3);
    assert(cache.stats().misses == 3);
    assert(cache.stats().hits == 2);
}

void testRevisionAndRestoreInvalidateCache() {
    NavigationGrid grid(8, 3);
    GridPathfinderScratch scratch;
    GridPathCache cache;

    const PathResult straight = cache.resolve(
        grid, {0, 1}, {7, 1}, scratch, 1000);
    assert(straight.found);
    assert(cache.entryCount() == 1);

    assert(grid.setBlocked({3, 1}, true));
    const PathResult detour = cache.resolve(
        grid, {0, 1}, {7, 1}, scratch, 1000);
    assert(detour.found);
    assert(detour.points != straight.points);
    for (const auto point : detour.points) {
        assert(!grid.blocked(point));
    }
    assert(cache.stats().invalidations == 1);

    NavigationGrid replacement(8, 3);
    assert(replacement.setBlocked({4, 1}, true));
    const auto sameRevision = replacement.revision();
    assert(sameRevision == grid.revision());
    grid = std::move(replacement);

    const PathResult restored = cache.resolve(
        grid, {0, 1}, {7, 1}, scratch, 1000);
    assert(restored.found);
    assert(cache.stats().invalidations == 2);
    for (const auto point : restored.points) {
        assert(!grid.blocked(point));
    }
}

void testDeterministicLruAndPointBudget() {
    NavigationGrid grid(8, 8);
    GridPathfinderScratch scratch;
    GridPathCache cache({2, 64});

    const PathResult a = cache.resolve(
        grid, {0, 0}, {2, 0}, scratch, 1000);
    const PathResult b = cache.resolve(
        grid, {0, 1}, {2, 1}, scratch, 1000);
    const PathResult aHit = cache.resolve(
        grid, {0, 0}, {2, 0}, scratch, 1000);
    const PathResult c = cache.resolve(
        grid, {0, 2}, {2, 2}, scratch, 1000);
    assert(a.found && b.found && aHit.found && c.found);
    assert(cache.entryCount() == 2);
    assert(cache.stats().misses == 3);
    assert(cache.stats().hits == 1);
    assert(cache.stats().evictions == 1);

    const PathResult aSecondHit = cache.resolve(
        grid, {0, 0}, {2, 0}, scratch, 1000);
    assert(aSecondHit.found);
    assert(cache.stats().hits == 2);

    const PathResult bMiss = cache.resolve(
        grid, {0, 1}, {2, 1}, scratch, 1000);
    assert(bMiss.found);
    assert(cache.stats().misses == 4);
    assert(cache.stats().evictions == 2);
    assert(cache.entryCapacity() >= 2);

    GridPathCache tiny({8, 1});
    const PathResult longFirst = tiny.resolve(
        grid, {0, 0}, {7, 0}, scratch, 1000);
    const PathResult longSecond = tiny.resolve(
        grid, {0, 0}, {7, 0}, scratch, 1000);
    assert(equalResult(longFirst, longSecond));
    assert(tiny.entryCount() == 0);
    assert(tiny.stats().misses == 2);
    assert(tiny.stats().uncachedResults == 2);
}

void testSimulationUsesSharedCache() {
    RtsSimulation simulation(8, 4);
    const auto first = simulation.createUnit({0, 0}, {1});
    const auto second = simulation.createUnit({0, 0}, {1});
    assert(first.valid() && second.valid());
    assert(simulation.submit(
        {0, 1, 1, CommandType::Move, first, 7, 0, false}));
    simulation.step(0);
    assert(simulation.pathCache().stats().misses == 1);
    assert(simulation.pathCache().stats().hits == 0);

    assert(simulation.submit(
        {1, 1, 2, CommandType::Move, second, 7, 0, false}));
    simulation.step(1);
    assert(simulation.pathCache().stats().misses == 1);
    assert(simulation.pathCache().stats().hits == 1);
    assert(simulation.pathCache().entryCount() == 1);
}

} // namespace

int main() {
    testStableHeapPath();
    testWorkspaceStopsGrowingAfterWarmup();
    testBudgetAndInvalidRequests();
    testCacheHitMatchesColdSearch();
    testCacheStoresNegativeAndBudgetResults();
    testRevisionAndRestoreInvalidateCache();
    testDeterministicLruAndPointBudget();
    testSimulationUsesSharedCache();
    std::cout << "navigation workspace and cache tests passed\n";
    return 0;
}
