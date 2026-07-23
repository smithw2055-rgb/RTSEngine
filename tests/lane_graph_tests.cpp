#include <RTSEngine/TowerDefense/LaneGraph.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace {

using namespace rts::tower_defense;

LaneGraph buildEqualCostGraph(bool reverseRegistration) {
    LaneGraph graph;
    const std::vector<LaneNode> nodes = reverseRegistration
        ? std::vector<LaneNode>{
              {4, {2, 0}},
              {3, {0, 1}},
              {2, {1, 0}},
              {1, {0, 0}}}
        : std::vector<LaneNode>{
              {1, {0, 0}},
              {2, {1, 0}},
              {3, {0, 1}},
              {4, {2, 0}}};
    for (const auto& node : nodes) {
        assert(graph.upsertNode(node));
    }

    if (reverseRegistration) {
        assert(graph.connect(3, 4));
        assert(graph.connect(1, 3));
        assert(graph.connect(2, 4));
        assert(graph.connect(1, 2));
    } else {
        assert(graph.connect(1, 2));
        assert(graph.connect(2, 4));
        assert(graph.connect(1, 3));
        assert(graph.connect(3, 4));
    }
    return graph;
}

void testStableTieBreakIgnoresRegistrationOrder() {
    const auto first = buildEqualCostGraph(false).findRoute(1, 4);
    const auto second = buildEqualCostGraph(true).findRoute(1, 4);

    assert(first.found);
    assert(second.found);
    assert(first.failure == LaneRouteFailure::None);
    assert(first.totalCost == 2);
    assert((first.nodeIds == std::vector<LaneNodeId>{1, 2, 4}));
    assert(first.nodeIds == second.nodeIds);
    assert(first.points == second.points);
}

void testCostAndConnectionStateControlRouting() {
    LaneGraph graph;
    assert(graph.upsertNode({1, {0, 0}}));
    assert(graph.upsertNode({2, {1, 0}}));
    assert(graph.upsertNode({3, {2, 0}}));
    assert(graph.connect(1, 3, 9));
    assert(graph.connect(1, 2, 2));
    assert(graph.connect(2, 3, 2));

    auto route = graph.findRoute(1, 3);
    assert(route.found);
    assert(route.totalCost == 4);
    assert((route.nodeIds == std::vector<LaneNodeId>{1, 2, 3}));

    const auto revision = graph.revision();
    assert(graph.setConnectionEnabled(2, 3, false));
    assert(graph.revision() == revision + 1);
    route = graph.findRoute(1, 3);
    assert(route.found);
    assert(route.totalCost == 9);
    assert((route.nodeIds == std::vector<LaneNodeId>{1, 3}));

    assert(graph.setConnectionEnabled(1, 3, false));
    route = graph.findRoute(1, 3);
    assert(!route.found);
    assert(route.failure == LaneRouteFailure::Unreachable);
}

void testMutationAndFailureModes() {
    LaneGraph graph;
    assert(!graph.upsertNode({0, {0, 0}}));
    assert(graph.upsertNode({1, {0, 0}}));
    assert(graph.upsertNode({2, {1, 0}}));
    assert(!graph.connect(1, 3));
    assert(!graph.connect(1, 1));
    assert(!graph.connect(1, 2, 0));
    assert(graph.connectBidirectional(1, 2, 3));
    assert(graph.nodeCount() == 2);
    assert(graph.connectionCount() == 2);
    assert(graph.connectionEnabled(1, 2));
    assert(graph.connectionEnabled(2, 1));

    const auto sameNode = graph.findRoute(1, 1);
    assert(sameNode.found);
    assert(sameNode.totalCost == 0);
    assert((sameNode.nodeIds == std::vector<LaneNodeId>{1}));

    assert(graph.removeNode(2));
    assert(graph.nodeCount() == 1);
    assert(graph.connectionCount() == 0);
    assert(graph.findRoute(1, 2).failure == LaneRouteFailure::UnknownGoal);
    assert(graph.findRoute(9, 1).failure == LaneRouteFailure::UnknownStart);
}

void testRouteCostOverflowIsExplicit() {
    LaneGraph graph;
    assert(graph.upsertNode({1, {0, 0}}));
    assert(graph.upsertNode({2, {1, 0}}));
    assert(graph.upsertNode({3, {2, 0}}));
    assert(graph.connect(
        1, 2, std::numeric_limits<std::uint32_t>::max()));
    assert(graph.connect(2, 3, 1));

    const auto route = graph.findRoute(1, 3);
    assert(!route.found);
    assert(route.failure == LaneRouteFailure::CostOverflow);
}

} // namespace

int main() {
    testStableTieBreakIgnoresRegistrationOrder();
    testCostAndConnectionStateControlRouting();
    testMutationAndFailureModes();
    testRouteCostOverflowIsExplicit();
    std::cout << "lane graph tests passed\n";
    return 0;
}
