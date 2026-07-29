#pragma once

#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::navigation {

struct GridPoint {
    std::int32_t x{};
    std::int32_t y{};

    friend bool operator==(GridPoint a, GridPoint b) noexcept {
        return a.x == b.x && a.y == b.y;
    }

    friend bool operator!=(GridPoint a, GridPoint b) noexcept {
        return !(a == b);
    }

    friend bool operator<(GridPoint a, GridPoint b) noexcept {
        return a.y < b.y || (a.y == b.y && a.x < b.x);
    }
};

inline std::int32_t ManhattanDistance(GridPoint a, GridPoint b) noexcept {
    const auto dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const auto dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx + dy;
}

struct NavigationGridState final {
    std::int32_t width{};
    std::int32_t height{};
    std::vector<std::uint8_t> blocked;
    std::uint64_t revision{};
};

class NavigationGrid {
public:
    static constexpr std::uint32_t kMaximumCells = 16u * 1024u * 1024u;
    static constexpr std::int32_t kChunkSize = 16;

    NavigationGrid(std::int32_t width = 32, std::int32_t height = 32)
        : width_(std::max<std::int32_t>(1, width)),
          height_(std::max<std::int32_t>(1, height)),
          blocked_(static_cast<std::size_t>(width_) *
                   static_cast<std::size_t>(height_), 0),
          chunkRevisions_(chunkCountFor(width_, height_), 0) {}

    NavigationGrid(const NavigationGrid&) = default;
    NavigationGrid(NavigationGrid&&) noexcept = default;

    NavigationGrid& operator=(const NavigationGrid& other) {
        if (this == &other) return *this;
        width_ = other.width_;
        height_ = other.height_;
        blocked_ = other.blocked_;
        revision_ = other.revision_;
        chunkRevisions_ = other.chunkRevisions_;
        changedChunksScratch_.clear();
        bumpCacheEpoch();
        return *this;
    }

    NavigationGrid& operator=(NavigationGrid&& other) noexcept {
        if (this == &other) return *this;
        width_ = other.width_;
        height_ = other.height_;
        blocked_ = std::move(other.blocked_);
        revision_ = other.revision_;
        chunkRevisions_ = std::move(other.chunkRevisions_);
        changedChunksScratch_.clear();
        bumpCacheEpoch();
        return *this;
    }

    std::int32_t width() const noexcept { return width_; }
    std::int32_t height() const noexcept { return height_; }
    std::uint64_t revision() const noexcept { return revision_; }
    std::uint64_t cacheEpoch() const noexcept { return cacheEpoch_; }
    std::int32_t chunkColumns() const noexcept {
        return (width_ + kChunkSize - 1) / kChunkSize;
    }
    std::int32_t chunkRows() const noexcept {
        return (height_ + kChunkSize - 1) / kChunkSize;
    }
    std::size_t chunkCount() const noexcept { return chunkRevisions_.size(); }

    bool contains(GridPoint point) const noexcept {
        return point.x >= 0 && point.y >= 0 &&
               point.x < width_ && point.y < height_;
    }

    bool blocked(GridPoint point) const noexcept {
        return !contains(point) || blocked_[index(point)] != 0;
    }

    std::uint32_t chunkIndex(GridPoint point) const noexcept {
        if (!contains(point)) return std::numeric_limits<std::uint32_t>::max();
        return static_cast<std::uint32_t>(
            (point.y / kChunkSize) * chunkColumns() +
            (point.x / kChunkSize));
    }

    std::uint64_t chunkRevision(std::uint32_t chunk) const noexcept {
        return chunk < chunkRevisions_.size() ? chunkRevisions_[chunk] : 0;
    }

    std::uint64_t chunkRevision(GridPoint point) const noexcept {
        return chunkRevision(chunkIndex(point));
    }

    bool setBlocked(GridPoint point, bool value) {
        if (!contains(point)) return false;
        const auto next = static_cast<std::uint8_t>(value ? 1 : 0);
        auto& cell = blocked_[index(point)];
        if (cell == next) return true;
        cell = next;
        const auto changedChunk = chunkIndex(point);
        commitTopologyChange(&changedChunk, 1);
        return true;
    }

