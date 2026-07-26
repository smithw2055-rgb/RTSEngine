#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/Combat.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/VisionTypes.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::gameplay {

using ConstructionId = std::uint32_t;
using ProductionId = std::uint32_t;

struct ResourceLedger {
    std::int32_t available{};
    std::int32_t reserved{};
    std::int32_t spent{};

    bool reserve(std::int32_t amount) noexcept {
        if (amount < 0 || available < amount) return false;
        available -= amount;
        reserved += amount;
        return true;
    }

    bool commit(std::int32_t amount) noexcept {
        if (amount < 0 || reserved < amount) return false;
        reserved -= amount;
        spent += amount;
        return true;
    }

    bool release(std::int32_t amount) noexcept {
        if (amount < 0 || reserved < amount) return false;
        reserved -= amount;
        available += amount;
        return true;
    }
};

struct BuildingDefinition {
    std::uint32_t id{};
    std::int32_t cost{};
    std::uint32_t buildTicks{1};
    std::int32_t width{1};
    std::int32_t height{1};
    bool producer{};
    CombatStats combat{};
    std::int32_t visionRange{8};
};

struct BuildingFootprint {
    GridPoint origin{};
    std::int32_t width{1};
    std::int32_t height{1};
};

struct ConstructionSite {
    ConstructionId id{};
    std::uint32_t definitionId{};
    std::int32_t reservedCost{};
    std::uint32_t progressTicks{};
    std::uint32_t requiredTicks{1};
    bool producer{};
    std::uint32_t ownerTeam{};
    std::uint32_t baseRequiredTicks{1};
};

struct Building {
    std::uint32_t definitionId{};
    bool producer{};
};

struct ProductionItem {
    ProductionId id{};
    std::uint32_t unitDefinitionId{};
    std::int32_t reservedCost{};
    std::uint32_t progressTicks{};
    std::uint32_t requiredTicks{1};
    std::uint32_t baseRequiredTicks{1};
};

struct ProductionQueue {
    std::vector<ProductionItem> items;
};

struct RallyPoint {
    GridPoint point{};
};

enum class BuildFailure : std::uint8_t {
    None,
    InvalidDefinition,
    OutOfBounds,
    Occupied,
    BlocksRequiredPath,
    InsufficientResources,
    UnknownConstruction
};

struct BuildResult {
    bool accepted{};
    BuildFailure failure{BuildFailure::None};
    ConstructionId constructionId{};
};

class BaseBuildingRuntime {
public:
    BaseBuildingRuntime(ResourceLedger& ledger, NavigationGrid& navigation)
        : ledger_(ledger), navigation_(navigation) {}

    BuildResult begin(const ecs::SystemContext& context,
                      ecs::EntityCommandBuffer& commands,
                      const BuildingDefinition& definition,
                      GridPoint origin,
                      GridPoint requiredPathStart,
                      GridPoint requiredPathGoal,
                      std::uint32_t ownerTeam = 0,
                      std::uint32_t baseBuildTicks = 0) {
        if (!valid(definition)) {
            return {false, BuildFailure::InvalidDefinition, 0};
        }

        const BuildingFootprint footprint{
            origin, definition.width, definition.height};
        const auto placementFailure =
            validatePlacement(footprint, requiredPathStart, requiredPathGoal);
        if (placementFailure != BuildFailure::None) {
            return {false, placementFailure, 0};
        }
        if (!ledger_.reserve(definition.cost)) {
            return {false, BuildFailure::InsufficientResources, 0};
        }

        const ConstructionId id = ++nextConstructionId_;
        const auto requiredTicks =
            std::max<std::uint32_t>(1, definition.buildTicks);
        const auto sourceTicks = std::max<std::uint32_t>(
            1, baseBuildTicks == 0 ? definition.buildTicks : baseBuildTicks);
        setBlocked(footprint, true);
        const auto deferred = commands.create(context);
        commands.add(context, deferred, footprint);
        commands.add(context, deferred, VisionSource{
            std::max<std::int32_t>(0, definition.visionRange)});
        commands.add(context, deferred, ConstructionSite{
            id,
            definition.id,
            definition.cost,
            0,
            requiredTicks,
            definition.producer,
            ownerTeam,
            sourceTicks
        });
        return {true, BuildFailure::None, id};
    }

