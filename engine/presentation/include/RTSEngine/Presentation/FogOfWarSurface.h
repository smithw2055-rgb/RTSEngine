#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::presentation {

enum class FogCellState : std::uint8_t {
    Unexplored,
    Explored,
    Visible
};

struct FogDirtyRect final {
    std::int32_t minimumX{};
    std::int32_t minimumY{};
    std::int32_t maximumX{-1};
    std::int32_t maximumY{-1};

    bool valid() const noexcept {
        return minimumX <= maximumX && minimumY <= maximumY;
    }
};

class FogOfWarSurface final {
public:
    FogOfWarSurface(
        std::int32_t width = 1,
        std::int32_t height = 1)
        : width_(std::max<std::int32_t>(1, width)),
          height_(std::max<std::int32_t>(1, height)),
          cells_(static_cast<std::size_t>(width_) *
                 static_cast<std::size_t>(height_),
                 FogCellState::Unexplored) {}

    std::int32_t width() const noexcept { return width_; }
    std::int32_t height() const noexcept { return height_; }

    bool contains(std::int32_t x, std::int32_t y) const noexcept {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }

    FogCellState state(std::int32_t x, std::int32_t y) const noexcept {
        return contains(x, y) ? cells_[index(x, y)]
                              : FogCellState::Unexplored;
    }

    bool updateVisibility(
        const std::vector<std::uint8_t>& visible,
        bool preserveExploration = true) {
        if (visible.size() != cells_.size()) return false;
        for (std::int32_t y = 0; y < height_; ++y) {
            for (std::int32_t x = 0; x < width_; ++x) {
                const auto offset = index(x, y);
                const auto previous = cells_[offset];
                FogCellState next = visible[offset] != 0
                    ? FogCellState::Visible
                    : (preserveExploration &&
                               previous != FogCellState::Unexplored
                           ? FogCellState::Explored
                           : FogCellState::Unexplored);
                if (next != previous) {
                    cells_[offset] = next;
                    includeDirty(x, y);
                }
            }
        }
        return true;
    }

    void revealCircle(
        std::int32_t centerX,
        std::int32_t centerY,
        std::int32_t radius,
        FogCellState revealAs = FogCellState::Visible) {
        radius = std::max<std::int32_t>(0, radius);
        const auto radiusSquared = static_cast<std::int64_t>(radius) * radius;
        for (std::int32_t y = centerY - radius;
             y <= centerY + radius; ++y) {
            for (std::int32_t x = centerX - radius;
                 x <= centerX + radius; ++x) {
                if (!contains(x, y)) continue;
                const auto dx = static_cast<std::int64_t>(x) - centerX;
                const auto dy = static_cast<std::int64_t>(y) - centerY;
                if (dx * dx + dy * dy > radiusSquared) continue;
                auto& value = cells_[index(x, y)];
                if (value != revealAs) {
                    value = revealAs;
                    includeDirty(x, y);
                }
            }
        }
    }

    void clear(FogCellState value = FogCellState::Unexplored) noexcept {
        if (std::all_of(
                cells_.begin(), cells_.end(),
                [&](FogCellState cell) { return cell == value; })) {
            return;
        }
        std::fill(cells_.begin(), cells_.end(), value);
        dirty_ = {0, 0, width_ - 1, height_ - 1};
    }

    FogDirtyRect dirtyRect() const noexcept { return dirty_; }

    FogDirtyRect consumeDirtyRect() noexcept {
        const auto result = dirty_;
        dirty_ = {};
        return result;
    }

    std::vector<std::uint8_t> buildAlphaTexture(
        std::uint8_t unexploredAlpha = 255,
        std::uint8_t exploredAlpha = 150,
        std::uint8_t visibleAlpha = 0) const {
        std::vector<std::uint8_t> result;
        result.reserve(cells_.size());
        for (const auto cell : cells_) {
            switch (cell) {
            case FogCellState::Unexplored:
                result.push_back(unexploredAlpha);
                break;
            case FogCellState::Explored:
                result.push_back(exploredAlpha);
                break;
            case FogCellState::Visible:
                result.push_back(visibleAlpha);
                break;
            }
        }
        return result;
    }

    const std::vector<FogCellState>& cells() const noexcept { return cells_; }

private:
    std::size_t index(std::int32_t x, std::int32_t y) const noexcept {
        return static_cast<std::size_t>(y) *
                   static_cast<std::size_t>(width_) +
               static_cast<std::size_t>(x);
    }

    void includeDirty(std::int32_t x, std::int32_t y) noexcept {
        if (!dirty_.valid()) {
            dirty_ = {x, y, x, y};
            return;
        }
        dirty_.minimumX = std::min(dirty_.minimumX, x);
        dirty_.minimumY = std::min(dirty_.minimumY, y);
        dirty_.maximumX = std::max(dirty_.maximumX, x);
        dirty_.maximumY = std::max(dirty_.maximumY, y);
    }

    std::int32_t width_{};
    std::int32_t height_{};
    std::vector<FogCellState> cells_;
    FogDirtyRect dirty_{};
};

} // namespace rts::presentation
