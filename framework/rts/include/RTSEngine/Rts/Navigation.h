#pragma once

#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct GridPoint {
    std::int32_t x{};
    std::int32_t y{};

    friend bool operator==(GridPoint a, GridPoint b) noexcept {
        return a.x == b.x && a.y == b.y;
    }
};

struct NavigationGridState final {
    std::int32_t width{};
    std::int32_t height{};
    std::vector<std::uint8_t> blocked;
    std::uint64_t revision{};
};

class NavigationGrid {
public:
    static constexpr std::uint32_t kMaximumCells = 16u * 1024u * 1024u;

    NavigationGrid(std::int32_t width = 32, std::int32_t height = 32)
        : width_(std::max<std::int32_t>(1, width)),
          height_(std::max<std::int32_t>(1, height)),
          blocked_(static_cast<std::size_t>(width_) *
                   static_cast<std::size_t>(height_), 0) {}

    std::int32_t width() const noexcept { return width_; }
    std::int32_t height() const noexcept { return height_; }
    std::uint64_t revision() const noexcept { return revision_; }

    bool contains(GridPoint point) const noexcept {
        return point.x >= 0 && point.y >= 0 &&
               point.x < width_ && point.y < height_;
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

    const std::vector<std::uint8_t>& blockers() const noexcept {
        return blocked_;
    }

    NavigationGridState snapshot() const {
        return {width_, height_, blocked_, revision_};
    }

    bool restore(const NavigationGridState& state) {
        if (!validate(state)) return false;
        width_ = state.width;
        height_ = state.height;
        blocked_ = state.blocked;
        revision_ = state.revision;
        return true;
    }

    bool restore(NavigationGridState&& state) {
        if (!validate(state)) return false;
        width_ = state.width;
        height_ = state.height;
        blocked_ = std::move(state.blocked);
        revision_ = state.revision;
        return true;
    }

    bool writeState(foundation::BinaryWriter& writer) const {
        const auto state = snapshot();
        if (!validate(state)) return false;
        writer.writeI32(state.width);
        writer.writeI32(state.height);
        writer.writeU64(state.revision);
        writer.writeU32(static_cast<std::uint32_t>(state.blocked.size()));
        for (const auto cell : state.blocked) writer.writeU8(cell);
        return true;
    }

    static bool readState(
        foundation::BinaryReader& reader,
        NavigationGridState& state,
        std::uint32_t maximumCells = kMaximumCells) {
        NavigationGridState candidate;
        std::uint32_t count = 0;
        if (!reader.readI32(candidate.width) ||
            !reader.readI32(candidate.height) ||
            !reader.readU64(candidate.revision) ||
            !reader.readU32(count) || count > maximumCells) {
            return false;
        }
        candidate.blocked.resize(count);
        for (auto& cell : candidate.blocked) {
            if (!reader.readU8(cell) || cell > 1) return false;
        }
        if (!validate(candidate, maximumCells)) return false;
        state = std::move(candidate);
        return true;
    }

    static bool validate(
        const NavigationGridState& state,
        std::uint32_t maximumCells = kMaximumCells) {
        if (state.width <= 0 || state.height <= 0) return false;
        const auto cells = static_cast<std::uint64_t>(state.width) *
                           static_cast<std::uint64_t>(state.height);
        if (cells == 0 || cells > maximumCells ||
            cells != state.blocked.size()) {
            return false;
        }
        return std::all_of(
            state.blocked.begin(), state.blocked.end(),
            [](std::uint8_t cell) { return cell <= 1; });
    }

private:
    std::size_t index(GridPoint point) const noexcept {
        return static_cast<std::size_t>(point.y) *
                   static_cast<std::size_t>(width_) +
               static_cast<std::size_t>(point.x);
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
        if (!grid.contains(start) || !grid.contains(goal) ||
            grid.blocked(start) || grid.blocked(goal)) {
            return result;
        }
        if (start == goal) {
            result.found = true;
            return result;
        }

        const auto width = grid.width();
        const auto total = static_cast<std::size_t>(
            static_cast<std::int64_t>(width) * grid.height());
        constexpr std::int32_t infinity =
            std::numeric_limits<std::int32_t>::max();
        std::vector<std::int32_t> cost(total, infinity);
        std::vector<std::int32_t> parent(total, -1);
        std::vector<std::uint8_t> open(total, 0);
        std::vector<std::uint8_t> closed(total, 0);

        const auto toIndex = [width](GridPoint point) {
            return static_cast<std::int32_t>(point.y * width + point.x);
        };
        const auto toPoint = [width](std::int32_t value) {
            return GridPoint{value % width, value / width};
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
            for (std::int32_t candidate = 0;
                 candidate < static_cast<std::int32_t>(total);
                 ++candidate) {
                if (!open[static_cast<std::size_t>(candidate)]) continue;
                const auto point = toPoint(candidate);
                const auto h = heuristic(point, goal);
                const auto f = cost[static_cast<std::size_t>(candidate)] + h;
                if (f < bestF ||
                    (f == bestF &&
                     (h < bestH ||
                      (h == bestH && candidate < current)))) {
                    current = candidate;
                    bestF = f;
                    bestH = h;
                }
            }

            if (current < 0) return result;
            if (++expanded > nodeBudget) {
                result.budgetExceeded = true;
                return result;
            }
            if (current == goalIndex) break;

            open[static_cast<std::size_t>(current)] = 0;
            closed[static_cast<std::size_t>(current)] = 1;
            const auto point = toPoint(current);
            for (const auto direction : directions) {
                const GridPoint next{
                    point.x + direction.x,
                    point.y + direction.y};
                if (!grid.contains(next) || grid.blocked(next)) continue;
                const auto nextIndex = toIndex(next);
                if (closed[static_cast<std::size_t>(nextIndex)]) continue;
                const auto nextCost =
                    cost[static_cast<std::size_t>(current)] + 1;
                if (nextCost < cost[static_cast<std::size_t>(nextIndex)]) {
                    cost[static_cast<std::size_t>(nextIndex)] = nextCost;
                    parent[static_cast<std::size_t>(nextIndex)] = current;
                    open[static_cast<std::size_t>(nextIndex)] = 1;
                }
            }
        }

        std::vector<GridPoint> reversed;
        for (auto cursor = goalIndex;
             cursor != startIndex;
             cursor = parent[static_cast<std::size_t>(cursor)]) {
            if (cursor < 0) return {};
            reversed.push_back(toPoint(cursor));
        }
        result.points.assign(reversed.rbegin(), reversed.rend());
        result.found = true;
        return result;
    }
};

} // namespace rts::gameplay
