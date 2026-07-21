#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

struct GridPoint {
    std::int32_t x{};
    std::int32_t y{};

    friend bool operator==(GridPoint a, GridPoint b) noexcept {
        return a.x == b.x && a.y == b.y;
    }
};

class NavigationGrid {
public:
    NavigationGrid(std::int32_t width = 32, std::int32_t height = 32)
        : width_(width), height_(height), blocked_(static_cast<std::size_t>(width * height), 0) {}

    std::int32_t width() const noexcept { return width_; }
    std::int32_t height() const noexcept { return height_; }
    std::uint64_t revision() const noexcept { return revision_; }

    bool contains(GridPoint point) const noexcept {
        return point.x >= 0 && point.y >= 0 && point.x < width_ && point.y < height_;
    }

    bool blocked(GridPoint point) const noexcept {
        return !contains(point) || blocked_[index(point)] != 0;
    }

    bool setBlocked(GridPoint point, bool value) {
        if (!contains(point)) {
            return false;
        }
        auto& cell = blocked_[index(point)];
        const auto next = static_cast<std::uint8_t>(value ? 1 : 0);
        if (cell == next) {
            return true;
        }
        cell = next;
        ++revision_;
        return true;
    }

    const std::vector<std::uint8_t>& blockers() const noexcept { return blocked_; }

private:
    std::size_t index(GridPoint point) const noexcept {
        return static_cast<std::size_t>(point.y * width_ + point.x);
    }

    std::int32_t width_{};
    std::int32_t height_{};
    std::vector<std::uint8_t> blocked_;
    std::uint64_t revision_{};
};

struct PathResult {
    bool found{};
    bool budgetExceeded{};
    std::vector<GridPoint> points;
};

class GridPathfinder {
public:
    static PathResult find(const NavigationGrid& grid,
                           GridPoint start,
                           GridPoint goal,
                           std::uint32_t nodeBudget = 4096) {
        PathResult result;
        if (!grid.contains(start) || !grid.contains(goal) || grid.blocked(start) || grid.blocked(goal)) {
            return result;
        }
        if (start == goal) {
            result.found = true;
            return result;
        }

        const auto width = grid.width();
        const auto total = static_cast<std::size_t>(width * grid.height());
        constexpr std::int32_t infinity = std::numeric_limits<std::int32_t>::max();
        std::vector<std::int32_t> cost(total, infinity);
        std::vector<std::int32_t> parent(total, -1);
        std::vector<std::uint8_t> open(total, 0);
        std::vector<std::uint8_t> closed(total, 0);

        const auto toIndex = [width](GridPoint point) {
            return static_cast<std::int32_t>(point.y * width + point.x);
        };
        const auto toPoint = [width](std::int32_t index) {
            return GridPoint{index % width, index / width};
        };
        const auto heuristic = [](GridPoint a, GridPoint b) {
            const auto dx = a.x > b.x ? a.x - b.x : b.x - a.x;
            const auto dy = a.y > b.y ? a.y - b.y : b.y - a.y;
            return dx + dy;
        };

        const auto startIndex = toIndex(start);
        const auto goalIndex = toIndex(goal);
        cost[static_cast<std::size_t>(startIndex)] = 0;
        open[static_cast<std::size_t>(startIndex)] = 1;

        static constexpr GridPoint directions[] = {
            {0, -1}, {1, 0}, {0, 1}, {-1, 0}
        };

        std::uint32_t expanded = 0;
        while (true) {
            std::int32_t current = -1;
            std::int32_t bestF = infinity;
            std::int32_t bestH = infinity;
            for (std::int32_t index = 0; index < static_cast<std::int32_t>(total); ++index) {
                if (!open[static_cast<std::size_t>(index)]) {
                    continue;
                }
                const auto point = toPoint(index);
                const auto h = heuristic(point, goal);
                const auto f = cost[static_cast<std::size_t>(index)] + h;
                if (f < bestF || (f == bestF && (h < bestH || (h == bestH && index < current)))) {
                    current = index;
                    bestF = f;
                    bestH = h;
                }
            }

            if (current < 0) {
                return result;
            }
            if (++expanded > nodeBudget) {
                result.budgetExceeded = true;
                return result;
            }
            if (current == goalIndex) {
                break;
            }

            open[static_cast<std::size_t>(current)] = 0;
            closed[static_cast<std::size_t>(current)] = 1;
            const auto point = toPoint(current);
            for (const auto direction : directions) {
                const GridPoint next{point.x + direction.x, point.y + direction.y};
                if (!grid.contains(next) || grid.blocked(next)) {
                    continue;
                }
                const auto nextIndex = toIndex(next);
                if (closed[static_cast<std::size_t>(nextIndex)]) {
                    continue;
                }
                const auto nextCost = cost[static_cast<std::size_t>(current)] + 1;
                if (nextCost < cost[static_cast<std::size_t>(nextIndex)]) {
                    cost[static_cast<std::size_t>(nextIndex)] = nextCost;
                    parent[static_cast<std::size_t>(nextIndex)] = current;
                    open[static_cast<std::size_t>(nextIndex)] = 1;
                }
            }
        }

        std::vector<GridPoint> reversed;
        for (auto cursor = goalIndex; cursor != startIndex; cursor = parent[static_cast<std::size_t>(cursor)]) {
            if (cursor < 0) {
                return {};
            }
            reversed.push_back(toPoint(cursor));
        }
        result.points.assign(reversed.rbegin(), reversed.rend());
        result.found = true;
        return result;
    }
};

} // namespace rts::gameplay
