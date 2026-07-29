#pragma once

#include <RTSEngine/Rts/Navigation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct GridFlowFieldLimits final {
    std::size_t maximumFields{8};
    std::size_t maximumCells{2u * 1024u * 1024u};
};

struct GridFlowFieldStats final {
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t builds{};
    std::uint64_t visitedCells{};
    std::uint64_t peakReachableCells{};
    std::uint64_t evictions{};
    std::uint64_t invalidations{};
    std::uint64_t uncachedFields{};
    std::uint64_t pathExtractions{};
    std::uint64_t extractedPathPoints{};
    std::uint64_t extractionFailures{};
    std::uint64_t directAssignments{};
    std::uint64_t directSamples{};
    std::uint64_t directSampleFailures{};
};

class GridFlowField final {
public:
    static constexpr std::int32_t kUnreachable =
        std::numeric_limits<std::int32_t>::max();

    bool build(const NavigationGrid& grid, GridPoint goal) {
        width_ = grid.width();
        height_ = grid.height();
        goal_ = goal;
        ready_ = true;
        validGoal_ = grid.contains(goal) && !grid.blocked(goal);
        reachableCells_ = 0;
        frontier_.clear();

        if (!validGoal_) {
            distances_.clear();
            return false;
        }

        const auto cells = static_cast<std::size_t>(width_) *
                           static_cast<std::size_t>(height_);
        distances_.resize(cells);
        std::fill(distances_.begin(), distances_.end(), kUnreachable);
        if (frontier_.capacity() < cells) frontier_.reserve(cells);

        const auto goalIndex = index(goal);
        distances_[goalIndex] = 0;
        frontier_.push_back(static_cast<std::int32_t>(goalIndex));

        static constexpr GridPoint directions[] = {
            {0, -1}, {1, 0}, {0, 1}, {-1, 0}
        };
        std::size_t head = 0;
        while (head < frontier_.size()) {
            const auto currentIndex = frontier_[head++];
            const auto current = point(currentIndex);
            const auto nextDistance =
                distances_[static_cast<std::size_t>(currentIndex)] + 1;
            for (const auto direction : directions) {
                const GridPoint next{
                    current.x + direction.x,
                    current.y + direction.y};
                if (!grid.contains(next) || grid.blocked(next)) continue;
                const auto nextIndex = index(next);
                if (distances_[nextIndex] != kUnreachable) continue;
                distances_[nextIndex] = nextDistance;
                frontier_.push_back(static_cast<std::int32_t>(nextIndex));
            }
        }
        reachableCells_ = frontier_.size();
        return true;
    }

    bool extract(GridPoint start, std::vector<GridPoint>& path) const {
        path.clear();
        if (!ready_ || !validGoal_ || !contains(start) ||
            distances_.empty()) {
            return false;
        }
        auto current = start;
        auto currentDistance = distance(start);
        if (currentDistance == kUnreachable) return false;
        if (current == goal_) return true;
        if (path.capacity() < static_cast<std::size_t>(currentDistance)) {
            path.reserve(static_cast<std::size_t>(currentDistance));
        }

        while (current != goal_) {
            GridPoint next;
            if (!nextStep(current, next)) {
                path.clear();
                return false;
            }
            path.push_back(next);
            current = next;
            if (path.size() > distances_.size()) {
                path.clear();
                return false;
            }
        }
        return true;
    }

    bool nextStep(GridPoint current, GridPoint& next) const noexcept {
        if (!ready_ || !validGoal_ || !contains(current) ||
            distances_.empty()) {
            return false;
        }
        const auto currentDistance = distance(current);
        if (currentDistance == kUnreachable || currentDistance == 0) {
            return false;
        }
        static constexpr GridPoint directions[] = {
            {0, -1}, {1, 0}, {0, 1}, {-1, 0}
        };
        for (const auto direction : directions) {
            const GridPoint candidate{
                current.x + direction.x,
                current.y + direction.y};
            if (!contains(candidate)) continue;
            const auto candidateDistance = distance(candidate);
            if (candidateDistance != kUnreachable &&
                candidateDistance + 1 == currentDistance) {
                next = candidate;
                return true;
            }
        }
        return false;
    }

    bool ready() const noexcept { return ready_; }
    bool validGoal() const noexcept { return validGoal_; }
    GridPoint goal() const noexcept { return goal_; }
    std::int32_t width() const noexcept { return width_; }
    std::int32_t height() const noexcept { return height_; }
    std::size_t cellCount() const noexcept { return distances_.size(); }
    std::size_t cellCapacity() const noexcept { return distances_.capacity(); }
    std::size_t frontierCapacity() const noexcept { return frontier_.capacity(); }
    std::size_t reachableCells() const noexcept { return reachableCells_; }