    BuildFailure cancel(const ecs::SystemContext& context,
                        ecs::EntityCommandBuffer& commands,
                        ecs::World& world,
                        ConstructionId id) {
        for (const auto entity :
             world.view<ConstructionSite, BuildingFootprint>()) {
            const auto* site = world.try_get<ConstructionSite>(entity);
            const auto* footprint =
                world.try_get<BuildingFootprint>(entity);
            if (!site || !footprint || site->id != id) continue;
            releaseFootprint(*footprint);
            ledger_.release(site->reservedCost);
            commands.destroy(context, entity);
            return BuildFailure::None;
        }
        return BuildFailure::UnknownConstruction;
    }

    void advance(const ecs::SystemContext& context,
                 ecs::EntityCommandBuffer& commands,
                 ecs::World& world) {
        for (const auto entity :
             world.view<ConstructionSite, BuildingFootprint>()) {
            auto* site = world.try_get<ConstructionSite>(entity);
            const auto* footprint =
                world.try_get<BuildingFootprint>(entity);
            if (!site || !footprint) continue;
            ++site->progressTicks;
            if (site->progressTicks < site->requiredTicks) continue;

            ledger_.commit(site->reservedCost);
            commands.add(
                context, entity,
                Building{site->definitionId, site->producer});
            if (site->producer) {
                commands.add(context, entity, ProductionQueue{});
                commands.add(context, entity, RallyPoint{
                    {footprint->origin.x + footprint->width,
                     footprint->origin.y + footprint->height / 2}
                });
            }
            commands.remove<ConstructionSite>(context, entity);
        }
    }

    BuildFailure validatePlacement(const BuildingFootprint& footprint,
                                   GridPoint requiredPathStart,
                                   GridPoint requiredPathGoal) const {
        const auto cells = footprintCells(footprint);
        for (const auto cell : cells) {
            if (!navigation_.contains(cell)) {
                return BuildFailure::OutOfBounds;
            }
            if (navigation_.blocked(cell)) {
                return BuildFailure::Occupied;
            }
        }

        NavigationGrid preview = navigation_;
        for (const auto cell : cells) preview.setBlocked(cell, true);
        if (requiredPathStart == requiredPathGoal) {
            return preview.blocked(requiredPathStart)
                ? BuildFailure::BlocksRequiredPath
                : BuildFailure::None;
        }
        if (!GridPathfinder::find(
                 preview, requiredPathStart, requiredPathGoal).found) {
            return BuildFailure::BlocksRequiredPath;
        }
        return BuildFailure::None;
    }

    void releaseFootprint(const BuildingFootprint& footprint) {
        setBlocked(footprint, false);
    }

    const ResourceLedger& ledger() const noexcept { return ledger_; }

    ConstructionId nextConstructionId() const noexcept {
        return nextConstructionId_;
    }

    void restoreNextConstructionId(ConstructionId value) noexcept {
        nextConstructionId_ = value;
    }

private:
    static bool valid(const BuildingDefinition& definition) noexcept {
        return definition.id != 0 && definition.cost >= 0 &&
               definition.width > 0 && definition.height > 0 &&
               definition.visionRange >= 0;
    }

    static std::vector<GridPoint> footprintCells(
        const BuildingFootprint& footprint) {
        std::vector<GridPoint> cells;
        cells.reserve(static_cast<std::size_t>(
            footprint.width * footprint.height));
        for (std::int32_t y = 0; y < footprint.height; ++y) {
            for (std::int32_t x = 0; x < footprint.width; ++x) {
                cells.push_back(
                    {footprint.origin.x + x, footprint.origin.y + y});
            }
        }
        return cells;
    }

    void setBlocked(const BuildingFootprint& footprint, bool blocked) {
        for (const auto cell : footprintCells(footprint)) {
            navigation_.setBlocked(cell, blocked);
        }
    }

    ResourceLedger& ledger_;
    NavigationGrid& navigation_;
    ConstructionId nextConstructionId_{};
};

} // namespace rts::gameplay
