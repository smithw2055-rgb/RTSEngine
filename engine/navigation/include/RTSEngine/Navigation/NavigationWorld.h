#pragma once

#include <RTSEngine/Navigation/GridNavigation.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::navigation {

enum class MovementDomain : std::uint8_t {
    Ground = 0,
    Air = 1,
    Naval = 2,
    Hover = 3
};

using MovementDomainMask = std::uint8_t;

constexpr MovementDomainMask DomainBit(MovementDomain domain) noexcept {
    return static_cast<MovementDomainMask>(1u << static_cast<unsigned>(domain));
}

constexpr MovementDomainMask kAllMovementDomains =
    DomainBit(MovementDomain::Ground) |
    DomainBit(MovementDomain::Air) |
    DomainBit(MovementDomain::Naval) |
    DomainBit(MovementDomain::Hover);

struct NavProfile final {
    static constexpr std::uint16_t kImpassableCost =
        std::numeric_limits<std::uint16_t>::max();

    std::uint32_t id{1};
    MovementDomain domain{MovementDomain::Ground};
    std::uint8_t footprintWidth{1};
    std::uint8_t footprintHeight{1};
    std::uint8_t clearanceRadius{};
    bool respectDynamicBlockers{true};
    bool respectReservations{true};
    std::uint16_t reservationCost{20};
    std::uint16_t tacticalWeight{1};
    std::uint16_t congestionWeight{1};
    std::array<std::uint16_t, 256> terrainCosts{};

    NavProfile() noexcept { terrainCosts.fill(10); }

    bool valid() const noexcept {
        return id != 0 && footprintWidth != 0 && footprintHeight != 0;
    }

    std::uint16_t minimumTerrainCost() const noexcept {
        std::uint16_t result = kImpassableCost;
        for (const auto value : terrainCosts) {
            if (value != 0 && value != kImpassableCost) {
                result = std::min(result, value);
            }
        }
        return result == kImpassableCost ? 1u : result;
    }
};

struct NavigationCellLayers final {
    std::uint8_t terrainType{};
    MovementDomainMask staticBlockedMask{};
    MovementDomainMask dynamicBlockedMask{};
    std::uint16_t reservationCount{};
    std::int16_t tacticalCost{};
    std::uint16_t congestionCost{};
};

struct NavigationWorldState final {
    std::int32_t width{};
    std::int32_t height{};
    std::vector<NavigationCellLayers> cells;
    std::uint64_t topologyRevision{};
    std::uint64_t reservationRevision{};
    std::uint64_t tacticalRevision{};
};

class NavigationWorld final {
public:
    static constexpr std::uint32_t kMaximumCells = 16u * 1024u * 1024u;
    static constexpr std::int32_t kChunkSize = 16;

    explicit NavigationWorld(
        std::int32_t width = 32,
        std::int32_t height = 32)
        : width_(std::max<std::int32_t>(1, width)),
          height_(std::max<std::int32_t>(1, height)),
          cells_(cellCount(width_, height_)),
          chunkRevisions_(chunkCountFor(width_, height_), 0) {}

    std::int32_t width() const noexcept { return width_; }
    std::int32_t height() const noexcept { return height_; }
    std::size_t size() const noexcept { return cells_.size(); }
    std::uint64_t topologyRevision() const noexcept {
        return topologyRevision_;
    }
    std::uint64_t reservationRevision() const noexcept {
        return reservationRevision_;
    }
    std::uint64_t tacticalRevision() const noexcept {
        return tacticalRevision_;
    }

    std::int32_t chunkColumns() const noexcept {
        return (width_ + kChunkSize - 1) / kChunkSize;
    }

    std::int32_t chunkRows() const noexcept {
        return (height_ + kChunkSize - 1) / kChunkSize;
    }

    std::size_t chunkCount() const noexcept {
        return chunkRevisions_.size();
    }

    bool contains(GridPoint point) const noexcept {
        return point.x >= 0 && point.y >= 0 &&
               point.x < width_ && point.y < height_;
    }

    const NavigationCellLayers* tryCell(GridPoint point) const noexcept {
        return contains(point) ? &cells_[index(point)] : nullptr;
    }

