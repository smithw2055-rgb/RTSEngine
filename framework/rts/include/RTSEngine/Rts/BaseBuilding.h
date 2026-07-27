#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/Combat.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/TeamEconomy.h>
#include <RTSEngine/Rts/VisionTypes.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::gameplay {

using ConstructionId = std::uint32_t;
using ProductionId = std::uint32_t;

struct BuildingDefinition {
    std::uint32_t id{};
    std::int32_t cost{};
    std::uint32_t buildTicks{1};
    std::int32_t width{1};
    std::int32_t height{1};
    bool producer{};
    CombatStats combat{};
    std::int32_t visionRange{8};
    ResourceTypeId dropOffResourceType{};
    std::uint32_t supplyProvided{};
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
    ResourceTypeId dropOffResourceType{};
    std::uint32_t supplyProvided{};
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
        : legacyLedger_(&ledger), navigation_(navigation) {}

    BaseBuildingRuntime(
        TeamEconomyRuntime& economy,
        NavigationGrid& navigation,
        std::uint32_t compatibilityTeam = 1)
        : economy_(&economy),
          navigation_(navigation),
          compatibilityTeam_(compatibilityTeam) {}

    BuildResult begin(const ecs::SystemContext& context,
                      ecs::EntityCommandBuffer& commands,
                      const BuildingDefinition& definition,
                      GridPoint origin,
                      GridPoint requiredPathStart,
                      GridPoint requiredPathGoal,
                      std::uint32_t ownerTeam = 0,
                      std::uint32_t baseBuildTicks = 0) {
        if (!valid(definition) || ownerTeam == 0) {
            return {false, BuildFailure::InvalidDefinition, 0};
        }

        const BuildingFootprint footprint{
            origin, definition.width, definition.height};
        const auto placementFailure =
            validatePlacement(footprint, requiredPathStart, requiredPathGoal);
        if (placementFailure != BuildFailure::None) {
            return {false, placementFailure, 0};
        }
        if (!reserve(ownerTeam, definition.cost)) {
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
            sourceTicks,
            definition.dropOffResourceType,
            definition.supplyProvided});
        return {true, BuildFailure::None, id};
    }

    BuildFailure cancel(const ecs::SystemContext& context,
                        ecs::EntityCommandBuffer& commands,
                        ecs::World& world,
                        ConstructionId id) {
        BuildFailure result = BuildFailure::UnknownConstruction;
        world.eachRef<ConstructionSite, BuildingFootprint>(
            [&](ecs::Entity entity,
                ConstructionSite& site,
                BuildingFootprint& footprint) {
                if (result == BuildFailure::None || site.id != id) return;
                releaseFootprint(footprint);
                release(site.ownerTeam, site.reservedCost);
                commands.destroy(context, entity);
                result = BuildFailure::None;
            });
        return result;
    }

    void advance(const ecs::SystemContext& context,
                 ecs::EntityCommandBuffer& commands,
                 ecs::World& world) {
        world.eachRef<ConstructionSite, BuildingFootprint>(
            [&](ecs::Entity entity,
                ConstructionSite& site,
                BuildingFootprint& footprint) {
                ++site.progressTicks;
                if (site.progressTicks < site.requiredTicks) return;

                commit(site.ownerTeam, site.reservedCost);
                commands.add(
                    context, entity,
                    Building{site.definitionId, site.producer});
                if (site.producer) {
                    commands.add(context, entity, ProductionQueue{});
                    commands.add(context, entity, RallyPoint{
                        {footprint.origin.x + footprint.width,
                         footprint.origin.y + footprint.height / 2}});
                }
                if (site.dropOffResourceType != 0) {
                    commands.add(
                        context,
                        entity,
                        ResourceDropOff{site.dropOffResourceType});
                }
                if (site.supplyProvided != 0) {
                    commands.add(
                        context,
                        entity,
                        SupplyProvider{site.supplyProvided});
                }
                commands.remove<ConstructionSite>(context, entity);
            });
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
        if (!preview.setBlockedBatch(cells, true)) {
            return BuildFailure::OutOfBounds;
        }
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

    const ResourceLedger& ledger() const noexcept {
        if (legacyLedger_) return *legacyLedger_;
        legacyView_ = economy_
            ? economy_->legacyLedger(compatibilityTeam_)
            : ResourceLedger{};
        return legacyView_;
    }

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

    bool reserve(std::uint32_t teamId, std::int32_t amount) {
        return economy_
            ? economy_->reserve(teamId, kPrimaryResourceType, amount)
            : legacyLedger_ && legacyLedger_->reserve(amount);
    }

    bool commit(std::uint32_t teamId, std::int32_t amount) {
        return economy_
            ? economy_->commit(teamId, kPrimaryResourceType, amount)
            : legacyLedger_ && legacyLedger_->commit(amount);
    }

    bool release(std::uint32_t teamId, std::int32_t amount) {
        return economy_
            ? economy_->release(teamId, kPrimaryResourceType, amount)
            : legacyLedger_ && legacyLedger_->release(amount);
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
        (void)navigation_.setBlockedBatch(
            footprintCells(footprint), blocked);
    }

    TeamEconomyRuntime* economy_{};
    ResourceLedger* legacyLedger_{};
    NavigationGrid& navigation_;
    std::uint32_t compatibilityTeam_{1};
    mutable ResourceLedger legacyView_{};
    ConstructionId nextConstructionId_{};
};

} // namespace rts::gameplay
