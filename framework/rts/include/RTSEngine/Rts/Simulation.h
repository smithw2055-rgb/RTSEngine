#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/Scheduler.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/Combat.h>
#include <RTSEngine/Rts/CombatDeathSystem.h>
#include <RTSEngine/Rts/DefinitionCatalog.h>
#include <RTSEngine/Rts/EconomySystems.h>
#include <RTSEngine/Rts/EntityFactory.h>
#include <RTSEngine/Rts/FlowField.h>
#include <RTSEngine/Rts/GameplayModifierSystem.h>
#include <RTSEngine/Rts/MovementSystem.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/NavigationSystem.h>
#include <RTSEngine/Rts/OrderSystem.h>
#include <RTSEngine/Rts/PathCache.h>
#include <RTSEngine/Rts/RuntimeTelemetry.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <RTSEngine/Rts/SnapshotBuilder.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rts::gameplay {

class RtsSimulationArchive;

class RtsSimulation {
public:
    RtsSimulation(std::int32_t width = 32, std::int32_t height = 32)
        : navigation_(width, height),
          movement_(width, height),
          building_(resources_, navigation_),
          combat_(width, height) {
        installSystems();
    }

    void registerBuilding(BuildingDefinition definition) {
        buildingDefinitions_.replace(std::move(definition));
    }

    void registerUnit(UnitDefinition definition) {
        unitDefinitions_.replace(std::move(definition));
    }

    void setResources(std::int32_t available) noexcept {
        resources_.available = std::max<std::int32_t>(0, available);
    }

    void setRequiredRoute(GridPoint start, GridPoint goal) noexcept {
        requiredPathStart_ = start;
        requiredPathGoal_ = goal;
    }

    void setPlayerTeam(std::uint32_t teamId) noexcept {
        playerTeamId_ = teamId;
    }

    void reserveMovementAgents(std::size_t count) {
        movement_.reserveAgents(count);
    }

    bool setTeamModifierProfile(
        std::uint32_t teamId,
        TeamModifierProfile profile) {
        return modifiers_.setProfile<MoveSpeed>(world_, teamId, profile);
    }

    const TeamModifierProfile& teamModifierProfile(
        std::uint32_t teamId) const noexcept {
        return modifiers_.profile(teamId);
    }

    ecs::Entity createUnit(
        Position position,
        MoveSpeed speed,
        std::uint32_t teamId = 1,
        CombatStats combat = {}) {
        return EntityFactory::createUnit(
            world_, modifiers_, position, speed, teamId, combat);
    }

    bool submit(TickCommand command) {
        return commands_.submit(std::move(command));
    }

    bool setBlocked(GridPoint point, bool blocked) {
        return navigation_.setBlocked(point, blocked);
    }

    const NavigationGrid& navigation() const noexcept {
        return navigation_;
    }

    const GridPathCache& pathCache() const noexcept {
        return pathCache_;
    }

    const GridFlowFieldCache& flowFields() const noexcept {
        return flowFields_;
    }

    const MovementReservationRuntime& movementReservations() const noexcept {
        return movement_;
    }

    const RuntimeTelemetry& telemetry() const noexcept {
        return telemetry_;
    }

    const ResourceLedger& resources() const noexcept {
        return resources_;
    }

    void step(std::uint64_t tick) {
        if (hasStepped_ && tick <= lastCompletedTick_) return;

        events_.clear();
        activeCommands_ = commands_.consume(tick);
        runStage(tick, ecs::Stage::Command);
        runStage(tick, ecs::Stage::Navigation);
        runStage(tick, ecs::Stage::Simulation);
        runStage(tick, ecs::Stage::Combat);
        runStage(tick, ecs::Stage::Cleanup);
        scheduler_.run_stage(world_, tick, ecs::Stage::Snapshot);
        hasStepped_ = true;
        lastCompletedTick_ = tick;
    }

    const WorldSnapshot& snapshot() const noexcept {
        return snapshot_;
    }

    const std::vector<DomainEvent>& events() const noexcept {
        return events_;
    }

    const ecs::World& world() const noexcept { return world_; }

    TickCommandStream::State commandStreamState() const {
        return commands_.snapshot();
    }

    bool restoreCommandStream(TickCommandStream::State state) {
        return commands_.restore(std::move(state));
    }

    std::uint64_t lastCompletedTick() const noexcept {
        return lastCompletedTick_;
    }

private:
    friend class RtsSimulationArchive;

    void runStage(std::uint64_t tick, ecs::Stage stage) {
        scheduler_.run_stage(world_, tick, stage);
        structuralCommands_.commit_through(world_, stage);
    }