    NavigationCellLayers* tryCell(GridPoint point) noexcept {
        return contains(point) ? &cells_[index(point)] : nullptr;
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

    bool setTerrain(GridPoint point, std::uint8_t terrainType) noexcept {
        auto* cell = tryCell(point);
        if (!cell) return false;
        if (cell->terrainType == terrainType) return true;
        cell->terrainType = terrainType;
        touchTopology(point);
        return true;
    }

    bool setStaticBlocked(
        GridPoint point,
        MovementDomainMask mask,
        bool blocked) noexcept {
        return setMask(point, mask, blocked, true);
    }

    bool setDynamicBlocked(
        GridPoint point,
        MovementDomainMask mask,
        bool blocked) noexcept {
        return setMask(point, mask, blocked, false);
    }

    bool setReservationCount(
        GridPoint point,
        std::uint16_t count) noexcept {
        auto* cell = tryCell(point);
        if (!cell) return false;
        if (cell->reservationCount == count) return true;
        cell->reservationCount = count;
        incrementRevision(reservationRevision_);
        return true;
    }

    bool addReservation(GridPoint point, std::uint16_t amount = 1) noexcept {
        auto* cell = tryCell(point);
        if (!cell) return false;
        const auto remaining = static_cast<std::uint32_t>(
            std::numeric_limits<std::uint16_t>::max() -
            cell->reservationCount);
        cell->reservationCount = static_cast<std::uint16_t>(
            cell->reservationCount + std::min<std::uint32_t>(amount, remaining));
        incrementRevision(reservationRevision_);
        return true;
    }

    bool setTacticalCost(GridPoint point, std::int16_t cost) noexcept {
        auto* cell = tryCell(point);
        if (!cell) return false;
        if (cell->tacticalCost == cost) return true;
        cell->tacticalCost = cost;
        incrementRevision(tacticalRevision_);
        return true;
    }

    bool setCongestionCost(GridPoint point, std::uint16_t cost) noexcept {
        auto* cell = tryCell(point);
        if (!cell) return false;
        if (cell->congestionCost == cost) return true;
        cell->congestionCost = cost;
        incrementRevision(tacticalRevision_);
        return true;
    }

    void clearReservations() noexcept {
        bool changed = false;
        for (auto& cell : cells_) {
            changed = changed || cell.reservationCount != 0;
            cell.reservationCount = 0;
        }
        if (changed) incrementRevision(reservationRevision_);
    }

    void clearTacticalCosts() noexcept {
        bool changed = false;
        for (auto& cell : cells_) {
            changed = changed || cell.tacticalCost != 0 ||
                      cell.congestionCost != 0;
            cell.tacticalCost = 0;
            cell.congestionCost = 0;
        }
        if (changed) incrementRevision(tacticalRevision_);
    }

    bool passable(GridPoint anchor, const NavProfile& profile) const noexcept {
        if (!profile.valid()) return false;
        const auto left = static_cast<std::int32_t>(profile.footprintWidth - 1u) / 2;
        const auto right = static_cast<std::int32_t>(profile.footprintWidth) - 1 - left;
        const auto top = static_cast<std::int32_t>(profile.footprintHeight - 1u) / 2;
        const auto bottom = static_cast<std::int32_t>(profile.footprintHeight) - 1 - top;
        const auto clearance = static_cast<std::int32_t>(profile.clearanceRadius);
        const auto domain = DomainBit(profile.domain);

        for (std::int32_t y = anchor.y - top - clearance;
             y <= anchor.y + bottom + clearance; ++y) {
            for (std::int32_t x = anchor.x - left - clearance;
                 x <= anchor.x + right + clearance; ++x) {
                const GridPoint point{x, y};
                const auto* cell = tryCell(point);
                if (!cell) return false;
                if ((cell->staticBlockedMask & domain) != 0) return false;
                if (profile.respectDynamicBlockers &&
                    (cell->dynamicBlockedMask & domain) != 0) {
                    return false;
                }
                if (profile.terrainCosts[cell->terrainType] ==
                    NavProfile::kImpassableCost) {
                    return false;
                }
            }
        }
        return true;
    }

    std::uint32_t traversalCost(
        GridPoint point,
        const NavProfile& profile) const noexcept {
        if (!passable(point, profile)) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        const auto& cell = cells_[index(point)];
        std::uint64_t result = profile.terrainCosts[cell.terrainType];
        if (profile.respectReservations) {
            result += static_cast<std::uint64_t>(cell.reservationCount) *
                      profile.reservationCost;
        }
        const auto tactical = std::max<std::int32_t>(0, cell.tacticalCost);
        result += static_cast<std::uint64_t>(tactical) *
                  profile.tacticalWeight;
        result += static_cast<std::uint64_t>(cell.congestionCost) *
                  profile.congestionWeight;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(
            result, std::numeric_limits<std::uint32_t>::max() - 1u));
    }

