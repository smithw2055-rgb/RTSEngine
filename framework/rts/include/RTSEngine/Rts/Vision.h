#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

class PackedVisibilityGrid final {
public:
    class const_iterator final {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::uint8_t;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = std::uint8_t;

        const_iterator() = default;
        const_iterator(const PackedVisibilityGrid* grid, std::size_t offset)
            : grid_(grid), offset_(offset) {}

        std::uint8_t operator*() const noexcept {
            return grid_ && grid_->test(offset_) ? 1u : 0u;
        }

        const_iterator& operator++() noexcept {
            ++offset_;
            return *this;
        }

        const_iterator operator++(int) noexcept {
            const auto copy = *this;
            ++*this;
            return copy;
        }

        friend bool operator==(
            const const_iterator& a,
            const const_iterator& b) noexcept {
            return a.grid_ == b.grid_ && a.offset_ == b.offset_;
        }

        friend bool operator!=(
            const const_iterator& a,
            const const_iterator& b) noexcept {
            return !(a == b);
        }

    private:
        const PackedVisibilityGrid* grid_{};
        std::size_t offset_{};
    };

    void assign(std::size_t bits, std::uint8_t value = 0u) {
        size_ = bits;
        words_.assign(wordCountFor(bits), value == 0u ? 0u : ~0ull);
        maskTail();
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t wordCount() const noexcept { return words_.size(); }
    std::size_t wordCapacity() const noexcept { return words_.capacity(); }

    bool test(std::size_t offset) const noexcept {
        if (offset >= size_) return false;
        return (words_[offset / 64u] & (1ull << (offset % 64u))) != 0;
    }

    bool set(std::size_t offset) noexcept {
        if (offset >= size_) return false;
        auto& word = words_[offset / 64u];
        const auto mask = 1ull << (offset % 64u);
        const bool changed = (word & mask) == 0;
        word |= mask;
        return changed;
    }

    void clearAll() noexcept {
        std::fill(words_.begin(), words_.end(), 0ull);
    }

    void exportBytes(std::vector<std::uint8_t>& output) const {
        output.resize(size_);
        for (std::size_t offset = 0; offset < size_; ++offset) {
            output[offset] = test(offset) ? 1u : 0u;
        }
    }

    const_iterator begin() const noexcept { return {this, 0}; }
    const_iterator end() const noexcept { return {this, size_}; }

private:
    static std::size_t wordCountFor(std::size_t bits) noexcept {
        return (bits + 63u) / 64u;
    }

    void maskTail() noexcept {
        if (words_.empty() || size_ % 64u == 0u) return;
        words_.back() &= (1ull << (size_ % 64u)) - 1ull;
    }

    std::size_t size_{};
    std::vector<std::uint64_t> words_;
};

struct TeamVisibilityLayer final {
    std::uint32_t teamId{};
    PackedVisibilityGrid current;
    PackedVisibilityGrid explored;
    std::uint32_t currentVisibleCells{};
    std::uint32_t exploredCells{};
};

class VisionRuntime final {
public:
    static constexpr std::uint32_t kMaximumTeams = 4096u;

    VisionRuntime(
        std::int32_t width = 32,
        std::int32_t height = 32)
        : width_(std::max<std::int32_t>(1, width)),
          height_(std::max<std::int32_t>(1, height)) {}

    std::int32_t width() const noexcept { return width_; }
    std::int32_t height() const noexcept { return height_; }
    std::size_t layerCount() const noexcept { return layers_.size(); }
    std::size_t offsetSetCount() const noexcept { return offsetSets_.size(); }

    std::size_t packedWordCount() const noexcept {
        std::size_t result = 0;
        for (const auto& layer : layers_) {
            result += layer.current.wordCount();
            result += layer.explored.wordCount();
        }
        return result;
    }

    std::size_t offsetPointCapacity() const noexcept {
        std::size_t result = 0;
        for (const auto& set : offsetSets_) result += set.points.capacity();
        return result;
    }

    const std::vector<TeamVisibilityLayer>& layers() const noexcept {
        return layers_;
    }

    bool visible(std::uint32_t teamId, GridPoint point) const noexcept {
        const auto* layer = findLayer(teamId);
        return layer && contains(point) && layer->current.test(index(point));
    }

    bool explored(std::uint32_t teamId, GridPoint point) const noexcept {
        const auto* layer = findLayer(teamId);
        return layer && contains(point) && layer->explored.test(index(point));
    }

    std::uint32_t currentVisibleCount(std::uint32_t teamId) const noexcept {
        const auto* layer = findLayer(teamId);
        return layer ? layer->currentVisibleCells : 0u;
    }

    std::uint32_t exploredCount(std::uint32_t teamId) const noexcept {
        const auto* layer = findLayer(teamId);
        return layer ? layer->exploredCells : 0u;
    }