    EconomyCommandDependencies economyCommandDependencies() {
        return {
            structuralCommands_,
            building_,
            resources_,
            modifiers_,
            buildingDefinitions_,
            unitDefinitions_,
            requiredPathStart_,
            requiredPathGoal_,
            nextProductionId_,
            events_};
    }

    NavigationSystemDependencies navigationDependencies() {
        return {
            navigation_,
            pathCache_,
            pathScratch_,
            flowFields_,
            telemetry_,
            structuralCommands_,
            modifiers_,
            buildingDefinitions_,
            events_};
    }

    void installSystems() {
        scheduler_.add(
            ecs::Stage::Command,
            0,
            100,
            [this](ecs::World& world,
                   const ecs::SystemContext& context) {
                for (const auto& command : activeCommands_) {
                    if (OrderSystem::handles(command.type)) {
                        OrderSystem::process(
                            world, context, command, {events_});
                    } else if (EconomyCommandSystem::handles(command.type)) {
                        EconomyCommandSystem::process(
                            world,
                            context,
                            command,
                            economyCommandDependencies());
                    }
                }
            });

        scheduler_.add(
            ecs::Stage::Navigation,
            -20,
            180,
            [this](ecs::World& world,
                   const ecs::SystemContext&) {
                NavigationSystem::synchronizeTeamModifiers(
                    world, navigationDependencies());
            });

        scheduler_.add(
            ecs::Stage::Navigation,
            -10,
            190,
            [this](ecs::World& world,
                   const ecs::SystemContext& context) {
                NavigationSystem::synchronizeConstruction(
                    world, context, navigationDependencies());
            });

        scheduler_.add(
            ecs::Stage::Navigation,
            0,
            200,
            [this](ecs::World& world,
                   const ecs::SystemContext& context) {
                NavigationSystem::run(
                    world, context, navigationDependencies());
            });

        scheduler_.add(
            ecs::Stage::Simulation,
            0,
            300,
            [this](ecs::World& world,
                   const ecs::SystemContext& context) {
                ConstructionSystem::run(
                    world,
                    context,
                    {building_, structuralCommands_, events_});
            });

        scheduler_.add(
            ecs::Stage::Simulation,
            10,
            310,
            [this](ecs::World& world,
                   const ecs::SystemContext& context) {
                ProductionSystem::run(
                    world,
                    context,
                    {structuralCommands_,
                     resources_,
                     modifiers_,
                     unitDefinitions_,
                     events_});
            });

        scheduler_.add(
            ecs::Stage::Simulation,
            20,
            320,
            [this](ecs::World& world,
                   const ecs::SystemContext& context) {
                MovementSystem::run(
                    world,
                    context,
                    {navigation_, movement_, telemetry_, events_});
            });

        scheduler_.add(
            ecs::Stage::Combat,
            0,
            350,
            [this](ecs::World& world,
                   const ecs::SystemContext& context) {
                CombatDeathSystem::run(
                    world,
                    context,
                    {combat_,
                     structuralCommands_,
                     building_,
                     resources_,
                     modifiers_,
                     playerTeamId_,
                     events_,
                     deathSideEffects_});
            });

        scheduler_.add(
            ecs::Stage::Snapshot,
            0,
            400,
            [this](ecs::World& world,
                   const ecs::SystemContext& context) {
                SnapshotBuilder::build(
                    world,
                    context.tick,
                    {resources_,
                     modifiers_,
                     navigation_,
                     commands_,
                     snapshot_});
            });
    }

    ecs::World world_;
    ecs::Scheduler scheduler_;
    TickCommandStream commands_;
    ecs::EntityCommandBuffer structuralCommands_;
    NavigationGrid navigation_;
    GridPathCache pathCache_;
    GridPathfinderScratch pathScratch_;
    GridFlowFieldCache flowFields_;
    RuntimeTelemetry telemetry_;
    MovementReservationRuntime movement_;
    ResourceLedger resources_;
    BaseBuildingRuntime building_;
    CombatRuntime combat_;
    GameplayModifierSystem modifiers_;
    std::vector<TickCommand> activeCommands_;
    std::vector<DomainEvent> events_;
    std::vector<DomainEvent> deathSideEffects_;
    WorldSnapshot snapshot_;
    DefinitionCatalog<BuildingDefinition> buildingDefinitions_;
    DefinitionCatalog<UnitDefinition> unitDefinitions_;
    GridPoint requiredPathStart_{0, 0};
    GridPoint requiredPathGoal_{0, 0};
    ProductionId nextProductionId_{};
    std::uint32_t playerTeamId_{1};
    std::uint64_t lastCompletedTick_{};
    bool hasStepped_{};
};

} // namespace rts::gameplay
