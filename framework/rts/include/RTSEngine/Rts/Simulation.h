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
#include <RTSEngine/Rts/Influence.h>
#include <RTSEngine/Rts/MovementSystem.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/NavigationSystem.h>
#include <RTSEngine/Rts/OrderSystem.h>
#include <RTSEngine/Rts/PathCache.h>
#include <RTSEngine/Rts/RuntimeTelemetry.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <RTSEngine/Rts/SnapshotBuilder.h>
#include <RTSEngine/Rts/Vision.h>
#include <rts/sim/AuthoritativeStep.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::tower_defense {
class TowerDefenseSimulation;
}

namespace rts::gameplay {

class RtsSimulationArchive;

enum class RtsStepResult : std::uint8_t {
    Advanced,
    StaleTick,
    NonSequentialTick
};

class RtsSimulation {
public:
    RtsSimulation(std::int32_t width = 32, std::int32_t height = 32);
    ~RtsSimulation();

    RtsSimulation(const RtsSimulation&) = delete;
    RtsSimulation& operator=(const RtsSimulation&) = delete;
    RtsSimulation(RtsSimulation&&) = delete;
    RtsSimulation& operator=(RtsSimulation&&) = delete;

    bool registerBuilding(BuildingDefinition definition) {
        if (configurationFrozen()) return false;
        buildingDefinitions_.replace(std::move(definition));
        return true;
    }

    bool registerUnit(UnitDefinition definition) {
        if (configurationFrozen()) return false;
        unitDefinitions_.replace(std::move(definition));
        return true;
    }

    void freezeConfiguration() noexcept { configurationFrozen_ = true; }

    bool configurationFrozen() const noexcept {
        return configurationFrozen_ || hasStepped_;
    }

    void setResources(std::int32_t available) noexcept {
        resources_.available = std::max<std::int32_t>(0, available);
    }

    bool setRequiredRoute(GridPoint start, GridPoint goal) noexcept {
        if (configurationFrozen()) return false;
        requiredPathStart_ = start;
        requiredPathGoal_ = goal;
        return true;
    }

    bool setPlayerTeam(std::uint32_t teamId) noexcept {
        if (configurationFrozen() || teamId == 0) return false;
        playerTeamId_ = teamId;
        return true;
    }

    void reserveMovementAgents(std::size_t count) {
        movement_.reserveAgents(count);
        combat_.reserveCombatants(count);
    }

    void reserveTickScratch(
        std::size_t activeCommandCount,
        std::size_t structuralCommandCount,
        std::size_t deferredEntityCount,
        std::size_t constructionCompletionCount) {
        activeCommands_.reserve(activeCommandCount);
        structuralCommands_.reserve(
            structuralCommandCount, deferredEntityCount);
        completingConstructions_.reserve(constructionCompletionCount);
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
        CombatStats combat = {},
        std::int32_t visionRange = 6) {
        if (configurationFrozen()) return {};
        return createUnitUnchecked(
            position, speed, teamId, combat, visionRange);
    }

    sim::CommandSubmitResult submitDetailed(TickCommand command) {
        if (isInternalIssuer(command.issuer)) {
            return sim::CommandSubmitResult::Unauthorized;
        }
        return commands_.submitDetailed(std::move(command));
    }

    bool submit(TickCommand command) {
        return submitDetailed(std::move(command)) ==
               sim::CommandSubmitResult::Accepted;
    }

    bool setBlocked(GridPoint point, bool blocked) {
        return navigation_.setBlocked(point, blocked);
    }

    const NavigationGrid& navigation() const noexcept {
        return navigation_;
    }

    const VisionRuntime& vision() const noexcept {
        return vision_;
    }