    void rebuild(
        const ecs::World& world,
        const NavigationGrid& navigation) {
        if (navigation.width() != width_ || navigation.height() != height_) {
            return;
        }

        clearCurrent();
        world.eachRef<Team, VisionSource, Position>(
            [&](ecs::Entity entity,
                const Team& team,
                const VisionSource& source,
                const Position& position) {
                if (source.range < 0) return;

                auto& layer = ensureLayer(team.id);
                reveal(
                    navigation,
                    {position.x, position.y},
                    source.range,
                    world.try_get<BuildingFootprint>(entity),
                    layer);
            });
    }

    void clear() noexcept {
        layers_.clear();
    }

    void appendExploredHash(foundation::CanonicalHash& hash) const {
        hash.WriteI32(width_);
        hash.WriteI32(height_);
        hash.WriteU32(static_cast<std::uint32_t>(layers_.size()));
        for (const auto& layer : layers_) {
            hash.WriteU32(layer.teamId);
            hash.WriteU32(static_cast<std::uint32_t>(layer.explored.size()));
            for (const auto value : layer.explored) hash.WriteU8(value);
        }
    }

    void buildSnapshot(
        std::vector<TeamVisibilitySnapshot>& output) const {
        output.clear();
        output.reserve(layers_.size());
        for (const auto& layer : layers_) {
            TeamVisibilitySnapshot snapshot;
            snapshot.teamId = layer.teamId;
            snapshot.currentVisibleCells = layer.currentVisibleCells;
            snapshot.exploredCells = layer.exploredCells;
            layer.current.exportBytes(snapshot.current);
            layer.explored.exportBytes(snapshot.explored);
            output.push_back(std::move(snapshot));
        }
    }

    bool writeExploredState(foundation::BinaryWriter& writer) const {
        if (layers_.size() > kMaximumTeams) return false;
        writer.writeU32(static_cast<std::uint32_t>(layers_.size()));
        for (const auto& layer : layers_) {
            if (layer.explored.size() != cellCount()) return false;
            writer.writeU32(layer.teamId);
            writer.writeU32(static_cast<std::uint32_t>(layer.explored.size()));
            for (const auto value : layer.explored) {
                writer.writeU8(value);
            }
        }
        return true;
    }

    static bool readExploredState(
        foundation::BinaryReader& reader,
        std::int32_t width,
        std::int32_t height,
        VisionRuntime& output) {
        VisionRuntime candidate(width, height);
        std::uint32_t layerCount = 0;
        if (!reader.readU32(layerCount) || layerCount > kMaximumTeams) {
            return false;
        }

        candidate.layers_.reserve(layerCount);
        std::uint32_t previousTeam = 0;
        bool hasPreviousTeam = false;
        const auto cells = candidate.cellCount();
        for (std::uint32_t layerIndex = 0;
             layerIndex < layerCount;
             ++layerIndex) {
            TeamVisibilityLayer layer;
            std::uint32_t encodedCells = 0;
            if (!reader.readU32(layer.teamId) ||
                (hasPreviousTeam && layer.teamId <= previousTeam) ||
                !reader.readU32(encodedCells) || encodedCells != cells) {
                return false;
            }
            layer.current.assign(cells, 0u);
            layer.explored.assign(cells, 0u);
            for (std::size_t offset = 0; offset < cells; ++offset) {
                std::uint8_t value = 0;
                if (!reader.readU8(value) || value > 1u) return false;
                if (value != 0u) {
                    layer.explored.set(offset);
                    if (layer.exploredCells !=
                        std::numeric_limits<std::uint32_t>::max()) {
                        ++layer.exploredCells;
                    }
                }
            }
            candidate.layers_.push_back(std::move(layer));
            previousTeam = candidate.layers_.back().teamId;
            hasPreviousTeam = true;
        }
        output = std::move(candidate);
        return true;
    }

private:
    struct VisionOffsetSet final {
        std::int32_t range{};
        std::vector<GridPoint> points;
    };

    bool contains(GridPoint point) const noexcept {
        return point.x >= 0 && point.y >= 0 &&
               point.x < width_ && point.y < height_;
    }

    std::size_t cellCount() const noexcept {
        return static_cast<std::size_t>(width_) *
               static_cast<std::size_t>(height_);
    }

    std::size_t index(GridPoint point) const noexcept {
        return static_cast<std::size_t>(point.y) *
                   static_cast<std::size_t>(width_) +
               static_cast<std::size_t>(point.x);
    }

    const TeamVisibilityLayer* findLayer(
        std::uint32_t teamId) const noexcept {
        const auto found = std::lower_bound(
            layers_.begin(),
            layers_.end(),
            teamId,
            [](const TeamVisibilityLayer& layer, std::uint32_t value) {
                return layer.teamId < value;
            });
        return found != layers_.end() && found->teamId == teamId
            ? &*found
            : nullptr;
    }

