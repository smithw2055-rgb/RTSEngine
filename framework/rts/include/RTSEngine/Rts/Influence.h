#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <RTSEngine/Rts/Vision.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

struct TeamInfluenceLayer final {
    std::uint32_t teamId{};
    std::vector<std::int32_t> friendly;
    std::vector<std::int32_t> threat;
    std::vector<std::int32_t> net;
    std::uint32_t friendlyActiveCells{};
    std::uint32_t threatActiveCells{};
    std::int32_t peakFriendly{};
    std::int32_t peakThreat{};
    std::int32_t minimumNet{};
    std::int32_t maximumNet{};
};

class InfluenceRuntime final {
public:
    InfluenceRuntime(
        std::int32_t width = 32,
        std::int32_t height = 32)
        : width_(std::max<std::int32_t>(1, width)),
          height_(std::max<std::int32_t>(1, height)) {}

    std::int32_t width() const noexcept { return width_; }
    std::int32_t height() const noexcept { return height_; }
    std::size_t layerCount() const noexcept { return layers_.size(); }

    const std::vector<TeamInfluenceLayer>& layers() const noexcept {
        return layers_;
    }

    std::int32_t friendly(
        std::uint32_t teamId,
        GridPoint point) const noexcept {
        return value(teamId, point, Channel::Friendly);
    }

    std::int32_t threat(
        std::uint32_t teamId,
        GridPoint point) const noexcept {
        return value(teamId, point, Channel::Threat);
    }

    std::int32_t net(
        std::uint32_t teamId,
        GridPoint point) const noexcept {
        return value(teamId, point, Channel::Net);
    }

    void rebuild(
        const ecs::World& world,
        const VisionRuntime& vision) {
        if (vision.width() != width_ || vision.height() != height_) return;

        collectTeams(world, vision);
        synchronizeLayers();
        clearValues();

        for (const auto entity : world.view<Position, Team, Health>()) {
            const auto* position = world.try_get<Position>(entity);
            const auto* team = world.try_get<Team>(entity);
            const auto* health = world.try_get<Health>(entity);
            if (!position || !team || !health || health->current <= 0) continue;

            const auto* armor = world.try_get<Armor>(entity);
            const auto* weapon = world.try_get<Weapon>(entity);
            const auto* footprint = world.try_get<BuildingFootprint>(entity);
            const auto source = sourcePoint(*position, footprint);
            const auto strength = sourceStrength(
                *health, armor, weapon, footprint);

            if (auto* ownLayer = findLayer(team->id)) {
                addKernel(
                    ownLayer->friendly,
                    source,
                    friendlyRadius(weapon, footprint),
                    strength);
            }

            if (!weapon || weapon->damage <= 0 || weapon->range < 0) continue;
            for (auto& observer : layers_) {
                if (observer.teamId == team->id ||
                    !visibleEntity(
                        vision,
                        observer.teamId,
                        *position,
                        footprint)) {
                    continue;
                }
                addKernel(
                    observer.threat,
                    source,
                    threatRadius(*weapon, footprint),
                    strength);
            }
        }

        finalizeLayers();
    }

    void clear() noexcept {
        layers_.clear();
        teamIds_.clear();
    }

    void buildSnapshot(
        std::vector<TeamInfluenceSnapshot>& output) const {
        output.clear();
        output.reserve(layers_.size());
        for (const auto& layer : layers_) {
            TeamInfluenceSnapshot snapshot;
            snapshot.teamId = layer.teamId;
            snapshot.friendlyActiveCells = layer.friendlyActiveCells;
            snapshot.threatActiveCells = layer.threatActiveCells;
            snapshot.peakFriendly = layer.peakFriendly;
            snapshot.peakThreat = layer.peakThreat;
            snapshot.minimumNet = layer.minimumNet;
            snapshot.maximumNet = layer.maximumNet;
            snapshot.friendly = layer.friendly;
            snapshot.threat = layer.threat;
            snapshot.net = layer.net;
            output.push_back(std::move(snapshot));
        }
    }

private:
    enum class Channel : std::uint8_t {
        Friendly,
        Threat,
        Net
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

    static std::int32_t clampI32(std::int64_t value) noexcept {
        return static_cast<std::int32_t>(std::max<std::int64_t>(
            std::numeric_limits<std::int32_t>::min(),
            std::min<std::int64_t>(
                std::numeric_limits<std::int32_t>::max(), value)));
    }

