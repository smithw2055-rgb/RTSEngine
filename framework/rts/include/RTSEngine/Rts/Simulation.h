#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/Scheduler.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/Combat.h>
#include <RTSEngine/Rts/Navigation.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

struct Position {
    std::int32_t x{};
    std::int32_t y{};
};

struct MoveSpeed {
    std::int32_t cellsPerTick{1};
};

enum class OrderType : std::uint8_t {
    Move,
    AttackMove
};

struct Order {
    OrderType type{OrderType::Move};
    GridPoint target{};
};

struct OrderQueue {
    std::vector<Order> pending;
};

struct MovementAgent {
    std::vector<GridPoint> path;
    std::size_t nextPoint{};
    std::uint64_t pathRevision{};
    GridPoint pathGoal{};
    bool hasPathGoal{};
    bool combatPath{};
    ecs::Entity chaseTarget{};
    GridPoint chaseTargetPosition{};
};

struct UnitDefinition {
    std::uint32_t id{};
    std::int32_t cost{};
    std::uint32_t trainTicks{1};
    std::int32_t cellsPerTick{1};
    CombatStats combat{};
};

enum class CommandType : std::uint8_t {
    Move,
    Stop,
    Build,
    CancelConstruction,
    Train,
    CancelProduction,
    SetRally,
    Attack,
    AttackMove,
    HoldPosition
};

struct TickCommand {
    std::uint64_t targetTick{};
    std::uint32_t issuer{};
    std::uint32_t sequence{};
    CommandType type{CommandType::Move};
    ecs::Entity subject{};
    std::int32_t targetX{};
    std::int32_t targetY{};
    bool append{};
    std::uint32_t definitionId{};
    std::uint32_t objectId{};
    ecs::Entity targetEntity{};
};

class TickCommandStream {
public:
    bool submit(TickCommand command) {
        if (command.targetTick < committedThrough_) return false;
        commands_.push_back(command);
        return true;
    }

