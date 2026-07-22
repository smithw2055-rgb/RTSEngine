#include <RTSEngine/Rts/Navigation.h>

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

} // namespace

int main() {
    testStableHeapPath();
    testWorkspaceStopsGrowingAfterWarmup();
    testBudgetAndInvalidRequests();
    std::cout << "navigation workspace tests passed\n";
    return 0;
}
