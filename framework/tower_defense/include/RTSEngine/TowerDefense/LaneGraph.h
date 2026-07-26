#pragma once

#include <RTSEngine/Rts/Navigation.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::tower_defense {

using LaneNodeId = std::uint32_t;

struct LaneNode final {
    LaneNodeId id{};
    gameplay::GridPoint point{};
};

enum class LaneRouteFailure : std::uint8_t {
    None,
    UnknownStart,
    UnknownGoal,
    Unreachable,
    CostOverflow
};

struct LaneRoute final {
    bool found{};
    LaneRouteFailure failure{LaneRouteFailure::Unreachable};
    std::uint32_t totalCost{};
    std::vector<LaneNodeId> nodeIds;
    std::vector<gameplay::GridPoint> points;
};

class LaneGraph final {
public:
    bool upsertNode(LaneNode node) {
        if (node.id == 0) return false;
        const auto found = lowerNode(node.id);
        if (found != nodes_.end() && found->node.id == node.id) {
            if (found->node.point == node.point) return true;
            found->node.point = node.point;
            ++revision_;
            return true;
        }
        NodeEntry entry;
        entry.node = node;
        nodes_.insert(found, std::move(entry));
        ++revision_;
        return true;
    }

    bool removeNode(LaneNodeId id) {
        const auto found = lowerNode(id);
        if (found == nodes_.end() || found->node.id != id) return false;
        nodes_.erase(found);
        for (auto& entry : nodes_) {
            const auto edge = lowerEdge(entry.edges, id);
            if (edge != entry.edges.end() && edge->to == id) {
                entry.edges.erase(edge);
            }
        }
        ++revision_;
        return true;
    }

    bool connect(LaneNodeId from, LaneNodeId to, std::uint32_t cost = 1) {
        if (from == 0 || to == 0 || from == to || cost == 0) return false;
        auto* source = findEntry(from);
        if (!source || !node(to)) return false;
        const auto found = lowerEdge(source->edges, to);
        if (found != source->edges.end() && found->to == to) {
            if (found->cost == cost && found->enabled) return true;
            found->cost = cost;
            found->enabled = true;
            ++revision_;
            return true;
        }
        source->edges.insert(found, Edge{to, cost, true});
        ++revision_;
        return true;
    }

    bool connectBidirectional(
        LaneNodeId first,
        LaneNodeId second,
        std::uint32_t cost = 1) {
        if (first == 0 || second == 0 || first == second || cost == 0 ||
            !node(first) || !node(second)) {
            return false;
        }
        return connect(first, second, cost) &&
               connect(second, first, cost);
    }

    bool disconnect(LaneNodeId from, LaneNodeId to) {
        auto* source = findEntry(from);
        if (!source) return false;
        const auto found = lowerEdge(source->edges, to);
        if (found == source->edges.end() || found->to != to) return false;
        source->edges.erase(found);
        ++revision_;
        return true;
    }

    bool setConnectionEnabled(
        LaneNodeId from,
        LaneNodeId to,
        bool enabled) {
        auto* source = findEntry(from);
        if (!source) return false;
        const auto found = lowerEdge(source->edges, to);
        if (found == source->edges.end() || found->to != to) return false;
        if (found->enabled == enabled) return true;
        found->enabled = enabled;
        ++revision_;
        return true;
    }

    const LaneNode* node(LaneNodeId id) const noexcept {
        const auto found = lowerNode(id);
        return found != nodes_.end() && found->node.id == id
            ? &found->node
            : nullptr;
    }

    bool hasConnection(LaneNodeId from, LaneNodeId to) const noexcept {
        const auto* source = findEntry(from);
        if (!source) return false;
        const auto found = lowerEdge(source->edges, to);
        return found != source->edges.end() && found->to == to;
    }

    bool connectionEnabled(LaneNodeId from, LaneNodeId to) const noexcept {
        const auto* source = findEntry(from);
        if (!source) return false;
        const auto found = lowerEdge(source->edges, to);
        return found != source->edges.end() && found->to == to &&
               found->enabled;
    }

    std::size_t nodeCount() const noexcept { return nodes_.size(); }

    std::size_t connectionCount() const noexcept {
        std::size_t count = 0;
        for (const auto& entry : nodes_) count += entry.edges.size();
        return count;
    }

    std::uint64_t revision() const noexcept { return revision_; }

    void appendHash(foundation::CanonicalHash& hash) const noexcept {
        hash.WriteU32(static_cast<std::uint32_t>(nodes_.size()));
        for (const auto& entry : nodes_) {
            hash.WriteU32(entry.node.id);
            hash.WriteI32(entry.node.point.x);
            hash.WriteI32(entry.node.point.y);
            hash.WriteU32(static_cast<std::uint32_t>(entry.edges.size()));
            for (const auto& edge : entry.edges) {
                hash.WriteU32(edge.to);
                hash.WriteU32(edge.cost);
                hash.WriteBool(edge.enabled);
            }
        }
    }