    NavigationWorldState snapshot() const {
        return {
            width_, height_, cells_, topologyRevision_,
            reservationRevision_, tacticalRevision_};
    }

    bool restore(const NavigationWorldState& state) {
        if (!validate(state)) return false;
        width_ = state.width;
        height_ = state.height;
        cells_ = state.cells;
        topologyRevision_ = state.topologyRevision;
        reservationRevision_ = state.reservationRevision;
        tacticalRevision_ = state.tacticalRevision;
        chunkRevisions_.assign(
            chunkCountFor(width_, height_), topologyRevision_);
        return true;
    }

    static bool validate(
        const NavigationWorldState& state,
        std::uint32_t maximumCells = kMaximumCells) noexcept {
        if (state.width <= 0 || state.height <= 0) return false;
        const auto expected = static_cast<std::uint64_t>(state.width) *
                              static_cast<std::uint64_t>(state.height);
        return expected != 0 && expected <= maximumCells &&
               expected == state.cells.size();
    }

    std::uint64_t staticContentHash() const noexcept {
        std::uint64_t hash = 1469598103934665603ull;
        hashValue(hash, static_cast<std::uint32_t>(width_));
        hashValue(hash, static_cast<std::uint32_t>(height_));
        for (const auto& cell : cells_) {
            hashByte(hash, cell.terrainType);
            hashByte(hash, cell.staticBlockedMask);
        }
        return hash == 0 ? 1 : hash;
    }

private:
    static std::size_t cellCount(
        std::int32_t width,
        std::int32_t height) noexcept {
        return static_cast<std::size_t>(width) *
               static_cast<std::size_t>(height);
    }

    static std::size_t chunkCountFor(
        std::int32_t width,
        std::int32_t height) noexcept {
        const auto columns = (width + kChunkSize - 1) / kChunkSize;
        const auto rows = (height + kChunkSize - 1) / kChunkSize;
        return static_cast<std::size_t>(columns) *
               static_cast<std::size_t>(rows);
    }

    std::size_t index(GridPoint point) const noexcept {
        return static_cast<std::size_t>(point.y) *
                   static_cast<std::size_t>(width_) +
               static_cast<std::size_t>(point.x);
    }

    static void incrementRevision(std::uint64_t& revision) noexcept {
        revision = revision == std::numeric_limits<std::uint64_t>::max()
            ? 1 : revision + 1;
    }

    void touchTopology(GridPoint point) noexcept {
        incrementRevision(topologyRevision_);
        const auto chunk = chunkIndex(point);
        if (chunk < chunkRevisions_.size()) {
            incrementRevision(chunkRevisions_[chunk]);
        }
    }

    bool setMask(
        GridPoint point,
        MovementDomainMask mask,
        bool blocked,
        bool isStatic) noexcept {
        auto* cell = tryCell(point);
        if (!cell || (mask & ~kAllMovementDomains) != 0) return false;
        auto& value = isStatic
            ? cell->staticBlockedMask
            : cell->dynamicBlockedMask;
        const auto next = static_cast<MovementDomainMask>(
            blocked ? (value | mask) : (value & ~mask));
        if (next == value) return true;
        value = next;
        touchTopology(point);
        return true;
    }

    static void hashByte(std::uint64_t& hash, std::uint8_t value) noexcept {
        hash ^= value;
        hash *= 1099511628211ull;
    }

    static void hashValue(std::uint64_t& hash, std::uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hashByte(hash, static_cast<std::uint8_t>(value >> shift));
        }
    }

    std::int32_t width_{};
    std::int32_t height_{};
    std::vector<NavigationCellLayers> cells_;
    std::uint64_t topologyRevision_{};
    std::uint64_t reservationRevision_{};
    std::uint64_t tacticalRevision_{};
    std::vector<std::uint64_t> chunkRevisions_;
};