    static void saturatingAdd(
        std::int32_t& destination,
        std::int32_t value) noexcept {
        destination = clampI32(
            static_cast<std::int64_t>(destination) + value);
    }

    static GridPoint sourcePoint(
        const Position& position,
        const BuildingFootprint* footprint) noexcept {
        if (!footprint) return {position.x, position.y};
        return {
            footprint->origin.x + footprint->width / 2,
            footprint->origin.y + footprint->height / 2};
    }

    static std::int32_t sourceStrength(
        const Health& health,
        const Armor* armor,
        const Weapon* weapon,
        const BuildingFootprint* footprint) noexcept {
        std::int64_t strength = std::max<std::int32_t>(1, health.current);
        strength += static_cast<std::int64_t>(
            std::max<std::int32_t>(0, armor ? armor->value : 0)) * 8;
        if (weapon && weapon->damage > 0) {
            const auto cycle = std::max<std::uint32_t>(1, weapon->cooldownTicks);
            strength += static_cast<std::int64_t>(weapon->damage) * 1000 /
                        static_cast<std::int64_t>(cycle);
            strength += static_cast<std::int64_t>(
                std::max<std::int32_t>(0, weapon->range)) * 16;
        }
        if (footprint) {
            strength += static_cast<std::int64_t>(
                std::max<std::int32_t>(1, footprint->width)) *
                std::max<std::int32_t>(1, footprint->height) * 8;
        }
        return clampI32(std::max<std::int64_t>(1, strength));
    }

    static std::int32_t footprintRadius(
        const BuildingFootprint* footprint) noexcept {
        if (!footprint) return 0;
        return std::max<std::int32_t>(
            0,
            (std::max(footprint->width, footprint->height) - 1) / 2);
    }

    static std::int32_t friendlyRadius(
        const Weapon* weapon,
        const BuildingFootprint* footprint) noexcept {
        const auto base = weapon
            ? std::max<std::int32_t>(2, weapon->range + 3)
            : 2;
        return base + footprintRadius(footprint);
    }

    static std::int32_t threatRadius(
        const Weapon& weapon,
        const BuildingFootprint* footprint) noexcept {
        return std::max<std::int32_t>(1, weapon.range + 3) +
               footprintRadius(footprint);
    }

    static bool visibleEntity(
        const VisionRuntime& vision,
        std::uint32_t observerTeam,
        const Position& position,
        const BuildingFootprint* footprint) noexcept {
        if (!footprint) {
            return vision.visible(observerTeam, {position.x, position.y});
        }
        for (std::int32_t y = 0; y < footprint->height; ++y) {
            for (std::int32_t x = 0; x < footprint->width; ++x) {
                if (vision.visible(
                        observerTeam,
                        {footprint->origin.x + x,
                         footprint->origin.y + y})) {
                    return true;
                }
            }
        }
        return false;
    }

    void addKernel(
        std::vector<std::int32_t>& grid,
        GridPoint source,
        std::int32_t radius,
        std::int32_t strength) noexcept {
        if (!contains(source) || grid.size() != cellCount()) return;
        const auto boundedRadius = std::min<std::int32_t>(
            std::max<std::int32_t>(0, radius),
            width_ + height_);
        const auto minimumX = std::max<std::int32_t>(0, source.x - boundedRadius);
        const auto maximumX = std::min<std::int32_t>(
            width_ - 1, source.x + boundedRadius);
        const auto minimumY = std::max<std::int32_t>(0, source.y - boundedRadius);
        const auto maximumY = std::min<std::int32_t>(
            height_ - 1, source.y + boundedRadius);
        const auto denominator = static_cast<std::int64_t>(boundedRadius) + 1;

        for (std::int32_t y = minimumY; y <= maximumY; ++y) {
            for (std::int32_t x = minimumX; x <= maximumX; ++x) {
                const auto distance =
                    std::abs(x - source.x) + std::abs(y - source.y);
                if (distance > boundedRadius) continue;
                const auto numerator = static_cast<std::int64_t>(
                    boundedRadius - distance + 1);
                const auto contribution = std::max<std::int32_t>(
                    1,
                    clampI32(
                        static_cast<std::int64_t>(strength) * numerator /
                        denominator));
                saturatingAdd(grid[index({x, y})], contribution);
            }
        }
    }