    bool setBlockedBatch(const std::vector<GridPoint>& points, bool value) {
        for (const auto point : points) {
            if (!contains(point)) return false;
        }

        const auto next = static_cast<std::uint8_t>(value ? 1 : 0);
        changedChunksScratch_.clear();
        if (changedChunksScratch_.capacity() <
            std::min(points.size(), chunkRevisions_.size())) {
            changedChunksScratch_.reserve(
                std::min(points.size(), chunkRevisions_.size()));
        }
        for (const auto point : points) {
            auto& cell = blocked_[index(point)];
            if (cell == next) continue;
            cell = next;
            const auto chunk = chunkIndex(point);
            const auto found = std::lower_bound(
                changedChunksScratch_.begin(),
                changedChunksScratch_.end(),
                chunk);
            if (found == changedChunksScratch_.end() || *found != chunk) {
                changedChunksScratch_.insert(found, chunk);
            }
        }
        if (!changedChunksScratch_.empty()) {
            commitTopologyChange(
                changedChunksScratch_.data(), changedChunksScratch_.size());
        }
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
        rebuildChunkRevisions();
        bumpCacheEpoch();
        return true;
    }

    bool restore(NavigationGridState&& state) {
        if (!validate(state)) return false;
        width_ = state.width;
        height_ = state.height;
        blocked_ = std::move(state.blocked);
        revision_ = state.revision;
        rebuildChunkRevisions();
        bumpCacheEpoch();
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
    static std::size_t chunkCountFor(
        std::int32_t width,
        std::int32_t height) noexcept {
        const auto columns = (width + kChunkSize - 1) / kChunkSize;
        const auto rows = (height + kChunkSize - 1) / kChunkSize;
        return static_cast<std::size_t>(columns) *
               static_cast<std::size_t>(rows);
    }

    static void incrementRevision(std::uint64_t& value) noexcept {
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            value = 1;
        } else {
            ++value;
        }
    }

    void commitTopologyChange(
        const std::uint32_t* chunks,
        std::size_t count) noexcept {
        incrementRevision(revision_);
        for (std::size_t indexValue = 0; indexValue < count; ++indexValue) {
            const auto chunk = chunks[indexValue];
            if (chunk < chunkRevisions_.size()) {
                incrementRevision(chunkRevisions_[chunk]);
            }
        }
    }

    void rebuildChunkRevisions() {
        chunkRevisions_.assign(chunkCountFor(width_, height_), revision_);
        changedChunksScratch_.clear();
    }

    void bumpCacheEpoch() noexcept {
        if (cacheEpoch_ == std::numeric_limits<std::uint64_t>::max()) {
            cacheEpoch_ = 1;
        } else {
            ++cacheEpoch_;
        }
    }

    std::size_t index(GridPoint point) const noexcept {
        return static_cast<std::size_t>(point.y) *
                   static_cast<std::size_t>(width_) +
               static_cast<std::size_t>(point.x);
    }

    std::int32_t width_{};
    std::int32_t height_{};
    std::vector<std::uint8_t> blocked_;
    std::uint64_t revision_{};
    std::uint64_t cacheEpoch_{1};
    std::vector<std::uint64_t> chunkRevisions_;
    std::vector<std::uint32_t> changedChunksScratch_;
};

struct PathResult {
    bool found{};
    bool budgetExceeded{};
    std::uint32_t expandedNodes{};
    std::vector<GridPoint> points;
    std::vector<std::uint32_t> dependencyChunks;
};

class GridPathfinderScratch final {
public:
    std::size_t cellCapacity() const noexcept { return cost_.capacity(); }
    std::size_t heapCapacity() const noexcept { return heap_.capacity(); }
    std::size_t reverseCapacity() const noexcept { return reverse_.capacity(); }
    std::size_t dependencyCapacity() const noexcept {
        return dependencyChunks_.capacity();
    }

private:
    friend class GridPathfinder;

    struct HeapNode final {
        std::int32_t index{};
        std::int32_t cost{};
        std::int32_t heuristic{};
    };

    void prepare(std::size_t cells, std::size_t chunks) {
        if (cost_.size() < cells) {
            cost_.resize(cells);
            parent_.resize(cells);
            state_.resize(cells);
        }
        if (dependencyFlags_.size() < chunks) dependencyFlags_.resize(chunks);
        std::fill_n(cost_.begin(), cells, infinity());
        std::fill_n(parent_.begin(), cells, -1);
        std::fill_n(state_.begin(), cells, static_cast<std::uint8_t>(0));
        std::fill_n(
            dependencyFlags_.begin(), chunks, static_cast<std::uint8_t>(0));
        heap_.clear();
        reverse_.clear();
        dependencyChunks_.clear();
        if (heap_.capacity() < cells) heap_.reserve(cells);
        if (reverse_.capacity() < cells) reverse_.reserve(cells);
        if (dependencyChunks_.capacity() < chunks) {
            dependencyChunks_.reserve(chunks);
        }
    }