struct WeightedPathResult final {
    bool found{};
    bool budgetExceeded{};
    std::uint32_t expandedNodes{};
    std::uint32_t totalCost{};
    std::vector<GridPoint> points;
    std::vector<std::uint32_t> dependencyChunks;
};

class WeightedPathfinderScratch final {
public:
    std::size_t cellCapacity() const noexcept { return gCost_.capacity(); }
    std::size_t heapCapacity() const noexcept { return heap_.capacity(); }

private:
    friend class WeightedGridPathfinder;

    struct HeapNode final {
        std::int32_t index{};
        std::uint32_t g{};
        std::uint32_t h{};
    };

    void prepare(std::size_t cells, std::size_t chunks) {
        if (gCost_.size() < cells) {
            gCost_.resize(cells);
            parent_.resize(cells);
            state_.resize(cells);
        }
        if (chunkFlags_.size() < chunks) chunkFlags_.resize(chunks);
        std::fill_n(gCost_.begin(), cells, infinity());
        std::fill_n(parent_.begin(), cells, -1);
        std::fill_n(state_.begin(), cells, static_cast<std::uint8_t>(0));
        std::fill_n(chunkFlags_.begin(), chunks, static_cast<std::uint8_t>(0));
        heap_.clear();
        reverse_.clear();
        dependencies_.clear();
        if (heap_.capacity() < cells) heap_.reserve(cells);
        if (reverse_.capacity() < cells) reverse_.reserve(cells);
        if (dependencies_.capacity() < chunks) dependencies_.reserve(chunks);
    }

    void touchChunk(std::uint32_t chunk) {
        if (chunk >= chunkFlags_.size() || chunkFlags_[chunk] != 0) return;
        chunkFlags_[chunk] = 1;
        dependencies_.push_back(chunk);
    }

    static constexpr std::uint32_t infinity() noexcept {
        return std::numeric_limits<std::uint32_t>::max();
    }

    std::vector<std::uint32_t> gCost_;
    std::vector<std::int32_t> parent_;
    std::vector<std::uint8_t> state_;
    std::vector<HeapNode> heap_;
    std::vector<GridPoint> reverse_;
    std::vector<std::uint8_t> chunkFlags_;
    std::vector<std::uint32_t> dependencies_;
};

class WeightedGridPathfinder final {
public:
    static WeightedPathResult find(
        const NavigationWorld& world,
        GridPoint start,
        GridPoint goal,
        const NavProfile& profile,
        std::uint32_t expansionBudget = 65536u) {
        WeightedPathfinderScratch scratch;
        return find(world, start, goal, profile, scratch, expansionBudget);
    }