    std::int32_t distance(GridPoint pointValue) const noexcept {
        if (!contains(pointValue) || distances_.empty()) return kUnreachable;
        return distances_[index(pointValue)];
    }

private:
    bool contains(GridPoint pointValue) const noexcept {
        return pointValue.x >= 0 && pointValue.y >= 0 &&
               pointValue.x < width_ && pointValue.y < height_;
    }

    std::size_t index(GridPoint pointValue) const noexcept {
        return static_cast<std::size_t>(pointValue.y) *
                   static_cast<std::size_t>(width_) +
               static_cast<std::size_t>(pointValue.x);
    }

    GridPoint point(std::int32_t indexValue) const noexcept {
        return {indexValue % width_, indexValue / width_};
    }

    std::int32_t width_{};
    std::int32_t height_{};
    GridPoint goal_{};
    bool ready_{};
    bool validGoal_{};
    std::size_t reachableCells_{};
    std::vector<std::int32_t> distances_;
    std::vector<std::int32_t> frontier_;
};

struct GridFlowDemand final {
    GridPoint goal{};
    std::uint32_t count{};
};

class GridFlowFieldCache final {
public:
    explicit GridFlowFieldCache(GridFlowFieldLimits limits = {})
        : limits_(limits) {
        entries_.reserve(limits_.maximumFields);
        demands_.reserve(32);
    }

    const GridFlowField& resolve(
        const NavigationGrid& grid,
        GridPoint goal) {
        synchronize(grid);
        auto found = lowerBound(goal);
        if (found != entries_.end() && found->goal == goal) {
            ++stats_.hits;
            found->lastUse = nextAccessSerial();
            return found->field;
        }

        ++stats_.misses;
        GridFlowField field;
        field.build(grid, goal);
        ++stats_.builds;
        stats_.visitedCells += field.reachableCells();
        stats_.peakReachableCells = std::max<std::uint64_t>(
            stats_.peakReachableCells, field.reachableCells());
        const auto fieldCells = field.cellCount();
        if (limits_.maximumFields == 0 ||
            fieldCells > limits_.maximumCells) {
            ++stats_.uncachedFields;
            transientField_ = std::move(field);
            return transientField_;
        }

        while (!entries_.empty() &&
               (entries_.size() >= limits_.maximumFields ||
                exceedsCellBudget(fieldCells))) {
            evictOne();
        }
        if (entries_.size() >= limits_.maximumFields ||
            exceedsCellBudget(fieldCells)) {
            ++stats_.uncachedFields;
            transientField_ = std::move(field);
            return transientField_;
        }

        Entry entry;
        entry.goal = goal;
        entry.field = std::move(field);
        entry.lastUse = nextAccessSerial();
        entry.insertionSerial = nextInsertionSerial();
        cellCount_ += entry.field.cellCount();
        found = lowerBound(goal);
        found = entries_.insert(found, std::move(entry));
        return found->field;
    }

    bool extractPath(
        const GridFlowField& field,
        GridPoint start,
        std::vector<GridPoint>& path) {
        ++stats_.pathExtractions;
        if (field.extract(start, path)) {
            stats_.extractedPathPoints += path.size();
            return true;
        }
        ++stats_.extractionFailures;
        return false;
    }

    bool assignDirect(
        const GridFlowField& field,
        GridPoint start) noexcept {
        ++stats_.pathExtractions;
        ++stats_.directAssignments;
        const auto distance = field.distance(start);
        if (distance == GridFlowField::kUnreachable) {
            ++stats_.extractionFailures;
            return false;
        }
        stats_.extractedPathPoints += static_cast<std::uint64_t>(distance);
        return true;
    }

    bool sample(
        const NavigationGrid& grid,
        GridPoint goal,
        GridPoint current,
        GridPoint& next) {
        ++stats_.directSamples;
        const auto& field = resolve(grid, goal);
        if (field.nextStep(current, next)) return true;
        ++stats_.directSampleFailures;
        return false;
    }

    void beginDemands() noexcept { demands_.clear(); }

    void addDemand(GridPoint goal) {
        auto found = std::lower_bound(
            demands_.begin(), demands_.end(), goal,
            [](const GridFlowDemand& demand, GridPoint value) {
                return lessPoint(demand.goal, value);
            });
        if (found != demands_.end() && found->goal == goal) {
            if (found->count != std::numeric_limits<std::uint32_t>::max()) {
                ++found->count;
            }
            return;
        }
        demands_.insert(found, {goal, 1u});
    }