    void touchChunk(std::uint32_t chunk) {
        if (chunk >= dependencyFlags_.size() || dependencyFlags_[chunk] != 0) {
            return;
        }
        dependencyFlags_[chunk] = 1;
        dependencyChunks_.push_back(chunk);
    }

    static constexpr std::int32_t infinity() noexcept {
        return std::numeric_limits<std::int32_t>::max();
    }

    std::vector<std::int32_t> cost_;
    std::vector<std::int32_t> parent_;
    std::vector<std::uint8_t> state_;
    std::vector<HeapNode> heap_;
    std::vector<GridPoint> reverse_;
    std::vector<std::uint8_t> dependencyFlags_;
    std::vector<std::uint32_t> dependencyChunks_;
};

class GridPathfinder {
public:
    static PathResult find(
        const NavigationGrid& grid,
        GridPoint start,
        GridPoint goal,
        std::uint32_t nodeBudget = 4096) {
        thread_local GridPathfinderScratch scratch;
        return find(grid, start, goal, scratch, nodeBudget);
    }

    static PathResult find(
        const NavigationGrid& grid,
        GridPoint start,
        GridPoint goal,
        GridPathfinderScratch& scratch,
        std::uint32_t nodeBudget = 4096) {
        if (!grid.contains(goal)) return {};
        if (grid.blocked(goal)) {
            PathResult result;
            scratch.prepare(
                static_cast<std::size_t>(grid.width()) *
                    static_cast<std::size_t>(grid.height()),
                grid.chunkCount());
            scratch.touchChunk(grid.chunkIndex(goal));
            result.dependencyChunks = scratch.dependencyChunks_;
            return result;
        }
        return findInternal(
            grid,
            start,
            scratch,
            nodeBudget,
            [goal](GridPoint point) { return point == goal; },
            [goal](GridPoint point) {
                return ManhattanDistance(point, goal);
            });
    }

    static PathResult findToRange(
        const NavigationGrid& grid,
        GridPoint start,
        GridPoint target,
        std::int32_t range,
        std::uint32_t nodeBudget = 4096) {
        thread_local GridPathfinderScratch scratch;
        return findToRange(
            grid, start, target, range, scratch, nodeBudget);
    }

    static PathResult findToRange(
        const NavigationGrid& grid,
        GridPoint start,
        GridPoint target,
        std::int32_t range,
        GridPathfinderScratch& scratch,
        std::uint32_t nodeBudget = 4096) {
        if (!grid.contains(target)) return {};
        const auto boundedRange = std::max<std::int32_t>(0, range);
        return findInternal(
            grid,
            start,
            scratch,
            nodeBudget,
            [target, boundedRange](GridPoint point) {
                return ManhattanDistance(point, target) <= boundedRange;
            },
            [target, boundedRange](GridPoint point) {
                return std::max<std::int32_t>(
                    0, ManhattanDistance(point, target) - boundedRange);
            });
    }

private:
    using HeapNode = GridPathfinderScratch::HeapNode;

    static void copyDependencies(
        const GridPathfinderScratch& scratch,
        PathResult& result) {
        result.dependencyChunks = scratch.dependencyChunks_;
    }