    std::vector<TickCommand> consume(std::uint64_t tick) {
        committedThrough_ = tick + 1;
        std::vector<TickCommand> result;
        auto iterator = commands_.begin();
        while (iterator != commands_.end()) {
            if (iterator->targetTick == tick) {
                result.push_back(*iterator);
                iterator = commands_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        std::sort(result.begin(), result.end(), [](const TickCommand& a, const TickCommand& b) {
            return a.issuer != b.issuer ? a.issuer < b.issuer : a.sequence < b.sequence;
        });
        result.erase(std::unique(result.begin(), result.end(),
                                 [](const TickCommand& a, const TickCommand& b) {
                                     return a.issuer == b.issuer &&
                                            a.sequence == b.sequence;
                                 }),
                     result.end());
        return result;
    }

private:
    std::vector<TickCommand> commands_;
    std::uint64_t committedThrough_{};
};

enum class DomainEventType : std::uint8_t {
    MoveAccepted,
    MoveCompleted,
    OrderStopped,
    PathReady,
    PathFailed,
    ConstructionAccepted,
    ConstructionRejected,
    ConstructionCancelled,
    ConstructionCompleted,
    ConstructionDestroyed,
    ProductionAccepted,
    ProductionRejected,
    ProductionCancelled,
    ProductionCompleted,
    RallyPointChanged,
    AttackAccepted,
    AttackRejected,
    AttackMoveAccepted,
    HoldPositionAccepted,
    AttackTargetLost,
    TargetAcquired,
    WeaponFired,
    DamageApplied,
    EntityDied,
    BountyAwarded
};

struct DomainEvent {
    std::uint64_t tick{};
    DomainEventType type{};
    ecs::Entity entity{};
    std::uint32_t objectId{};
    std::uint32_t reason{};
    ecs::Entity secondary{};
    std::int32_t value{};
};

enum class SnapshotKind : std::uint8_t {
    Unit,
    Construction,
    Building
};

struct SnapshotEntity {
    ecs::Entity entity{};
    std::int32_t x{};
    std::int32_t y{};
    bool moving{};
    std::uint32_t queuedOrders{};
    SnapshotKind kind{SnapshotKind::Unit};
    std::uint32_t definitionId{};
    std::uint32_t progressTicks{};
    std::uint32_t requiredTicks{};
    std::uint32_t productionQueueSize{};
    std::uint32_t teamId{};
    std::int32_t healthCurrent{};
    std::int32_t healthMaximum{};
    std::int32_t armor{};
    std::uint32_t cooldownRemaining{};
    ecs::Entity target{};
    CombatMode combatMode{CombatMode::Guard};
};

struct WorldSnapshot {
    std::uint64_t tick{};
    std::uint64_t worldHash{};
    ResourceLedger resources{};
    std::vector<SnapshotEntity> entities;
};

class RtsSimulation {
public:
    RtsSimulation(std::int32_t width = 32, std::int32_t height = 32)
        : navigation_(width, height),
          building_(resources_, navigation_),
          combat_(width, height) {
        installSystems();
    }

    void registerBuilding(BuildingDefinition definition) {
        replaceDefinition(buildingDefinitions_, definition);
    }

    void registerUnit(UnitDefinition definition) {
        replaceDefinition(unitDefinitions_, definition);
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

    ecs::Entity createUnit(Position position,
                           MoveSpeed speed,
                           std::uint32_t teamId = 1,
                           CombatStats combat = {}) {
        const auto entity = world_.create();
        world_.emplace<Position>(entity, position);
        world_.emplace<MoveSpeed>(entity, speed);
        world_.emplace<OrderQueue>(entity, OrderQueue{});
        world_.emplace<MovementAgent>(entity, MovementAgent{});
        world_.emplace<Team>(entity, Team{teamId});
        attachCombatProfile(entity, combat);
        return entity;
    }

    bool submit(TickCommand command) { return commands_.submit(command); }
    bool setBlocked(GridPoint point, bool blocked) {
        return navigation_.setBlocked(point, blocked);
    }

    const NavigationGrid& navigation() const noexcept { return navigation_; }
    const ResourceLedger& resources() const noexcept { return resources_; }

    void step(std::uint64_t tick) {
        events_.clear();
        activeCommands_ = commands_.consume(tick);

        runStage(tick, ecs::Stage::Command);
        runStage(tick, ecs::Stage::Navigation);
        runStage(tick, ecs::Stage::Simulation);
        runStage(tick, ecs::Stage::Combat);
        runStage(tick, ecs::Stage::Cleanup);
        scheduler_.run_stage(world_, tick, ecs::Stage::Snapshot);
    }

    const WorldSnapshot& snapshot() const noexcept { return snapshot_; }
    const std::vector<DomainEvent>& events() const noexcept { return events_; }
    const ecs::World& world() const noexcept { return world_; }

private:
    template<class Definition>
    static void replaceDefinition(std::vector<Definition>& definitions,
                                  Definition definition) {
        auto iterator = std::find_if(definitions.begin(), definitions.end(),
                                     [&](const Definition& value) {
                                         return value.id == definition.id;
                                     });
        if (iterator == definitions.end()) {
            definitions.push_back(definition);
        } else {
            *iterator = definition;
        }
        std::sort(definitions.begin(), definitions.end(),
                  [](const Definition& a, const Definition& b) {
                      return a.id < b.id;
                  });
    }

    static std::int32_t distance(GridPoint a, GridPoint b) noexcept {
        const auto dx = a.x > b.x ? a.x - b.x : b.x - a.x;
        const auto dy = a.y > b.y ? a.y - b.y : b.y - a.y;
        return dx + dy;
    }

    const BuildingDefinition* buildingDefinition(std::uint32_t id) const {
        const auto iterator =
            std::find_if(buildingDefinitions_.begin(), buildingDefinitions_.end(),
                         [id](const BuildingDefinition& value) {
                             return value.id == id;
                         });
        return iterator == buildingDefinitions_.end() ? nullptr : &*iterator;
    }

    const UnitDefinition* unitDefinition(std::uint32_t id) const {
        const auto iterator =
            std::find_if(unitDefinitions_.begin(), unitDefinitions_.end(),
                         [id](const UnitDefinition& value) {
                             return value.id == id;
                         });
        return iterator == unitDefinitions_.end() ? nullptr : &*iterator;
    }

    void runStage(std::uint64_t tick, ecs::Stage stage) {
        scheduler_.run_stage(world_, tick, stage);
        structuralCommands_.commit_through(world_, stage);
    }

    void installSystems() {
        scheduler_.add(ecs::Stage::Command, 0, 100,
                       [this](ecs::World& world,
                              const ecs::SystemContext& context) {
                           for (const auto& command : activeCommands_) {
                               processCommand(world, context, command);
                           }
                       });

        scheduler_.add(ecs::Stage::Navigation, -10, 190,
                       [this](ecs::World& world,
                              const ecs::SystemContext& context) {
                           synchronizeConstructionCombat(world, context);
                       });

        scheduler_.add(ecs::Stage::Navigation, 0, 200,
                       [this](ecs::World& world,
                              const ecs::SystemContext& context) {
                           updateNavigation(world, context);
                       });

        scheduler_.add(ecs::Stage::Simulation, 0, 300,
                       [this](ecs::World& world,
                              const ecs::SystemContext& context) {
                           std::vector<ConstructionId> completing;
                           for (const auto entity :
                                world.view<ConstructionSite>()) {
                               const auto* site =
                                   world.try_get<ConstructionSite>(entity);
                               if (site &&
                                   site->progressTicks + 1 >=
                                       site->requiredTicks) {
                                   completing.push_back(site->id);
                               }
                           }
                           building_.advance(context, structuralCommands_, world);
                           for (const auto id : completing) {
                               events_.push_back(
                                   {context.tick,
                                    DomainEventType::ConstructionCompleted,
                                    {},
                                    id,
                                    0});
                           }
                       });

        scheduler_.add(ecs::Stage::Simulation, 10, 310,
                       [this](ecs::World& world,
                              const ecs::SystemContext& context) {
                           advanceProduction(world, context);
                       });

        scheduler_.add(ecs::Stage::Simulation, 20, 320,
                       [this](ecs::World& world,
                              const ecs::SystemContext& context) {
                           updateMovement(world, context);
                       });

        scheduler_.add(ecs::Stage::Combat, 0, 350,
                       [this](ecs::World& world,
                              const ecs::SystemContext& context) {
                           deathSideEffects_.clear();
                           combat_.advance<Position>(
                               context,
                               structuralCommands_,
                               world,
                               [this, &world, &context](ecs::Entity victim,
                                                       ecs::Entity killer) {
                                   handleDeath(world, context, victim, killer);
                               });
                           forwardCombatEvents();
                       });

        scheduler_.add(ecs::Stage::Snapshot, 0, 400,
                       [this](ecs::World& world,
                              const ecs::SystemContext& context) {
                           buildSnapshot(world, context.tick);
                       });
    }

    void processCommand(ecs::World& world,
                        const ecs::SystemContext& context,
                        const TickCommand& command) {
        switch (command.type) {
        case CommandType::Move:
        case CommandType::Stop:
            processOrderCommand(world, context, command);
            break;
        case CommandType::Build: {
            const auto* definition =
                buildingDefinition(command.definitionId);
            BuildResult result;
            if (definition) {
                result = building_.begin(
                    context,
                    structuralCommands_,
                    *definition,
                    {command.targetX, command.targetY},
                    requiredPathStart_,
                    requiredPathGoal_,
                    command.issuer);
            } else {
                result = {false, BuildFailure::InvalidDefinition, 0};
            }
            events_.push_back(
                {context.tick,
                 result.accepted
                     ? DomainEventType::ConstructionAccepted
                     : DomainEventType::ConstructionRejected,
                 {},
                 result.constructionId,
                 static_cast<std::uint32_t>(result.failure)});
            break;
        }
        case CommandType::CancelConstruction: {
            const auto failure = building_.cancel(
                context, structuralCommands_, world, command.objectId);
            events_.push_back(
                {context.tick,
                 failure == BuildFailure::None
                     ? DomainEventType::ConstructionCancelled
                     : DomainEventType::ConstructionRejected,
                 {},
                 command.objectId,
                 static_cast<std::uint32_t>(failure)});
            break;
        }
        case CommandType::Train:
            beginProduction(world, context, command);
            break;
        case CommandType::CancelProduction:
            cancelProduction(world, context, command);
            break;
        case CommandType::SetRally: {
            auto* rally = world.try_get<RallyPoint>(command.subject);
            if (rally) {
                rally->point = {command.targetX, command.targetY};
                events_.push_back(
                    {context.tick,
                     DomainEventType::RallyPointChanged,
                     command.subject,
                     0,
                     0});
            }
            break;
        }
        case CommandType::Attack:
            processAttackCommand(world, context, command);
            break;
        case CommandType::AttackMove:
            processAttackMoveCommand(world, context, command);
            break;
        case CommandType::HoldPosition:
            processHoldPositionCommand(world, context, command);
            break;
        }
    }

    static void clearPath(MovementAgent& agent) {
        agent.path.clear();
        agent.nextPoint = 0;
        agent.hasPathGoal = false;
        agent.combatPath = false;
        agent.chaseTarget = {};
        agent.chaseTargetPosition = {};
    }

    static void clearTarget(ecs::World& world, ecs::Entity entity) {
        if (auto* target = world.try_get<CombatTarget>(entity)) {
            target->entity = {};
        }
    }

    static void applyFrontOrderMode(ecs::World& world,
                                    ecs::Entity entity,
                                    const OrderQueue& queue) {
        auto* directive = world.try_get<CombatDirective>(entity);
        if (!directive || directive->mode == CombatMode::AttackTarget ||
            directive->mode == CombatMode::HoldPosition) {
            return;
        }
        directive->forcedTarget = {};
        directive->mode = queue.pending.empty()
            ? CombatMode::Guard
            : (queue.pending.front().type == OrderType::AttackMove
                   ? CombatMode::AttackMove
                   : CombatMode::PassiveMove);
    }

    void processOrderCommand(ecs::World& world,
                             const ecs::SystemContext& context,
                             const TickCommand& command) {
        if (!world.alive(command.subject)) return;
        auto* queue = world.try_get<OrderQueue>(command.subject);
        auto* agent = world.try_get<MovementAgent>(command.subject);
        if (!queue || !agent) return;

        if (command.type == CommandType::Stop) {
            queue->pending.clear();
            clearPath(*agent);
            clearTarget(world, command.subject);
            if (auto* directive =
                    world.try_get<CombatDirective>(command.subject)) {
                directive->mode = CombatMode::Guard;
                directive->forcedTarget = {};
            }
            events_.push_back(
                {context.tick,
                 DomainEventType::OrderStopped,
                 command.subject,
                 0,
                 0});
            return;
        }

        if (!command.append) {
            queue->pending.clear();
            clearPath(*agent);
        }
        queue->pending.push_back(
            {OrderType::Move, {command.targetX, command.targetY}});
        clearTarget(world, command.subject);
        if (auto* directive =
                world.try_get<CombatDirective>(command.subject)) {
            directive->forcedTarget = {};
            directive->mode = queue->pending.front().type ==
                                      OrderType::AttackMove
                ? CombatMode::AttackMove
                : CombatMode::PassiveMove;
        }
        events_.push_back(
            {context.tick,
             DomainEventType::MoveAccepted,
             command.subject,
             0,
             0});
    }

    void processAttackMoveCommand(ecs::World& world,
                                  const ecs::SystemContext& context,
                                  const TickCommand& command) {
        if (!world.alive(command.subject)) return;
        auto* queue = world.try_get<OrderQueue>(command.subject);
        auto* agent = world.try_get<MovementAgent>(command.subject);
        auto* directive =
            world.try_get<CombatDirective>(command.subject);
        auto* target = world.try_get<CombatTarget>(command.subject);
        const auto* weapon = world.try_get<Weapon>(command.subject);
        if (!queue || !agent || !directive || !target || !weapon) {
            events_.push_back(
                {context.tick,
                 DomainEventType::AttackRejected,
                 command.subject,
                 0,
                 1});
            return;
        }

        if (!command.append) {
            queue->pending.clear();
            clearPath(*agent);
        }
        queue->pending.push_back(
            {OrderType::AttackMove, {command.targetX, command.targetY}});
        directive->forcedTarget = {};
        directive->mode = queue->pending.front().type ==
                                  OrderType::AttackMove
            ? CombatMode::AttackMove
            : CombatMode::PassiveMove;
        target->entity = {};
        events_.push_back(
            {context.tick,
             DomainEventType::AttackMoveAccepted,
             command.subject,
             0,
             0});
    }

    void processAttackCommand(ecs::World& world,
                              const ecs::SystemContext& context,
                              const TickCommand& command) {
        auto* queue = world.try_get<OrderQueue>(command.subject);
        auto* agent = world.try_get<MovementAgent>(command.subject);
        auto* directive =
            world.try_get<CombatDirective>(command.subject);
        auto* target = world.try_get<CombatTarget>(command.subject);
        const auto* attackerTeam = world.try_get<Team>(command.subject);
        const auto* weapon = world.try_get<Weapon>(command.subject);
        const auto* targetTeam = world.try_get<Team>(command.targetEntity);
        const auto* targetHealth =
            world.try_get<Health>(command.targetEntity);
        const auto* targetPosition =
            world.try_get<Position>(command.targetEntity);
        const bool valid =
            world.alive(command.subject) &&
            world.alive(command.targetEntity) &&
            queue && agent && directive && target && attackerTeam && weapon &&
            targetTeam && targetHealth && targetPosition &&
            targetHealth->current > 0 &&
            targetTeam->id != attackerTeam->id;
        if (!valid) {
            events_.push_back(
                {context.tick,
                 DomainEventType::AttackRejected,
                 command.subject,
                 0,
                 2,
                 command.targetEntity,
                 0});
            return;
        }

        queue->pending.clear();
        clearPath(*agent);
        directive->mode = CombatMode::AttackTarget;
        directive->forcedTarget = command.targetEntity;
        target->entity = command.targetEntity;
        events_.push_back(
            {context.tick,
             DomainEventType::AttackAccepted,
             command.subject,
             0,
             0,
             command.targetEntity,
             0});
    }

    void processHoldPositionCommand(ecs::World& world,
                                    const ecs::SystemContext& context,
                                    const TickCommand& command) {
        auto* queue = world.try_get<OrderQueue>(command.subject);
        auto* agent = world.try_get<MovementAgent>(command.subject);
        auto* directive =
            world.try_get<CombatDirective>(command.subject);
        auto* target = world.try_get<CombatTarget>(command.subject);
        if (!world.alive(command.subject) || !queue || !agent ||
            !directive || !target) {
            events_.push_back(
                {context.tick,
                 DomainEventType::AttackRejected,
                 command.subject,
                 0,
                 3});
            return;
        }

        queue->pending.clear();
        clearPath(*agent);
        directive->mode = CombatMode::HoldPosition;
        directive->forcedTarget = {};
        target->entity = {};
        events_.push_back(
            {context.tick,
             DomainEventType::HoldPositionAccepted,
             command.subject,
             0,
             0});
    }

    void beginProduction(ecs::World& world,
                         const ecs::SystemContext& context,
                         const TickCommand& command) {
        auto* queue = world.try_get<ProductionQueue>(command.subject);
        const auto* building = world.try_get<Building>(command.subject);
        const auto* definition = unitDefinition(command.definitionId);
        if (!queue || !building || !building->producer || !definition ||
            definition->id == 0 || definition->cost < 0 ||
            !resources_.reserve(definition->cost)) {
            events_.push_back(
                {context.tick,
                 DomainEventType::ProductionRejected,
                 command.subject,
                 0,
                 command.definitionId});
            return;
        }

        const ProductionId id = ++nextProductionId_;
        queue->items.push_back(
            {id,
             definition->id,
             definition->cost,
             0,
             std::max<std::uint32_t>(1, definition->trainTicks)});
        events_.push_back(
            {context.tick,
             DomainEventType::ProductionAccepted,
             command.subject,
             id,
             definition->id});
    }

    void cancelProduction(ecs::World& world,
                          const ecs::SystemContext& context,
                          const TickCommand& command) {
        auto* queue = world.try_get<ProductionQueue>(command.subject);
        if (!queue) {
            events_.push_back(
                {context.tick,
                 DomainEventType::ProductionRejected,
                 command.subject,
                 command.objectId,
                 0});
            return;
        }
        const auto iterator =
            std::find_if(queue->items.begin(), queue->items.end(),
                         [&](const ProductionItem& item) {
                             return item.id == command.objectId;
                         });
        if (iterator == queue->items.end()) {
            events_.push_back(
                {context.tick,
                 DomainEventType::ProductionRejected,
                 command.subject,
                 command.objectId,
                 0});
            return;
        }
        resources_.release(iterator->reservedCost);
        queue->items.erase(iterator);
        events_.push_back(
            {context.tick,
             DomainEventType::ProductionCancelled,
             command.subject,
             command.objectId,
             0});
    }

    void advanceProduction(ecs::World& world,
                           const ecs::SystemContext& context) {
        for (const auto entity :
             world.view<Building, ProductionQueue, RallyPoint>()) {
            auto* queue = world.try_get<ProductionQueue>(entity);
            const auto* rally = world.try_get<RallyPoint>(entity);
            if (!queue || !rally || queue->items.empty()) continue;

            auto& item = queue->items.front();
            ++item.progressTicks;
            if (item.progressTicks < item.requiredTicks) continue;

            const auto* definition =
                unitDefinition(item.unitDefinitionId);
            if (!definition) {
                const auto rejectedId = item.id;
                const auto rejectedDefinition = item.unitDefinitionId;
                resources_.release(item.reservedCost);
                queue->items.erase(queue->items.begin());
                events_.push_back(
                    {context.tick,
                     DomainEventType::ProductionRejected,
                     entity,
                     rejectedId,
                     rejectedDefinition});
                continue;
            }

            resources_.commit(item.reservedCost);
            const auto producedId = item.id;
            const auto* ownerTeam = world.try_get<Team>(entity);
            const auto deferred = structuralCommands_.create(context);
            structuralCommands_.add(
                context,
                deferred,
                Position{rally->point.x, rally->point.y});
            structuralCommands_.add(
                context,
                deferred,
                MoveSpeed{definition->cellsPerTick});
            structuralCommands_.add(context, deferred, OrderQueue{});
            structuralCommands_.add(context, deferred, MovementAgent{});
            structuralCommands_.add(
                context,
                deferred,
                Team{ownerTeam ? ownerTeam->id : 0});
            queueCombatProfile(context, deferred, definition->combat);
            queue->items.erase(queue->items.begin());
            events_.push_back(
                {context.tick,
                 DomainEventType::ProductionCompleted,
                 entity,
                 producedId,
                 definition->id});
        }
    }

    void synchronizeConstructionCombat(
        ecs::World& world,
        const ecs::SystemContext& context) {
        for (const auto entity :
             world.view<ConstructionSite, BuildingFootprint>()) {
            const auto* site = world.try_get<ConstructionSite>(entity);
            const auto* footprint =
                world.try_get<BuildingFootprint>(entity);
            if (!site || !footprint) continue;

            if (!world.try_get<Position>(entity)) {
                structuralCommands_.add(
                    context,
                    entity,
                    Position{footprint->origin.x, footprint->origin.y});
            }
            if (!world.try_get<Team>(entity)) {
                structuralCommands_.add(
                    context, entity, Team{site->ownerTeam});
            }
            const auto* definition =
                buildingDefinition(site->definitionId);
            if (definition && definition->combat.maximumHealth > 0 &&
                !world.try_get<Health>(entity)) {
                queueCombatProfile(
                    context, entity, definition->combat);
            }
        }
    }

    void updateNavigation(ecs::World& world,
                          const ecs::SystemContext& context) {
        for (const auto entity :
             world.view<Position, OrderQueue, MovementAgent>()) {
            auto* position = world.try_get<Position>(entity);
            auto* queue = world.try_get<OrderQueue>(entity);
            auto* agent = world.try_get<MovementAgent>(entity);
            auto* directive =
                world.try_get<CombatDirective>(entity);
            if (!position || !queue || !agent) continue;

            if (directive &&
                directive->mode == CombatMode::AttackTarget) {
                updateAttackTargetNavigation(
                    world, context, entity, *position, *agent,
                    *directive);
                continue;
            }

            if (queue->pending.empty()) {
                if (directive &&
                    (directive->mode == CombatMode::PassiveMove ||
                     directive->mode == CombatMode::AttackMove)) {
                    directive->mode = CombatMode::Guard;
                }
                continue;
            }

            if (directive &&
                directive->mode != CombatMode::HoldPosition) {
                directive->forcedTarget = {};
                directive->mode =
                    queue->pending.front().type ==
                            OrderType::AttackMove
                        ? CombatMode::AttackMove
                        : CombatMode::PassiveMove;
            }

            if (agent->pathRevision != navigation_.revision()) {
                clearPath(*agent);
            }

            const auto goal = queue->pending.front().target;
            if (position->x == goal.x && position->y == goal.y) {
                queue->pending.erase(queue->pending.begin());
                clearPath(*agent);
                applyFrontOrderMode(world, entity, *queue);
                events_.push_back(
                    {context.tick,
                     DomainEventType::MoveCompleted,
                     entity,
                     0,
                     0});
                continue;
            }

            if (!agent->path.empty() && agent->hasPathGoal &&
                agent->pathGoal == goal &&
                agent->pathRevision == navigation_.revision() &&
                !agent->combatPath) {
                continue;
            }

            const auto path = GridPathfinder::find(
                navigation_,
                {position->x, position->y},
                goal);
            agent->pathRevision = navigation_.revision();
            if (!path.found) {
                queue->pending.erase(queue->pending.begin());
                clearPath(*agent);
                applyFrontOrderMode(world, entity, *queue);
                events_.push_back(
                    {context.tick,
                     DomainEventType::PathFailed,
                     entity,
                     0,
                     0});
                continue;
            }
            assignPath(*agent, path, goal, false);
            events_.push_back(
                {context.tick,
                 DomainEventType::PathReady,
                 entity,
                 0,
                 0});
        }
    }

    void updateAttackTargetNavigation(
        ecs::World& world,
        const ecs::SystemContext& context,
        ecs::Entity entity,
        const Position& position,
        MovementAgent& agent,
        CombatDirective& directive) {
        const auto* ownTeam = world.try_get<Team>(entity);
        const auto* weapon = world.try_get<Weapon>(entity);
        auto* target = world.try_get<CombatTarget>(entity);
        const auto* targetTeam =
            world.try_get<Team>(directive.forcedTarget);
        const auto* targetHealth =
            world.try_get<Health>(directive.forcedTarget);
        const auto* targetPosition =
            world.try_get<Position>(directive.forcedTarget);

        const bool valid =
            ownTeam && weapon && target && targetTeam &&
            targetHealth && targetPosition &&
            world.alive(directive.forcedTarget) &&
            targetHealth->current > 0 &&
            targetTeam->id != ownTeam->id;
        if (!valid) {
            const auto lost = directive.forcedTarget;
            directive.mode = CombatMode::Guard;
            directive.forcedTarget = {};
            if (target) target->entity = {};
            clearPath(agent);
            events_.push_back(
                {context.tick,
                 DomainEventType::AttackTargetLost,
                 entity,
                 0,
                 0,
                 lost,
                 0});
            return;
        }

        target->entity = directive.forcedTarget;
        const GridPoint start{position.x, position.y};
        const GridPoint targetPoint{
            targetPosition->x, targetPosition->y};
        if (distance(start, targetPoint) <= weapon->range) {
            clearPath(agent);
            return;
        }

        const bool targetChanged =
            agent.chaseTarget != directive.forcedTarget ||
            !(agent.chaseTargetPosition == targetPoint);
        const bool needsPath =
            agent.path.empty() || !agent.combatPath ||
            agent.pathRevision != navigation_.revision() ||
            targetChanged;
        if (!needsPath) return;

        const auto path =
            findAttackPath(start, targetPoint, weapon->range);
        if (!path.found) {
            const auto lost = directive.forcedTarget;
            directive.mode = CombatMode::Guard;
            directive.forcedTarget = {};
            target->entity = {};
            clearPath(agent);
            events_.push_back(
                {context.tick,
                 DomainEventType::PathFailed,
                 entity,
                 0,
                 0,
                 lost,
                 0});
            return;
        }

        const auto goal =
            path.points.empty() ? start : path.points.back();
        agent.pathRevision = navigation_.revision();
        assignPath(agent, path, goal, true);
        agent.chaseTarget = directive.forcedTarget;
        agent.chaseTargetPosition = targetPoint;
        events_.push_back(
            {context.tick,
             DomainEventType::PathReady,
             entity,
             0,
             0,
             directive.forcedTarget,
             0});
    }

    PathResult findAttackPath(GridPoint start,
                              GridPoint target,
                              std::int32_t range) const {
        std::vector<GridPoint> candidates;
        const auto boundedRange =
            std::max<std::int32_t>(0, range);
        for (std::int32_t y = 0;
             y < navigation_.height();
             ++y) {
            for (std::int32_t x = 0;
                 x < navigation_.width();
                 ++x) {
                const GridPoint candidate{x, y};
                if (!navigation_.blocked(candidate) &&
                    distance(candidate, target) <= boundedRange) {
                    candidates.push_back(candidate);
                }
            }
        }
        std::stable_sort(
            candidates.begin(),
            candidates.end(),
            [start](GridPoint a, GridPoint b) {
                const auto aDistance = distance(start, a);
                const auto bDistance = distance(start, b);
                if (aDistance != bDistance) {
                    return aDistance < bDistance;
                }
                if (a.y != b.y) return a.y < b.y;
                return a.x < b.x;
            });

        for (const auto candidate : candidates) {
            const auto path =
                GridPathfinder::find(navigation_, start, candidate);
            if (path.found) return path;
        }
        return {};
    }

    static void assignPath(MovementAgent& agent,
                           const PathResult& path,
                           GridPoint goal,
                           bool combatPath) {
        agent.path = path.points;
        agent.nextPoint = 0;
        agent.pathGoal = goal;
        agent.hasPathGoal = true;
        agent.combatPath = combatPath;
    }

    bool shouldPauseForCombat(const ecs::World& world,
                              ecs::Entity entity,
                              const Position& position) const {
        const auto* directive =
            world.try_get<CombatDirective>(entity);
        const auto* target = world.try_get<CombatTarget>(entity);
        const auto* weapon = world.try_get<Weapon>(entity);
        if (!directive || !target || !weapon ||
            directive->mode == CombatMode::PassiveMove ||
            !world.alive(target->entity)) {
            return false;
        }
        const auto* targetPosition =
            world.try_get<Position>(target->entity);
        const auto* targetHealth =
            world.try_get<Health>(target->entity);
        return targetPosition && targetHealth &&
               targetHealth->current > 0 &&
               distance(
                   {position.x, position.y},
                   {targetPosition->x, targetPosition->y}) <=
                   weapon->range;
    }

    void updateMovement(ecs::World& world,
                        const ecs::SystemContext& context) {
        for (const auto entity :
             world.view<Position, MoveSpeed, OrderQueue,
                        MovementAgent>()) {
            auto* position = world.try_get<Position>(entity);
            const auto* speed = world.try_get<MoveSpeed>(entity);
            auto* queue = world.try_get<OrderQueue>(entity);
            auto* agent = world.try_get<MovementAgent>(entity);
            if (!position || !speed || !queue || !agent ||
                agent->path.empty()) {
                continue;
            }
            if (shouldPauseForCombat(
                    world, entity, *position)) {
                continue;
            }

            const auto amount =
                std::max<std::int32_t>(1, speed->cellsPerTick);
            for (std::int32_t step = 0;
                 step < amount &&
                 agent->nextPoint < agent->path.size();
                 ++step) {
                const auto point =
                    agent->path[agent->nextPoint++];
                position->x = point.x;
                position->y = point.y;
            }
            if (agent->nextPoint != agent->path.size()) {
                continue;
            }

            const bool completedCombatPath =
                agent->combatPath;
            clearPath(*agent);
            if (completedCombatPath) continue;

            if (!queue->pending.empty()) {
                queue->pending.erase(queue->pending.begin());
            }
            applyFrontOrderMode(world, entity, *queue);
            events_.push_back(
                {context.tick,
                 DomainEventType::MoveCompleted,
                 entity,
                 0,
                 0});
        }
    }

    void attachCombatProfile(ecs::Entity entity,
                             const CombatStats& profile) {
        if (profile.maximumHealth <= 0) return;
        world_.emplace<Health>(
            entity,
            Health{profile.maximumHealth,
                   profile.maximumHealth});
        world_.emplace<Armor>(
            entity,
            Armor{std::max<std::int32_t>(0, profile.armor)});
        if (profile.bounty > 0) {
            world_.emplace<Bounty>(
                entity,
                Bounty{profile.bounty});
        }
        if (profile.attackCapable()) {
            world_.emplace<Weapon>(
                entity,
                Weapon{
                    profile.weaponDamage,
                    profile.weaponRange,
                    std::max<std::uint32_t>(
                        1, profile.cooldownTicks),
                    0});
            world_.emplace<CombatTarget>(
                entity, CombatTarget{});
            world_.emplace<CombatDirective>(
                entity, CombatDirective{});
        }
    }

    template<class Target>
    void queueCombatProfile(
        const ecs::SystemContext& context,
        Target target,
        const CombatStats& profile) {
        if (profile.maximumHealth <= 0) return;
        structuralCommands_.add(
            context,
            target,
            Health{profile.maximumHealth,
                   profile.maximumHealth});
        structuralCommands_.add(
            context,
            target,
            Armor{std::max<std::int32_t>(
                0, profile.armor)});
        if (profile.bounty > 0) {
            structuralCommands_.add(
                context, target, Bounty{profile.bounty});
        }
        if (profile.attackCapable()) {
            structuralCommands_.add(
                context,
                target,
                Weapon{
                    profile.weaponDamage,
                    profile.weaponRange,
                    std::max<std::uint32_t>(
                        1, profile.cooldownTicks),
                    0});
            structuralCommands_.add(
                context, target, CombatTarget{});
            structuralCommands_.add(
                context, target, CombatDirective{});
        }
    }

    void handleDeath(ecs::World& world,
                     const ecs::SystemContext& context,
                     ecs::Entity victim,
                     ecs::Entity killer) {
        const auto* footprint =
            world.try_get<BuildingFootprint>(victim);
        if (footprint) {
            building_.releaseFootprint(*footprint);
        }

        const auto* site =
            world.try_get<ConstructionSite>(victim);
        if (site) {
            resources_.release(site->reservedCost);
            deathSideEffects_.push_back(
                {context.tick,
                 DomainEventType::ConstructionDestroyed,
                 victim,
                 site->id,
                 0,
                 killer,
                 0});
        }

        const auto* production =
            world.try_get<ProductionQueue>(victim);
        if (production) {
            for (const auto& item : production->items) {
                resources_.release(item.reservedCost);
            }
        }

        const auto* bounty = world.try_get<Bounty>(victim);
        const auto* killerTeam = world.try_get<Team>(killer);
        const auto* victimTeam = world.try_get<Team>(victim);
        if (bounty && bounty->amount > 0 && killerTeam &&
            victimTeam &&
            killerTeam->id == playerTeamId_ &&
            killerTeam->id != victimTeam->id) {
            resources_.available += bounty->amount;
            deathSideEffects_.push_back(
                {context.tick,
                 DomainEventType::BountyAwarded,
                 killer,
                 0,
                 0,
                 victim,
                 bounty->amount});
        }
    }

    void forwardCombatEvents() {
        for (const auto& event : combat_.events()) {
            switch (event.type) {
            case CombatEventType::TargetAcquired:
                events_.push_back(
                    {event.tick,
                     DomainEventType::TargetAcquired,
                     event.source,
                     0,
                     0,
                     event.target,
                     event.value});
                break;
            case CombatEventType::WeaponFired:
                events_.push_back(
                    {event.tick,
                     DomainEventType::WeaponFired,
                     event.source,
                     0,
                     0,
                     event.target,
                     event.value});
                break;
            case CombatEventType::DamageApplied:
                events_.push_back(
                    {event.tick,
                     DomainEventType::DamageApplied,
                     event.source,
                     0,
                     0,
                     event.target,
                     event.value});
                break;
            case CombatEventType::EntityDied:
                events_.push_back(
                    {event.tick,
                     DomainEventType::EntityDied,
                     event.target,
                     0,
                     0,
                     event.source,
                     0});
                break;
            }
        }
        events_.insert(events_.end(),
                       deathSideEffects_.begin(),
                       deathSideEffects_.end());
    }

    static void populateCombatSnapshot(
        const ecs::World& world,
        ecs::Entity entity,
        SnapshotEntity& snapshot) {
        if (const auto* team = world.try_get<Team>(entity)) {
            snapshot.teamId = team->id;
        }
        if (const auto* health =
                world.try_get<Health>(entity)) {
            snapshot.healthCurrent = health->current;
            snapshot.healthMaximum = health->maximum;
        }
        if (const auto* armor =
                world.try_get<Armor>(entity)) {
            snapshot.armor = armor->value;
        }
        if (const auto* weapon =
                world.try_get<Weapon>(entity)) {
            snapshot.cooldownRemaining =
                weapon->cooldownRemaining;
        }
        if (const auto* target =
                world.try_get<CombatTarget>(entity)) {
            snapshot.target = target->entity;
        }
        if (const auto* directive =
                world.try_get<CombatDirective>(entity)) {
            snapshot.combatMode = directive->mode;
        }
    }

    static void hashEntity(
        foundation::CanonicalHash& hash,
        ecs::Entity entity) {
        hash.WriteU32(entity.index);
        hash.WriteU32(entity.generation);
    }

    static void hashOptionalEntity(
        foundation::CanonicalHash& hash,
        ecs::Entity entity) {
        hash.WriteU32(entity.index);
        hash.WriteU32(entity.generation);
    }

    static void hashCombat(
        foundation::CanonicalHash& hash,
        const ecs::World& world,
        ecs::Entity entity) {
        const auto* team = world.try_get<Team>(entity);
        hash.WriteBool(team != nullptr);
        if (team) hash.WriteU32(team->id);

        const auto* health = world.try_get<Health>(entity);
        hash.WriteBool(health != nullptr);
        if (health) {
            hash.WriteI32(health->current);
            hash.WriteI32(health->maximum);
        }

        const auto* armor = world.try_get<Armor>(entity);
        hash.WriteBool(armor != nullptr);
        if (armor) hash.WriteI32(armor->value);

        const auto* weapon = world.try_get<Weapon>(entity);
        hash.WriteBool(weapon != nullptr);
        if (weapon) {
            hash.WriteI32(weapon->damage);
            hash.WriteI32(weapon->range);
            hash.WriteU32(weapon->cooldownTicks);
            hash.WriteU32(weapon->cooldownRemaining);
        }

        const auto* target =
            world.try_get<CombatTarget>(entity);
        hash.WriteBool(target != nullptr);
        if (target) hashOptionalEntity(hash, target->entity);

        const auto* directive =
            world.try_get<CombatDirective>(entity);
        hash.WriteBool(directive != nullptr);
        if (directive) {
            hash.WriteU8(
                static_cast<std::uint8_t>(directive->mode));
            hashOptionalEntity(hash, directive->forcedTarget);
        }

        const auto* bounty = world.try_get<Bounty>(entity);
        hash.WriteBool(bounty != nullptr);
        if (bounty) hash.WriteI32(bounty->amount);
    }

    static void hashFootprint(
        foundation::CanonicalHash& hash,
        const BuildingFootprint& footprint) {
        hash.WriteI32(footprint.origin.x);
        hash.WriteI32(footprint.origin.y);
        hash.WriteI32(footprint.width);
        hash.WriteI32(footprint.height);
    }

    static void hashUnit(
        foundation::CanonicalHash& hash,
        const ecs::World& world,
        ecs::Entity entity,
        const Position& position,
        const OrderQueue& queue,
        const MovementAgent& agent,
        bool moving) {
        hashEntity(hash, entity);
        hash.WriteI32(position.x);
        hash.WriteI32(position.y);
        hash.WriteBool(moving);
        hash.WriteU32(
            static_cast<std::uint32_t>(queue.pending.size()));
        for (const auto& order : queue.pending) {
            hash.WriteU8(
                static_cast<std::uint8_t>(order.type));
            hash.WriteI32(order.target.x);
            hash.WriteI32(order.target.y);
        }
        hash.WriteU64(agent.pathRevision);
        hash.WriteU64(
            static_cast<std::uint64_t>(agent.nextPoint));
        hash.WriteU64(
            static_cast<std::uint64_t>(agent.path.size()));
        for (const auto point : agent.path) {
            hash.WriteI32(point.x);
            hash.WriteI32(point.y);
        }
        hash.WriteI32(agent.pathGoal.x);
        hash.WriteI32(agent.pathGoal.y);
        hash.WriteBool(agent.hasPathGoal);
        hash.WriteBool(agent.combatPath);
        hashOptionalEntity(hash, agent.chaseTarget);
        hash.WriteI32(agent.chaseTargetPosition.x);
        hash.WriteI32(agent.chaseTargetPosition.y);
        hashCombat(hash, world, entity);
    }

    void buildSnapshot(ecs::World& world,
                       std::uint64_t tick) {
        snapshot_.tick = tick;
        snapshot_.resources = resources_;
        snapshot_.entities.clear();

        foundation::CanonicalHash hash;
        hash.WriteU64(tick);
        hash.WriteI32(resources_.available);
        hash.WriteI32(resources_.reserved);
        hash.WriteI32(resources_.spent);
        hash.WriteU64(navigation_.revision());
        for (const auto blocked : navigation_.blockers()) {
            hash.WriteU8(blocked);
        }

        for (const auto entity :
             world.view<Position, OrderQueue,
                        MovementAgent>()) {
            const auto* position =
                world.try_get<Position>(entity);
            const auto* queue =
                world.try_get<OrderQueue>(entity);
            const auto* agent =
                world.try_get<MovementAgent>(entity);
            const bool moving =
                !agent->path.empty() ||
                !queue->pending.empty() ||
                agent->combatPath;

            SnapshotEntity value;
            value.entity = entity;
            value.x = position->x;
            value.y = position->y;
            value.moving = moving;
            value.queuedOrders =
                static_cast<std::uint32_t>(
                    queue->pending.size());
            value.kind = SnapshotKind::Unit;
            populateCombatSnapshot(world, entity, value);
            snapshot_.entities.push_back(value);

            hash.WriteU8(
                static_cast<std::uint8_t>(
                    SnapshotKind::Unit));
            hashUnit(
                hash, world, entity, *position, *queue,
                *agent, moving);
        }

        for (const auto entity :
             world.view<ConstructionSite,
                        BuildingFootprint>()) {
            const auto* site =
                world.try_get<ConstructionSite>(entity);
            const auto* footprint =
                world.try_get<BuildingFootprint>(entity);

            SnapshotEntity value;
            value.entity = entity;
            value.x = footprint->origin.x;
            value.y = footprint->origin.y;
            value.kind = SnapshotKind::Construction;
            value.definitionId = site->definitionId;
            value.progressTicks = site->progressTicks;
            value.requiredTicks = site->requiredTicks;
            populateCombatSnapshot(world, entity, value);
            snapshot_.entities.push_back(value);

            hash.WriteU8(
                static_cast<std::uint8_t>(
                    SnapshotKind::Construction));
            hashEntity(hash, entity);
            hash.WriteU32(site->id);
            hash.WriteU32(site->definitionId);
            hash.WriteI32(site->reservedCost);
            hash.WriteU32(site->progressTicks);
            hash.WriteU32(site->requiredTicks);
            hash.WriteBool(site->producer);
            hash.WriteU32(site->ownerTeam);
            hashFootprint(hash, *footprint);
            hashCombat(hash, world, entity);
        }

        for (const auto entity :
             world.view<Building, BuildingFootprint>()) {
            const auto* building =
                world.try_get<Building>(entity);
            const auto* footprint =
                world.try_get<BuildingFootprint>(entity);
            const auto* queue =
                world.try_get<ProductionQueue>(entity);
            const auto queueSize =
                queue
                    ? static_cast<std::uint32_t>(
                          queue->items.size())
                    : 0;

            SnapshotEntity value;
            value.entity = entity;
            value.x = footprint->origin.x;
            value.y = footprint->origin.y;
            value.kind = SnapshotKind::Building;
            value.definitionId = building->definitionId;
            value.productionQueueSize = queueSize;
            populateCombatSnapshot(world, entity, value);
            snapshot_.entities.push_back(value);

            hash.WriteU8(
                static_cast<std::uint8_t>(
                    SnapshotKind::Building));
            hashEntity(hash, entity);
            hash.WriteU32(building->definitionId);
            hash.WriteBool(building->producer);
            hashFootprint(hash, *footprint);
            hash.WriteU32(queueSize);
            if (queue) {
                for (const auto& item : queue->items) {
                    hash.WriteU32(item.id);
                    hash.WriteU32(item.unitDefinitionId);
                    hash.WriteI32(item.reservedCost);
                    hash.WriteU32(item.progressTicks);
                    hash.WriteU32(item.requiredTicks);
                }
            }
            const auto* rally =
                world.try_get<RallyPoint>(entity);
            hash.WriteBool(rally != nullptr);
            if (rally) {
                hash.WriteI32(rally->point.x);
                hash.WriteI32(rally->point.y);
            }
            hashCombat(hash, world, entity);
        }

        std::sort(
            snapshot_.entities.begin(),
            snapshot_.entities.end(),
            [](const SnapshotEntity& a,
               const SnapshotEntity& b) {
                return a.entity < b.entity;
            });
        snapshot_.worldHash = hash.Value();
    }

    ecs::World world_;
    ecs::Scheduler scheduler_;
    TickCommandStream commands_;
    ecs::EntityCommandBuffer structuralCommands_;
    NavigationGrid navigation_;
    ResourceLedger resources_;
    BaseBuildingRuntime building_;
    CombatRuntime combat_;
    std::vector<TickCommand> activeCommands_;
    std::vector<DomainEvent> events_;
    std::vector<DomainEvent> deathSideEffects_;
    WorldSnapshot snapshot_;
    std::vector<BuildingDefinition> buildingDefinitions_;
    std::vector<UnitDefinition> unitDefinitions_;
    GridPoint requiredPathStart_{0, 0};
    GridPoint requiredPathGoal_{0, 0};
    ProductionId nextProductionId_{};
    std::uint32_t playerTeamId_{1};
};

} // namespace rts::gameplay