    std::uint32_t demandCount(GridPoint goal) const noexcept {
        const auto found = std::lower_bound(
            demands_.begin(), demands_.end(), goal,
            [](const GridFlowDemand& demand, GridPoint value) {
                return lessPoint(demand.goal, value);
            });
        return found != demands_.end() && found->goal == goal
            ? found->count
            : 0u;
    }

    const std::vector<GridFlowDemand>& demands() const noexcept {
        return demands_;
    }

    void clear() noexcept {
        entries_.clear();
        demands_.clear();
        cellCount_ = 0;
        activeGrid_ = nullptr;
        activeWidth_ = 0;
        activeHeight_ = 0;
        activeRevision_ = 0;
        activeEpoch_ = 0;
        bound_ = false;
        transientField_ = {};
    }

    void resetStats() noexcept { stats_ = {}; }

    const GridFlowFieldLimits& limits() const noexcept { return limits_; }
    const GridFlowFieldStats& stats() const noexcept { return stats_; }
    std::size_t entryCount() const noexcept { return entries_.size(); }
    std::size_t cellCount() const noexcept { return cellCount_; }
    std::size_t entryCapacity() const noexcept { return entries_.capacity(); }
    std::size_t demandCapacity() const noexcept { return demands_.capacity(); }
    std::size_t demandGroupCount() const noexcept { return demands_.size(); }

private:
    struct Entry final {
        GridPoint goal{};
        GridFlowField field;
        std::uint64_t lastUse{};
        std::uint64_t insertionSerial{};
    };

    using Iterator = std::vector<Entry>::iterator;

    static bool lessPoint(GridPoint a, GridPoint b) noexcept {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    }

    Iterator lowerBound(GridPoint goal) {
        return std::lower_bound(
            entries_.begin(), entries_.end(), goal,
            [](const Entry& entry, GridPoint value) {
                return lessPoint(entry.goal, value);
            });
    }

    void synchronize(const NavigationGrid& grid) {
        const bool changed = bound_ &&
            (activeGrid_ != &grid || activeWidth_ != grid.width() ||
             activeHeight_ != grid.height() ||
             activeRevision_ != grid.revision() ||
             activeEpoch_ != grid.cacheEpoch());
        if (changed) {
            entries_.clear();
            cellCount_ = 0;
            ++stats_.invalidations;
        }
        if (!bound_ || changed) {
            activeGrid_ = &grid;
            activeWidth_ = grid.width();
            activeHeight_ = grid.height();
            activeRevision_ = grid.revision();
            activeEpoch_ = grid.cacheEpoch();
            bound_ = true;
        }
    }

    bool exceedsCellBudget(std::size_t additional) const noexcept {
        return additional > limits_.maximumCells ||
               cellCount_ > limits_.maximumCells - additional;
    }

    void evictOne() {
        auto victim = entries_.begin();
        for (auto candidate = entries_.begin() + 1;
             candidate != entries_.end(); ++candidate) {
            if (candidate->lastUse < victim->lastUse ||
                (candidate->lastUse == victim->lastUse &&
                 (candidate->insertionSerial < victim->insertionSerial ||
                  (candidate->insertionSerial == victim->insertionSerial &&
                   lessPoint(candidate->goal, victim->goal))))) {
                victim = candidate;
            }
        }
        cellCount_ -= victim->field.cellCount();
        entries_.erase(victim);
        ++stats_.evictions;
    }

    std::uint64_t nextAccessSerial() noexcept {
        if (accessSerial_ != std::numeric_limits<std::uint64_t>::max()) {
            ++accessSerial_;
        }
        return accessSerial_;
    }

    std::uint64_t nextInsertionSerial() noexcept {
        if (insertionSerial_ != std::numeric_limits<std::uint64_t>::max()) {
            ++insertionSerial_;
        }
        return insertionSerial_;
    }

    GridFlowFieldLimits limits_;
    GridFlowFieldStats stats_;
    const NavigationGrid* activeGrid_{};
    std::int32_t activeWidth_{};
    std::int32_t activeHeight_{};
    std::uint64_t activeRevision_{};
    std::uint64_t activeEpoch_{};
    std::uint64_t accessSerial_{};
    std::uint64_t insertionSerial_{};
    std::size_t cellCount_{};
    bool bound_{};
    std::vector<Entry> entries_;
    std::vector<GridFlowDemand> demands_;
    GridFlowField transientField_;
};

} // namespace rts::gameplay