    const InfluenceRuntime& influence() const {
        refreshDerivedInfluence();
        return influence_;
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

    sim::AuthoritativeStepValidation validateStep(
        std::uint64_t tick) const noexcept {
        return sim::ValidateAuthoritativeStep(
            hasStepped_, lastCompletedTick_, tick);
    }

    RtsStepResult stepDetailed(std::uint64_t tick) {
        const auto validation = validateStep(tick);
        if (!validation) {
            return validation.failure == sim::AuthoritativeStepFailure::StaleTick
                ? RtsStepResult::StaleTick
                : RtsStepResult::NonSequentialTick;
        }

        configurationFrozen_ = true;
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
        return RtsStepResult::Advanced;
    }

    bool step(std::uint64_t tick) {
        return stepDetailed(tick) == RtsStepResult::Advanced;
    }

    bool stepNext() {
        return step(hasStepped_ ? lastCompletedTick_ + 1u : 0u);
    }

    std::uint64_t nextExpectedTick() const noexcept {
        return hasStepped_ ? lastCompletedTick_ + 1u : 0u;
    }

    const WorldSnapshot& snapshot() const {
        refreshDerivedInfluence();
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
        if (std::any_of(
                state.pending.begin(),
                state.pending.end(),
                [](const TickCommand& command) {
                    return isInternalIssuer(command.issuer);
                })) {
            return false;
        }
        return commands_.restore(std::move(state));
    }

    std::uint64_t lastCompletedTick() const noexcept {
        return lastCompletedTick_;
    }

private:
    friend class RtsSimulationArchive;
    friend class ::rts::tower_defense::TowerDefenseSimulation;

    static constexpr std::uint32_t kInternalIssuerMask = 0x80000000u;
    static constexpr std::uint64_t kInvalidDerivedHash =
        std::numeric_limits<std::uint64_t>::max();

    static bool isInternalIssuer(std::uint32_t issuer) noexcept {
        return (issuer & kInternalIssuerMask) != 0;
    }

    ecs::Entity createUnitUnchecked(
        Position position,
        MoveSpeed speed,
        std::uint32_t teamId,
        CombatStats combat,
        std::int32_t visionRange) {
        return EntityFactory::createUnit(
            world_,
            modifiers_,
            position,
            speed,
            teamId,
            combat,
            visionRange);
    }

    ecs::Entity createUnitInternal(
        Position position,
        MoveSpeed speed,
        std::uint32_t teamId,
        CombatStats combat,
        std::int32_t visionRange) {
        return createUnitUnchecked(
            position, speed, teamId, combat, visionRange);
    }

    sim::CommandSubmitResult submitInternalDetailed(TickCommand command) {
        if (!isInternalIssuer(command.issuer)) {
            return sim::CommandSubmitResult::Unauthorized;
        }
        return commands_.submitDetailed(std::move(command));
    }

    bool submitInternal(TickCommand command) {
        return submitInternalDetailed(std::move(command)) ==
               sim::CommandSubmitResult::Accepted;
    }

    void refreshDerivedInfluence() const {
        if (!hasStepped_) {
            influence_.clear();
            snapshot_.influenceWidth = 0;
            snapshot_.influenceHeight = 0;
            snapshot_.influence.clear();
            influenceWorldHash_ = kInvalidDerivedHash;
            return;
        }
        if (influenceWorldHash_ == snapshot_.worldHash) return;

        influence_.rebuild(world_, vision_);
        snapshot_.influenceWidth = influence_.width();
        snapshot_.influenceHeight = influence_.height();
        influence_.buildSnapshot(snapshot_.influence);
        influenceWorldHash_ = snapshot_.worldHash;
    }

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
                            world, context, command, {events_, &vision_});
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
                    {building_,
                     structuralCommands_,
                     completingConstructions_,
                     events_});
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
            -10,
            390,
            [this](ecs::World& world,
                   const ecs::SystemContext&) {
                VisionSystem::run(world, navigation_, vision_);
            });

        scheduler_.add(
            ecs::Stage::Snapshot,
            -5,
            395,
            [this](ecs::World& world,
                   const ecs::SystemContext&) {
                InfluenceSystem::run(world, vision_, influence_);
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
                     snapshot_,
                     &vision_});
                snapshot_.influenceWidth = influence_.width();
                snapshot_.influenceHeight = influence_.height();
                influence_.buildSnapshot(snapshot_.influence);
                influenceWorldHash_ = snapshot_.worldHash;
            });
    }

    ecs::World world_;
    ecs::Scheduler scheduler_;
    TickCommandStream commands_;
    ecs::EntityCommandBuffer structuralCommands_;
    NavigationGrid navigation_;
    VisionRuntime vision_;
    mutable InfluenceRuntime influence_;
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
    std::vector<ConstructionId> completingConstructions_;
    std::vector<DomainEvent> events_;
    std::vector<DomainEvent> deathSideEffects_;
    mutable WorldSnapshot snapshot_;
    DefinitionCatalog<BuildingDefinition> buildingDefinitions_;
    DefinitionCatalog<UnitDefinition> unitDefinitions_;
    GridPoint requiredPathStart_{0, 0};
    GridPoint requiredPathGoal_{0, 0};
    ProductionId nextProductionId_{};
    std::uint32_t playerTeamId_{1};
    std::uint64_t lastCompletedTick_{};
    mutable std::uint64_t influenceWorldHash_{kInvalidDerivedHash};
    bool configurationFrozen_{};
    bool hasStepped_{};
};

} // namespace rts::gameplay