    template<class GoalPredicate, class Heuristic>
    static PathResult findInternal(
        const NavigationGrid& grid,
        GridPoint start,
        GridPathfinderScratch& scratch,
        std::uint32_t nodeBudget,
        GoalPredicate&& goal,
        Heuristic&& heuristic) {
        PathResult result;
        if (!grid.contains(start)) return result;

        const auto width = grid.width();
        const auto total = static_cast<std::size_t>(
            static_cast<std::int64_t>(width) * grid.height());
        scratch.prepare(total, grid.chunkCount());
        scratch.touchChunk(grid.chunkIndex(start));
        if (grid.blocked(start)) {
            copyDependencies(scratch, result);
            return result;
        }
        if (goal(start)) {
            result.found = true;
            copyDependencies(scratch, result);
            return result;
        }

        const auto toIndex = [width](GridPoint point) {
            return static_cast<std::int32_t>(point.y * width + point.x);
        };
        const auto toPoint = [width](std::int32_t value) {
            return GridPoint{value % width, value / width};
        };

        const auto startIndex = toIndex(start);
        scratch.cost_[static_cast<std::size_t>(startIndex)] = 0;
        scratch.state_[static_cast<std::size_t>(startIndex)] = 1;
        pushHeap(scratch.heap_, {startIndex, 0, heuristic(start)});

        static constexpr GridPoint directions[] = {
            {0, -1}, {1, 0}, {0, 1}, {-1, 0}
        };

        std::int32_t reachedIndex = -1;
        while (!scratch.heap_.empty()) {
            const auto currentNode = popHeap(scratch.heap_);
            const auto currentIndex = static_cast<std::size_t>(
                currentNode.index);
            if (scratch.state_[currentIndex] == 2 ||
                scratch.cost_[currentIndex] != currentNode.cost) {
                continue;
            }

            ++result.expandedNodes;
            if (result.expandedNodes > nodeBudget) {
                result.budgetExceeded = true;
                copyDependencies(scratch, result);
                return result;
            }

            const auto point = toPoint(currentNode.index);
            scratch.touchChunk(grid.chunkIndex(point));
            if (goal(point)) {
                reachedIndex = currentNode.index;
                break;
            }

            scratch.state_[currentIndex] = 2;
            for (const auto direction : directions) {
                const GridPoint next{
                    point.x + direction.x,
                    point.y + direction.y};
                if (!grid.contains(next)) continue;
                scratch.touchChunk(grid.chunkIndex(next));
                if (grid.blocked(next)) continue;

                const auto nextIndex = toIndex(next);
                const auto nextOffset = static_cast<std::size_t>(nextIndex);
                if (scratch.state_[nextOffset] == 2) continue;

                const auto nextCost = currentNode.cost + 1;
                if (nextCost >= scratch.cost_[nextOffset]) continue;

                scratch.cost_[nextOffset] = nextCost;
                scratch.parent_[nextOffset] = currentNode.index;
                scratch.state_[nextOffset] = 1;
                pushHeap(
                    scratch.heap_,
                    {nextIndex, nextCost, heuristic(next)});
            }
        }

        if (reachedIndex < 0) {
            copyDependencies(scratch, result);
            return result;
        }
        for (auto cursor = reachedIndex; cursor != startIndex;) {
            if (cursor < 0) {
                copyDependencies(scratch, result);
                return result;
            }
            scratch.reverse_.push_back(toPoint(cursor));
            cursor = scratch.parent_[static_cast<std::size_t>(cursor)];
        }
        result.points.assign(
            scratch.reverse_.rbegin(), scratch.reverse_.rend());
        result.found = true;
        copyDependencies(scratch, result);
        return result;
    }

    static bool better(const HeapNode& a, const HeapNode& b) noexcept {
        const auto aTotal = a.cost + a.heuristic;
        const auto bTotal = b.cost + b.heuristic;
        if (aTotal != bTotal) return aTotal < bTotal;
        if (a.heuristic != b.heuristic) {
            return a.heuristic < b.heuristic;
        }
        return a.index < b.index;
    }

    static void pushHeap(
        std::vector<HeapNode>& heap,
        HeapNode value) {
        heap.push_back(value);
        auto child = heap.size() - 1;
        while (child > 0) {
            const auto parent = (child - 1) / 2;
            if (!better(heap[child], heap[parent])) break;
            std::swap(heap[child], heap[parent]);
            child = parent;
        }
    }

    static HeapNode popHeap(std::vector<HeapNode>& heap) {
        const auto result = heap.front();
        heap.front() = heap.back();
        heap.pop_back();
        if (heap.empty()) return result;

        std::size_t parent = 0;
        while (true) {
            const auto left = parent * 2 + 1;
            if (left >= heap.size()) break;
            const auto right = left + 1;
            auto bestChild = left;
            if (right < heap.size() && better(heap[right], heap[left])) {
                bestChild = right;
            }
            if (!better(heap[bestChild], heap[parent])) break;
            std::swap(heap[parent], heap[bestChild]);
            parent = bestChild;
        }
        return result;
    }
};

} // namespace rts::navigation