    static WeightedPathResult find(
        const NavigationWorld& world,
        GridPoint start,
        GridPoint goal,
        const NavProfile& profile,
        WeightedPathfinderScratch& scratch,
        std::uint32_t expansionBudget = 65536u) {
        WeightedPathResult result;
        if (!world.contains(start) || !world.contains(goal) ||
            !world.passable(start, profile) ||
            !world.passable(goal, profile)) {
            return result;
        }
        if (start == goal) {
            result.found = true;
            return result;
        }

        const auto width = world.width();
        const auto cells = world.size();
        scratch.prepare(cells, world.chunkCount());
        const auto startIndex = toIndex(start, width);
        const auto goalIndex = toIndex(goal, width);
        scratch.gCost_[startIndex] = 0;
        push(scratch, {
            startIndex, 0,
            heuristic(start, goal, profile.minimumTerrainCost())});

        static constexpr std::array<GridPoint, 4> offsets{{
            {0, -1}, {-1, 0}, {1, 0}, {0, 1}}};

        while (!scratch.heap_.empty()) {
            const auto current = pop(scratch);
            if (current.g != scratch.gCost_[current.index] ||
                scratch.state_[current.index] == 2u) {
                continue;
            }
            scratch.state_[current.index] = 2u;
            scratch.touchChunk(world.chunkIndex(fromIndex(current.index, width)));
            if (current.index == goalIndex) {
                result.found = true;
                result.totalCost = current.g;
                buildPath(
                    scratch, startIndex, goalIndex, width, result.points);
                result.dependencyChunks = scratch.dependencies_;
                std::sort(
                    result.dependencyChunks.begin(),
                    result.dependencyChunks.end());
                return result;
            }
            if (result.expandedNodes >= expansionBudget) {
                result.budgetExceeded = true;
                result.dependencyChunks = scratch.dependencies_;
                std::sort(
                    result.dependencyChunks.begin(),
                    result.dependencyChunks.end());
                return result;
            }
            ++result.expandedNodes;

            const auto point = fromIndex(current.index, width);
            for (const auto offset : offsets) {
                const GridPoint next{point.x + offset.x, point.y + offset.y};
                if (!world.contains(next)) continue;
                const auto step = world.traversalCost(next, profile);
                if (step == std::numeric_limits<std::uint32_t>::max()) continue;
                const auto nextIndex = toIndex(next, width);
                const auto nextCost = saturatingAdd(current.g, step);
                if (nextCost >= scratch.gCost_[nextIndex]) continue;
                scratch.gCost_[nextIndex] = nextCost;
                scratch.parent_[nextIndex] = current.index;
                scratch.state_[nextIndex] = 1u;
                scratch.touchChunk(world.chunkIndex(next));
                push(scratch, {
                    nextIndex,
                    nextCost,
                    heuristic(next, goal, profile.minimumTerrainCost())});
            }
        }

        result.dependencyChunks = scratch.dependencies_;
        std::sort(
            result.dependencyChunks.begin(), result.dependencyChunks.end());
        return result;
    }

private:
    using HeapNode = WeightedPathfinderScratch::HeapNode;

    static std::int32_t toIndex(GridPoint point, std::int32_t width) noexcept {
        return point.y * width + point.x;
    }

    static GridPoint fromIndex(
        std::int32_t index,
        std::int32_t width) noexcept {
        return {index % width, index / width};
    }

    static std::uint32_t heuristic(
        GridPoint point,
        GridPoint goal,
        std::uint16_t minimumCost) noexcept {
        const auto distance = static_cast<std::uint64_t>(
            ManhattanDistance(point, goal));
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(
            distance * minimumCost,
            std::numeric_limits<std::uint32_t>::max() - 1u));
    }

    static std::uint32_t saturatingAdd(
        std::uint32_t a,
        std::uint32_t b) noexcept {
        return b > std::numeric_limits<std::uint32_t>::max() - a
            ? std::numeric_limits<std::uint32_t>::max()
            : a + b;
    }

    static bool less(const HeapNode& a, const HeapNode& b) noexcept {
        const auto af = static_cast<std::uint64_t>(a.g) + a.h;
        const auto bf = static_cast<std::uint64_t>(b.g) + b.h;
        if (af != bf) return af < bf;
        if (a.h != b.h) return a.h < b.h;
        if (a.g != b.g) return a.g < b.g;
        return a.index < b.index;
    }

    static void push(WeightedPathfinderScratch& scratch, HeapNode node) {
        scratch.heap_.push_back(node);
        std::size_t child = scratch.heap_.size() - 1;
        while (child != 0) {
            const auto parent = (child - 1) / 2;
            if (!less(scratch.heap_[child], scratch.heap_[parent])) break;
            std::swap(scratch.heap_[child], scratch.heap_[parent]);
            child = parent;
        }
    }

    static HeapNode pop(WeightedPathfinderScratch& scratch) {
        const auto result = scratch.heap_.front();
        scratch.heap_.front() = scratch.heap_.back();
        scratch.heap_.pop_back();
        std::size_t parent = 0;
        while (parent < scratch.heap_.size()) {
            const auto left = parent * 2 + 1;
            if (left >= scratch.heap_.size()) break;
            const auto right = left + 1;
            auto child = left;
            if (right < scratch.heap_.size() &&
                less(scratch.heap_[right], scratch.heap_[left])) {
                child = right;
            }
            if (!less(scratch.heap_[child], scratch.heap_[parent])) break;
            std::swap(scratch.heap_[child], scratch.heap_[parent]);
            parent = child;
        }
        return result;
    }

    static void buildPath(
        WeightedPathfinderScratch& scratch,
        std::int32_t startIndex,
        std::int32_t goalIndex,
        std::int32_t width,
        std::vector<GridPoint>& output) {
        scratch.reverse_.clear();
        for (auto current = goalIndex;
             current != startIndex && current >= 0;
             current = scratch.parent_[current]) {
            scratch.reverse_.push_back(fromIndex(current, width));
        }
        output.assign(scratch.reverse_.rbegin(), scratch.reverse_.rend());
    }
};

