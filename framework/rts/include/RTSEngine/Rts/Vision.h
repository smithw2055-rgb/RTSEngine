#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct TeamVisibilityLayer final {
    std::uint32_t teamId{};
    std::vector<std::uint8_t> current;
    std::vector<std::uint8_t> explored;
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

    const std::vector<TeamVisibilityLayer>& layers() const noexcept {
        return layers_;
    }

    bool visible(std::uint32_t teamId, GridPoint point) const noexcept {
        const auto* layer = findLayer(teamId);
        return layer && contains(point) && layer->current[index(point)] != 0;
    }

    bool explored(std::uint32_t teamId, GridPoint point) const noexcept {
        const auto* layer = findLayer(teamId);
        return layer && contains(point) && layer->explored[index(point)] != 0;
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
        for (const auto entity : world.view<Team, VisionSource>()) {
            const auto* team = world.try_get<Team>(entity);
            const auto* source = world.try_get<VisionSource>(entity);
            const auto* position = world.try_get<Position>(entity);
            if (!team || !source || !position || source->range < 0) continue;

            auto& layer = ensureLayer(team->id);
            reveal(
                navigation,
                {position->x, position->y},
                source->range,
                world.try_get<BuildingFootprint>(entity),
                layer);
        }
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
            snapshot.current = layer.current;
            snapshot.explored = layer.explored;
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
                if (value > 1u) return false;
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
            layer.explored.resize(cells);
            for (auto& value : layer.explored) {
                if (!reader.readU8(value) || value > 1u) return false;
                if (value != 0u &&
                    layer.exploredCells !=
                        std::numeric_limits<std::uint32_t>::max()) {
                    ++layer.exploredCells;
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
    static bool pointLess(GridPoint a, GridPoint b) noexcept {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    }

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
            std::fill(layer.current.begin(), layer.current.end(), 0u);
            layer.currentVisibleCells = 0;
        }
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
        if (layer.current[offset] == 0u) {
            layer.current[offset] = 1u;
            if (layer.currentVisibleCells !=
                std::numeric_limits<std::uint32_t>::max()) {
                ++layer.currentVisibleCells;
            }
        }
        if (layer.explored[offset] == 0u) {
            layer.explored[offset] = 1u;
            if (layer.exploredCells !=
                std::numeric_limits<std::uint32_t>::max()) {
                ++layer.exploredCells;
            }
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
        const auto minimumX = std::max<std::int32_t>(0, source.x - boundedRange);
        const auto maximumX = std::min<std::int32_t>(
            width_ - 1, source.x + boundedRange);
        const auto minimumY = std::max<std::int32_t>(0, source.y - boundedRange);
        const auto maximumY = std::min<std::int32_t>(
            height_ - 1, source.y + boundedRange);
        const auto squaredRange =
            static_cast<std::int64_t>(boundedRange) * boundedRange;

        for (std::int32_t y = minimumY; y <= maximumY; ++y) {
            for (std::int32_t x = minimumX; x <= maximumX; ++x) {
                const auto dx = static_cast<std::int64_t>(x) - source.x;
                const auto dy = static_cast<std::int64_t>(y) - source.y;
                if (dx * dx + dy * dy > squaredRange) continue;
                const GridPoint target{x, y};
                if (lineVisible(
                        navigation, source, target, ownFootprint)) {
                    markVisible(layer, target);
                }
            }
        }
    }

    std::int32_t width_{};
    std::int32_t height_{};
    std::vector<TeamVisibilityLayer> layers_;
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