    LaneRoute findRoute(LaneNodeId start, LaneNodeId goal) const {
        const auto startIndex = indexOf(start);
        if (startIndex == kInvalidIndex) {
            return failure(LaneRouteFailure::UnknownStart);
        }
        const auto goalIndex = indexOf(goal);
        if (goalIndex == kInvalidIndex) {
            return failure(LaneRouteFailure::UnknownGoal);
        }
        if (start == goal) {
            LaneRoute route;
            route.found = true;
            route.failure = LaneRouteFailure::None;
            route.nodeIds.push_back(start);
            route.points.push_back(nodes_[startIndex].node.point);
            return route;
        }

        constexpr auto infinity = std::numeric_limits<std::uint64_t>::max();
        std::vector<std::uint64_t> distance(nodes_.size(), infinity);
        std::vector<std::size_t> previous(nodes_.size(), kInvalidIndex);
        std::vector<std::uint8_t> visited(nodes_.size(), 0);
        distance[startIndex] = 0;

        for (std::size_t pass = 0; pass < nodes_.size(); ++pass) {
            auto current = kInvalidIndex;
            for (std::size_t index = 0; index < nodes_.size(); ++index) {
                if (visited[index] != 0 || distance[index] == infinity) continue;
                if (current == kInvalidIndex ||
                    distance[index] < distance[current] ||
                    (distance[index] == distance[current] &&
                     nodes_[index].node.id < nodes_[current].node.id)) {
                    current = index;
                }
            }
            if (current == kInvalidIndex) break;
            if (current == goalIndex) break;
            visited[current] = 1;

            for (const auto& edge : nodes_[current].edges) {
                if (!edge.enabled) continue;
                const auto next = indexOf(edge.to);
                if (next == kInvalidIndex || visited[next] != 0) continue;
                if (distance[current] > infinity - edge.cost) continue;
                const auto candidate = distance[current] + edge.cost;
                const auto previousId = previous[next] == kInvalidIndex
                    ? std::numeric_limits<LaneNodeId>::max()
                    : nodes_[previous[next]].node.id;
                if (candidate < distance[next] ||
                    (candidate == distance[next] &&
                     nodes_[current].node.id < previousId)) {
                    distance[next] = candidate;
                    previous[next] = current;
                }
            }
        }

        if (distance[goalIndex] == infinity) {
            return failure(LaneRouteFailure::Unreachable);
        }
        if (distance[goalIndex] >
            std::numeric_limits<std::uint32_t>::max()) {
            return failure(LaneRouteFailure::CostOverflow);
        }

        std::vector<std::size_t> reversed;
        for (auto current = goalIndex; current != kInvalidIndex;
             current = previous[current]) {
            reversed.push_back(current);
            if (current == startIndex) break;
        }
        if (reversed.empty() || reversed.back() != startIndex) {
            return failure(LaneRouteFailure::Unreachable);
        }

        LaneRoute route;
        route.found = true;
        route.failure = LaneRouteFailure::None;
        route.totalCost = static_cast<std::uint32_t>(distance[goalIndex]);
        route.nodeIds.reserve(reversed.size());
        route.points.reserve(reversed.size());
        for (auto iterator = reversed.rbegin(); iterator != reversed.rend();
             ++iterator) {
            route.nodeIds.push_back(nodes_[*iterator].node.id);
            route.points.push_back(nodes_[*iterator].node.point);
        }
        return route;
    }

private:
    struct Edge final {
        LaneNodeId to{};
        std::uint32_t cost{1};
        bool enabled{true};
    };

    struct NodeEntry final {
        LaneNode node;
        std::vector<Edge> edges;
    };

    static constexpr std::size_t kInvalidIndex =
        std::numeric_limits<std::size_t>::max();

    using NodeIterator = std::vector<NodeEntry>::iterator;
    using ConstNodeIterator = std::vector<NodeEntry>::const_iterator;
    using EdgeIterator = std::vector<Edge>::iterator;
    using ConstEdgeIterator = std::vector<Edge>::const_iterator;

    NodeIterator lowerNode(LaneNodeId id) noexcept {
        return std::lower_bound(
            nodes_.begin(), nodes_.end(), id,
            [](const NodeEntry& entry, LaneNodeId value) {
                return entry.node.id < value;
            });
    }

    ConstNodeIterator lowerNode(LaneNodeId id) const noexcept {
        return std::lower_bound(
            nodes_.begin(), nodes_.end(), id,
            [](const NodeEntry& entry, LaneNodeId value) {
                return entry.node.id < value;
            });
    }

    static EdgeIterator lowerEdge(
        std::vector<Edge>& edges,
        LaneNodeId to) noexcept {
        return std::lower_bound(
            edges.begin(), edges.end(), to,
            [](const Edge& edge, LaneNodeId value) {
                return edge.to < value;
            });
    }

    static ConstEdgeIterator lowerEdge(
        const std::vector<Edge>& edges,
        LaneNodeId to) noexcept {
        return std::lower_bound(
            edges.begin(), edges.end(), to,
            [](const Edge& edge, LaneNodeId value) {
                return edge.to < value;
            });
    }

    NodeEntry* findEntry(LaneNodeId id) noexcept {
        const auto found = lowerNode(id);
        return found != nodes_.end() && found->node.id == id
            ? &*found
            : nullptr;
    }

    const NodeEntry* findEntry(LaneNodeId id) const noexcept {
        const auto found = lowerNode(id);
        return found != nodes_.end() && found->node.id == id
            ? &*found
            : nullptr;
    }

    std::size_t indexOf(LaneNodeId id) const noexcept {
        const auto found = lowerNode(id);
        return found != nodes_.end() && found->node.id == id
            ? static_cast<std::size_t>(found - nodes_.begin())
            : kInvalidIndex;
    }

    static LaneRoute failure(LaneRouteFailure value) {
        LaneRoute route;
        route.failure = value;
        return route;
    }

    std::vector<NodeEntry> nodes_;
    std::uint64_t revision_{};
};

} // namespace rts::tower_defense