struct NavPathRequest final {
    std::uint64_t requestId{};
    std::uint64_t submitTick{};
    std::uint32_t profileId{};
    GridPoint start{};
    GridPoint goal{};
    std::uint32_t expansionBudget{65536};
};

struct NavPathCompletion final {
    NavPathRequest request{};
    WeightedPathResult result{};
    std::uint64_t topologyRevision{};
    std::uint64_t reservationRevision{};
    std::uint64_t tacticalRevision{};

    bool validFor(const NavigationWorld& world) const noexcept {
        return topologyRevision == world.topologyRevision() &&
               reservationRevision == world.reservationRevision() &&
               tacticalRevision == world.tacticalRevision();
    }
};

class NavigationRequestQueue final {
public:
    bool submit(NavPathRequest request) {
        if (request.requestId == 0 || request.profileId == 0) return false;
        const auto iterator = std::lower_bound(
            pending_.begin(), pending_.end(), request.requestId,
            [](const NavPathRequest& value, std::uint64_t id) {
                return value.requestId < id;
            });
        if (iterator != pending_.end() &&
            iterator->requestId == request.requestId) {
            return false;
        }
        pending_.insert(iterator, request);
        return true;
    }

    void solvePending(
        const NavigationWorld& world,
        const std::vector<NavProfile>& profiles) {
        for (const auto& request : pending_) {
            const auto profile = std::find_if(
                profiles.begin(), profiles.end(),
                [&](const NavProfile& value) {
                    return value.id == request.profileId;
                });
            NavPathCompletion completion;
            completion.request = request;
            completion.topologyRevision = world.topologyRevision();
            completion.reservationRevision = world.reservationRevision();
            completion.tacticalRevision = world.tacticalRevision();
            if (profile != profiles.end() && profile->id == request.profileId) {
                completion.result = WeightedGridPathfinder::find(
                    world,
                    request.start,
                    request.goal,
                    *profile,
                    scratch_,
                    request.expansionBudget);
            }
            completed_.push_back(std::move(completion));
        }
        pending_.clear();
        std::sort(
            completed_.begin(), completed_.end(),
            [](const NavPathCompletion& a, const NavPathCompletion& b) {
                return a.request.requestId < b.request.requestId;
            });
    }

    std::vector<NavPathCompletion> commitReady(
        const NavigationWorld& world,
        std::uint64_t maximumSubmitTick) {
        std::vector<NavPathCompletion> result;
        auto iterator = completed_.begin();
        while (iterator != completed_.end()) {
            if (iterator->request.submitTick > maximumSubmitTick) {
                ++iterator;
                continue;
            }
            if (iterator->validFor(world)) {
                result.push_back(std::move(*iterator));
            }
            iterator = completed_.erase(iterator);
        }
        return result;
    }

    std::size_t pendingCount() const noexcept { return pending_.size(); }
    std::size_t completedCount() const noexcept { return completed_.size(); }

private:
    std::vector<NavPathRequest> pending_;
    std::vector<NavPathCompletion> completed_;
    WeightedPathfinderScratch scratch_;
};

struct FixedPosition2D final {
    static constexpr std::int32_t kOne = 1 << 16;
    std::int32_t x{};
    std::int32_t y{};

    static constexpr FixedPosition2D fromCell(GridPoint point) noexcept {
        return {point.x * kOne, point.y * kOne};
    }

    GridPoint cell() const noexcept {
        return {x / kOne, y / kOne};
    }
};

enum class Facing16 : std::uint8_t {
    East,
    EastSouthEast,
    SouthEast,
    SouthSouthEast,
    South,
    SouthSouthWest,
    SouthWest,
    WestSouthWest,
    West,
    WestNorthWest,
    NorthWest,
    NorthNorthWest,
    North,
    NorthNorthEast,
    NorthEast,
    EastNorthEast
};