    TeamVisibilityLayer& ensureLayer(std::uint32_t teamId) {
        auto found = std::lower_bound(
            layers_.begin(),
            layers_.end(),
            teamId,
            [](const TeamVisibilityLayer& layer, std::uint32_t value) {
                return layer.teamId < value;
            });
        if (found != layers_.end() && found->teamId == teamId) {
            return *found;
        }

        TeamVisibilityLayer layer;
        layer.teamId = teamId;
        layer.current.assign(cellCount(), 0u);
        layer.explored.assign(cellCount(), 0u);
        found = layers_.insert(found, std::move(layer));
        return *found;
    }

    void clearCurrent() noexcept {
        for (auto& layer : layers_) {
            layer.current.clearAll();
            layer.currentVisibleCells = 0;
        }
    }

    const std::vector<GridPoint>& offsetsFor(std::int32_t range) {
        auto found = std::lower_bound(
            offsetSets_.begin(),
            offsetSets_.end(),
            range,
            [](const VisionOffsetSet& set, std::int32_t value) {
                return set.range < value;
            });
        if (found != offsetSets_.end() && found->range == range) {
            return found->points;
        }

        VisionOffsetSet set;
        set.range = range;
        const auto diameter = static_cast<std::size_t>(range * 2 + 1);
        set.points.reserve(diameter * diameter);
        const auto squaredRange =
            static_cast<std::int64_t>(range) * range;
        for (std::int32_t y = -range; y <= range; ++y) {
            for (std::int32_t x = -range; x <= range; ++x) {
                const auto squaredDistance =
                    static_cast<std::int64_t>(x) * x +
                    static_cast<std::int64_t>(y) * y;
                if (squaredDistance <= squaredRange) {
                    set.points.push_back({x, y});
                }
            }
        }
        found = offsetSets_.insert(found, std::move(set));
        return found->points;
    }

    static bool insideFootprint(
        GridPoint point,
        const BuildingFootprint* footprint) noexcept {
        return footprint &&
               point.x >= footprint->origin.x &&
               point.y >= footprint->origin.y &&
               point.x < footprint->origin.x + footprint->width &&
               point.y < footprint->origin.y + footprint->height;
    }

    static bool lineVisible(
        const NavigationGrid& navigation,
        GridPoint source,
        GridPoint target,
        const BuildingFootprint* ownFootprint) noexcept {
        std::int32_t x = source.x;
        std::int32_t y = source.y;
        const auto dx = std::abs(target.x - source.x);
        const auto sx = source.x < target.x ? 1 : -1;
        const auto dy = -std::abs(target.y - source.y);
        const auto sy = source.y < target.y ? 1 : -1;
        auto error = dx + dy;

        while (x != target.x || y != target.y) {
            const auto doubled = error * 2;
            if (doubled >= dy) {
                error += dy;
                x += sx;
            }
            if (doubled <= dx) {
                error += dx;
                y += sy;
            }
            const GridPoint current{x, y};
            if (current == target) return true;
            if (!insideFootprint(current, ownFootprint) &&
                navigation.blocked(current)) {
                return false;
            }
        }
        return true;
    }

    void markVisible(
        TeamVisibilityLayer& layer,
        GridPoint point) noexcept {
        const auto offset = index(point);
        if (layer.current.set(offset) &&
            layer.currentVisibleCells !=
                std::numeric_limits<std::uint32_t>::max()) {
            ++layer.currentVisibleCells;
        }
        if (layer.explored.set(offset) &&
            layer.exploredCells !=
                std::numeric_limits<std::uint32_t>::max()) {
            ++layer.exploredCells;
        }
    }

    void reveal(
        const NavigationGrid& navigation,
        GridPoint source,
        std::int32_t range,
        const BuildingFootprint* ownFootprint,
        TeamVisibilityLayer& layer) {
        if (!contains(source)) return;
        const auto boundedRange = std::min<std::int32_t>(
            std::max<std::int32_t>(0, range),
            std::max(width_, height_));
        const auto& offsets = offsetsFor(boundedRange);
        for (const auto offset : offsets) {
            const GridPoint target{
                source.x + offset.x,
                source.y + offset.y};
            if (!contains(target)) continue;
            if (lineVisible(navigation, source, target, ownFootprint)) {
                markVisible(layer, target);
            }
        }
    }

    std::int32_t width_{};
    std::int32_t height_{};
    std::vector<TeamVisibilityLayer> layers_;
    std::vector<VisionOffsetSet> offsetSets_;
};

class VisionSystem final {
public:
    static void run(
        const ecs::World& world,
        const NavigationGrid& navigation,
        VisionRuntime& runtime) {
        runtime.rebuild(world, navigation);
    }
};

} // namespace rts::gameplay
