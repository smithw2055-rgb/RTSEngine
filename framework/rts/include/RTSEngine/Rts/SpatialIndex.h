#pragma once

#include <RTSEngine/Ecs/Entity.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts::gameplay {

struct SpatialIndexEntry final {
    ecs::Entity entity{};
    std::int32_t x{};
    std::int32_t y{};
};

class FixedGridSpatialIndex final {
public:
    FixedGridSpatialIndex(
        std::int32_t width = 64,
        std::int32_t height = 64,
        std::int32_t cellSize = 4)
        : width_(std::max<std::int32_t>(1, width)),
          height_(std::max<std::int32_t>(1, height)),
          cellSize_(std::max<std::int32_t>(1, cellSize)),
          columns_((width_ + cellSize_ - 1) / cellSize_),
          rows_((height_ + cellSize_ - 1) / cellSize_),
          buckets_(static_cast<std::size_t>(columns_) *
                   static_cast<std::size_t>(rows_)) {}

    std::int32_t width() const noexcept { return width_; }
    std::int32_t height() const noexcept { return height_; }
    std::int32_t cellSize() const noexcept { return cellSize_; }
    std::int32_t columns() const noexcept { return columns_; }
    std::int32_t rows() const noexcept { return rows_; }
    std::size_t entryCount() const noexcept { return entryCount_; }
    std::size_t bucketCount() const noexcept { return buckets_.size(); }

    std::size_t totalBucketCapacity() const noexcept {
        std::size_t capacity = 0;
        for (const auto& bucket : buckets_) capacity += bucket.capacity();
        return capacity;
    }

    void clear() noexcept {
        for (auto& bucket : buckets_) bucket.clear();
        entryCount_ = 0;
    }

    bool insert(ecs::Entity entity, std::int32_t x, std::int32_t y) {
        if (!entity.valid()) return false;
        const auto bucket = bucketIndex(x, y);
        if (bucket < 0) return false;
        buckets_[static_cast<std::size_t>(bucket)].push_back(
            {entity, x, y});
        ++entryCount_;
        return true;
    }

    void finalize() {
        entryCount_ = 0;
        for (auto& bucket : buckets_) {
            std::sort(
                bucket.begin(), bucket.end(),
                [](const SpatialIndexEntry& a, const SpatialIndexEntry& b) {
                    if (a.entity != b.entity) return a.entity < b.entity;
                    if (a.y != b.y) return a.y < b.y;
                    return a.x < b.x;
                });
            bucket.erase(
                std::unique(
                    bucket.begin(), bucket.end(),
                    [](const SpatialIndexEntry& a,
                       const SpatialIndexEntry& b) {
                        return a.entity == b.entity;
                    }),
                bucket.end());
            entryCount_ += bucket.size();
        }
    }

    template<class Visitor>
    void visitManhattan(
        std::int32_t x,
        std::int32_t y,
        std::int32_t range,
        Visitor&& visitor) const {
        if (range < 0 || buckets_.empty()) return;

        const auto minCellX = clampCoordinate(
            static_cast<std::int64_t>(x) - range, width_);
        const auto maxCellX = clampCoordinate(
            static_cast<std::int64_t>(x) + range, width_);
        const auto minCellY = clampCoordinate(
            static_cast<std::int64_t>(y) - range, height_);
        const auto maxCellY = clampCoordinate(
            static_cast<std::int64_t>(y) + range, height_);
        if (minCellX > maxCellX || minCellY > maxCellY) return;

        const auto minBucketX = minCellX / cellSize_;
        const auto maxBucketX = maxCellX / cellSize_;
        const auto minBucketY = minCellY / cellSize_;
        const auto maxBucketY = maxCellY / cellSize_;
        auto&& callback = visitor;

        for (std::int32_t bucketY = minBucketY;
             bucketY <= maxBucketY;
             ++bucketY) {
            for (std::int32_t bucketX = minBucketX;
                 bucketX <= maxBucketX;
                 ++bucketX) {
                const auto& bucket = buckets_[static_cast<std::size_t>(
                    bucketY * columns_ + bucketX)];
                for (const auto& entry : bucket) {
                    if (manhattan(entry.x, entry.y, x, y) <= range) {
                        callback(entry);
                    }
                }
            }
        }
    }

    void queryManhattan(
        std::int32_t x,
        std::int32_t y,
        std::int32_t range,
        std::vector<ecs::Entity>& result) const {
        result.clear();
        visitManhattan(
            x, y, range,
            [&result](const SpatialIndexEntry& entry) {
                result.push_back(entry.entity);
            });
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
    }

private:
    static std::int32_t clampCoordinate(
        std::int64_t value,
        std::int32_t extent) noexcept {
        if (value < 0) return 0;
        const auto maximum = static_cast<std::int64_t>(extent) - 1;
        if (value > maximum) return static_cast<std::int32_t>(maximum);
        return static_cast<std::int32_t>(value);
    }

    static std::int64_t manhattan(
        std::int32_t ax,
        std::int32_t ay,
        std::int32_t bx,
        std::int32_t by) noexcept {
        const auto dx = static_cast<std::int64_t>(ax) - bx;
        const auto dy = static_cast<std::int64_t>(ay) - by;
        return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    }

    std::int32_t bucketIndex(std::int32_t x, std::int32_t y) const noexcept {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return -1;
        return (y / cellSize_) * columns_ + (x / cellSize_);
    }

    std::int32_t width_{};
    std::int32_t height_{};
    std::int32_t cellSize_{};
    std::int32_t columns_{};
    std::int32_t rows_{};
    std::size_t entryCount_{};
    std::vector<std::vector<SpatialIndexEntry>> buckets_;
};

} // namespace rts::gameplay
