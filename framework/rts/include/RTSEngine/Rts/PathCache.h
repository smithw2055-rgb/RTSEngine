#pragma once

#include <RTSEngine/Rts/Navigation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct GridPathCacheLimits final {
    std::size_t maximumEntries{256};
    std::size_t maximumPoints{64u * 1024u};
};

struct GridPathCacheStats final {
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t insertions{};
    std::uint64_t evictions{};
    std::uint64_t invalidations{};
    std::uint64_t uncachedResults{};
};

struct GridPathCacheKey final {
    GridPoint start{};
    GridPoint goal{};
    std::uint32_t nodeBudget{4096};
};

class GridPathCache final {
public:
    explicit GridPathCache(GridPathCacheLimits limits = {})
        : limits_(limits) {
        entries_.reserve(limits_.maximumEntries);
    }

    const PathResult& resolve(
        const NavigationGrid& grid,
        GridPoint start,
        GridPoint goal,
        GridPathfinderScratch& scratch,
        std::uint32_t nodeBudget = 4096) {
        synchronize(grid);
        const GridPathCacheKey key{start, goal, nodeBudget};
        auto found = lowerBound(key);
        if (found != entries_.end() && equalKey(found->key, key)) {
            ++stats_.hits;
            found->lastUse = nextAccessSerial();
            return found->result;
        }

        ++stats_.misses;
        auto result = GridPathfinder::find(
            grid, start, goal, scratch, nodeBudget);
        const auto resultPoints = result.points.size();
        if (limits_.maximumEntries == 0 ||
            resultPoints > limits_.maximumPoints) {
            ++stats_.uncachedResults;
            transientResult_ = std::move(result);
            return transientResult_;
        }

        while (!entries_.empty() &&
               (entries_.size() >= limits_.maximumEntries ||
                exceedsPointBudget(resultPoints))) {
            evictOne();
        }
        if (entries_.size() >= limits_.maximumEntries ||
            exceedsPointBudget(resultPoints)) {
            ++stats_.uncachedResults;
            transientResult_ = std::move(result);
            return transientResult_;
        }

        Entry entry;
        entry.key = key;
        entry.result = std::move(result);
        entry.lastUse = nextAccessSerial();
        entry.insertionSerial = nextInsertionSerial();
        pointCount_ += entry.result.points.size();

        found = lowerBound(key);
        found = entries_.insert(found, std::move(entry));
        ++stats_.insertions;
        return found->result;
    }

    void clear() noexcept {
        entries_.clear();
        pointCount_ = 0;
        activeGrid_ = nullptr;
        activeWidth_ = 0;
        activeHeight_ = 0;
        activeRevision_ = 0;
        activeCacheEpoch_ = 0;
        bound_ = false;
        transientResult_ = {};
    }

    void resetStats() noexcept { stats_ = {}; }

    const GridPathCacheLimits& limits() const noexcept { return limits_; }
    const GridPathCacheStats& stats() const noexcept { return stats_; }
    std::size_t entryCount() const noexcept { return entries_.size(); }
    std::size_t pointCount() const noexcept { return pointCount_; }
    std::size_t entryCapacity() const noexcept { return entries_.capacity(); }
    std::uint64_t activeRevision() const noexcept { return activeRevision_; }

private:
    struct Entry final {
        GridPathCacheKey key{};
        PathResult result;
        std::uint64_t lastUse{};
        std::uint64_t insertionSerial{};
    };

    using Iterator = std::vector<Entry>::iterator;

    static bool lessPoint(GridPoint a, GridPoint b) noexcept {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    }

    static bool lessKey(
        const GridPathCacheKey& a,
        const GridPathCacheKey& b) noexcept {
        if (a.start != b.start) return lessPoint(a.start, b.start);
        if (a.goal != b.goal) return lessPoint(a.goal, b.goal);
        return a.nodeBudget < b.nodeBudget;
    }

    static bool equalKey(
        const GridPathCacheKey& a,
        const GridPathCacheKey& b) noexcept {
        return a.start == b.start && a.goal == b.goal &&
               a.nodeBudget == b.nodeBudget;
    }

    Iterator lowerBound(const GridPathCacheKey& key) {
        return std::lower_bound(
            entries_.begin(), entries_.end(), key,
            [](const Entry& entry, const GridPathCacheKey& value) {
                return lessKey(entry.key, value);
            });
    }

    void synchronize(const NavigationGrid& grid) {
        const bool changed = bound_ &&
            (activeGrid_ != &grid || activeWidth_ != grid.width() ||
             activeHeight_ != grid.height() ||
             activeRevision_ != grid.revision() ||
             activeCacheEpoch_ != grid.cacheEpoch());
        if (changed) {
            entries_.clear();
            pointCount_ = 0;
            ++stats_.invalidations;
        }
        if (!bound_ || changed) {
            activeGrid_ = &grid;
            activeWidth_ = grid.width();
            activeHeight_ = grid.height();
            activeRevision_ = grid.revision();
            activeCacheEpoch_ = grid.cacheEpoch();
            bound_ = true;
        }
    }

    bool exceedsPointBudget(std::size_t additional) const noexcept {
        return additional > limits_.maximumPoints ||
               pointCount_ > limits_.maximumPoints - additional;
    }

    void evictOne() {
        auto victim = entries_.begin();
        for (auto candidate = entries_.begin() + 1;
             candidate != entries_.end(); ++candidate) {
            if (candidate->lastUse < victim->lastUse ||
                (candidate->lastUse == victim->lastUse &&
                 (candidate->insertionSerial < victim->insertionSerial ||
                  (candidate->insertionSerial == victim->insertionSerial &&
                   lessKey(candidate->key, victim->key))))) {
                victim = candidate;
            }
        }
        pointCount_ -= victim->result.points.size();
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

    GridPathCacheLimits limits_;
    GridPathCacheStats stats_;
    const NavigationGrid* activeGrid_{};
    std::int32_t activeWidth_{};
    std::int32_t activeHeight_{};
    std::uint64_t activeRevision_{};
    std::uint64_t activeCacheEpoch_{};
    std::uint64_t accessSerial_{};
    std::uint64_t insertionSerial_{};
    std::size_t pointCount_{};
    bool bound_{};
    std::vector<Entry> entries_;
    PathResult transientResult_;
};

} // namespace rts::gameplay