    void collectTeams(
        const ecs::World& world,
        const VisionRuntime& vision) {
        teamIds_.clear();
        teamIds_.reserve(std::max(teamIds_.capacity(), vision.layerCount()));
        for (const auto entity : world.view<Team>()) {
            const auto* team = world.try_get<Team>(entity);
            if (team) teamIds_.push_back(team->id);
        }
        for (const auto& layer : vision.layers()) {
            teamIds_.push_back(layer.teamId);
        }
        std::sort(teamIds_.begin(), teamIds_.end());
        teamIds_.erase(
            std::unique(teamIds_.begin(), teamIds_.end()), teamIds_.end());
    }

    void synchronizeLayers() {
        layers_.erase(
            std::remove_if(
                layers_.begin(),
                layers_.end(),
                [&](const TeamInfluenceLayer& layer) {
                    return !std::binary_search(
                        teamIds_.begin(), teamIds_.end(), layer.teamId);
                }),
            layers_.end());
        for (const auto teamId : teamIds_) ensureLayer(teamId);
    }

    TeamInfluenceLayer& ensureLayer(std::uint32_t teamId) {
        auto found = std::lower_bound(
            layers_.begin(),
            layers_.end(),
            teamId,
            [](const TeamInfluenceLayer& layer, std::uint32_t value) {
                return layer.teamId < value;
            });
        if (found != layers_.end() && found->teamId == teamId) return *found;

        TeamInfluenceLayer layer;
        layer.teamId = teamId;
        layer.friendly.assign(cellCount(), 0);
        layer.threat.assign(cellCount(), 0);
        layer.net.assign(cellCount(), 0);
        return *layers_.insert(found, std::move(layer));
    }

    TeamInfluenceLayer* findLayer(std::uint32_t teamId) noexcept {
        auto found = std::lower_bound(
            layers_.begin(),
            layers_.end(),
            teamId,
            [](const TeamInfluenceLayer& layer, std::uint32_t value) {
                return layer.teamId < value;
            });
        return found != layers_.end() && found->teamId == teamId
            ? &*found
            : nullptr;
    }

    const TeamInfluenceLayer* findLayer(
        std::uint32_t teamId) const noexcept {
        const auto found = std::lower_bound(
            layers_.begin(),
            layers_.end(),
            teamId,
            [](const TeamInfluenceLayer& layer, std::uint32_t value) {
                return layer.teamId < value;
            });
        return found != layers_.end() && found->teamId == teamId
            ? &*found
            : nullptr;
    }

    void clearValues() noexcept {
        for (auto& layer : layers_) {
            std::fill(layer.friendly.begin(), layer.friendly.end(), 0);
            std::fill(layer.threat.begin(), layer.threat.end(), 0);
            std::fill(layer.net.begin(), layer.net.end(), 0);
            layer.friendlyActiveCells = 0;
            layer.threatActiveCells = 0;
            layer.peakFriendly = 0;
            layer.peakThreat = 0;
            layer.minimumNet = 0;
            layer.maximumNet = 0;
        }
    }

    void finalizeLayers() noexcept {
        for (auto& layer : layers_) {
            for (std::size_t cell = 0; cell < cellCount(); ++cell) {
                const auto friendlyValue = layer.friendly[cell];
                const auto threatValue = layer.threat[cell];
                layer.net[cell] = clampI32(
                    static_cast<std::int64_t>(friendlyValue) - threatValue);
                if (friendlyValue > 0) ++layer.friendlyActiveCells;
                if (threatValue > 0) ++layer.threatActiveCells;
                layer.peakFriendly = std::max(
                    layer.peakFriendly, friendlyValue);
                layer.peakThreat = std::max(layer.peakThreat, threatValue);
                layer.minimumNet = std::min(
                    layer.minimumNet, layer.net[cell]);
                layer.maximumNet = std::max(
                    layer.maximumNet, layer.net[cell]);
            }
        }
    }

    std::int32_t value(
        std::uint32_t teamId,
        GridPoint point,
        Channel channel) const noexcept {
        const auto* layer = findLayer(teamId);
        if (!layer || !contains(point)) return 0;
        const auto offset = index(point);
        switch (channel) {
        case Channel::Friendly:
            return layer->friendly[offset];
        case Channel::Threat:
            return layer->threat[offset];
        case Channel::Net:
            return layer->net[offset];
        }
        return 0;
    }

    std::int32_t width_{};
    std::int32_t height_{};
    std::vector<std::uint32_t> teamIds_;
    std::vector<TeamInfluenceLayer> layers_;
};

class InfluenceSystem final {
public:
    static void run(
        const ecs::World& world,
        const VisionRuntime& vision,
        InfluenceRuntime& runtime) {
        runtime.rebuild(world, vision);
    }
};

} // namespace rts::gameplay