struct FixedMoveResult final {
    FixedPosition2D position{};
    Facing16 facing{Facing16::South};
    bool arrived{};
};

class FixedMover final {
public:
    static FixedMoveResult advanceToward(
        FixedPosition2D current,
        FixedPosition2D target,
        std::uint32_t maximumStepQ16) noexcept {
        const auto dx = static_cast<std::int64_t>(target.x) - current.x;
        const auto dy = static_cast<std::int64_t>(target.y) - current.y;
        const auto ax = dx < 0 ? -dx : dx;
        const auto ay = dy < 0 ? -dy : dy;
        const auto extent = std::max(ax, ay);
        if (extent == 0 || maximumStepQ16 == 0) {
            return {current, facingFor(dx, dy), extent == 0};
        }
        if (extent <= maximumStepQ16) {
            return {target, facingFor(dx, dy), true};
        }
        const auto nextX = static_cast<std::int64_t>(current.x) +
            dx * maximumStepQ16 / extent;
        const auto nextY = static_cast<std::int64_t>(current.y) +
            dy * maximumStepQ16 / extent;
        return {
            {clampI32(nextX), clampI32(nextY)},
            facingFor(dx, dy),
            false};
    }

private:
    static std::int32_t clampI32(std::int64_t value) noexcept {
        return static_cast<std::int32_t>(std::max<std::int64_t>(
            std::numeric_limits<std::int32_t>::min(),
            std::min<std::int64_t>(
                std::numeric_limits<std::int32_t>::max(), value)));
    }

    static Facing16 facingFor(std::int64_t dx, std::int64_t dy) noexcept {
        if (dx == 0 && dy == 0) return Facing16::South;
        const auto ax = dx < 0 ? -dx : dx;
        const auto ay = dy < 0 ? -dy : dy;
        if (ay * 5 <= ax * 2) return dx >= 0 ? Facing16::East : Facing16::West;
        if (ax * 5 <= ay * 2) return dy >= 0 ? Facing16::South : Facing16::North;
        if (dx >= 0 && dy >= 0) return Facing16::SouthEast;
        if (dx < 0 && dy >= 0) return Facing16::SouthWest;
        if (dx < 0 && dy < 0) return Facing16::NorthWest;
        return Facing16::NorthEast;
    }
};

enum class FormationKind : std::uint8_t {
    Line,
    Column,
    Box,
    Wedge
};

struct FormationSlot final {
    std::uint64_t entityId{};
    std::int32_t offsetXQ16{};
    std::int32_t offsetYQ16{};
};

class FormationPlanner final {
public:
    static std::vector<FormationSlot> assign(
        FormationKind kind,
        std::vector<std::uint64_t> entityIds,
        std::int32_t spacingQ16 = FixedPosition2D::kOne) {
        entityIds.erase(
            std::remove(entityIds.begin(), entityIds.end(), 0),
            entityIds.end());
        std::sort(entityIds.begin(), entityIds.end());
        entityIds.erase(
            std::unique(entityIds.begin(), entityIds.end()),
            entityIds.end());
        std::vector<FormationSlot> result;
        result.reserve(entityIds.size());
        const auto count = static_cast<std::int32_t>(entityIds.size());
        const auto columns = std::max<std::int32_t>(
            1, integerCeilingSqrt(std::max(1, count)));
        for (std::int32_t index = 0; index < count; ++index) {
            std::int32_t x = 0;
            std::int32_t y = 0;
            switch (kind) {
            case FormationKind::Line:
                x = index - (count - 1) / 2;
                break;
            case FormationKind::Column:
                y = index;
                break;
            case FormationKind::Box:
                x = index % columns - (columns - 1) / 2;
                y = index / columns;
                break;
            case FormationKind::Wedge: {
                const auto row = integerCeilingSqrt(index + 1) - 1;
                const auto first = row * row;
                const auto position = index - first;
                x = position - row;
                y = row;
                break;
            }
            }
            result.push_back({
                entityIds[static_cast<std::size_t>(index)],
                x * spacingQ16,
                y * spacingQ16});
        }
        return result;
    }

private:
    static std::int32_t integerCeilingSqrt(std::int32_t value) noexcept {
        std::int32_t result = 0;
        while (result * result < value) ++result;
        return result;
    }
};

} // namespace rts::navigation
