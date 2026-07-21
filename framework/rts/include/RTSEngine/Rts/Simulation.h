#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/Scheduler.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/Navigation.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::gameplay {

struct Position {
    std::int32_t x{};
    std::int32_t y{};
};

struct MoveSpeed {
    std::int32_t cellsPerTick{1};
};

enum class OrderType : std::uint8_t { Move };

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
};

struct UnitDefinition {
    std::uint32_t id{};
    std::int32_t cost{};
    std::uint32_t trainTicks{1};
    std::int32_t cellsPerTick{1};
};

enum class CommandType : std::uint8_t {
    Move,
    Stop,
    Build,
    CancelConstruction,
    Train,
    CancelProduction,
    SetRally
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
        result.erase(std::unique(result.begin(), result.end(), [](const TickCommand& a, const TickCommand& b) {
            return a.issuer == b.issuer && a.sequence == b.sequence;
        }), result.end());
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
    ProductionAccepted,
    ProductionRejected,
    ProductionCancelled,
    ProductionCompleted,
    RallyPointChanged
};

struct DomainEvent {
    std::uint64_t tick{};
    DomainEventType type{};
    ecs::Entity entity{};
    std::uint32_t objectId{};
    std::uint32_t reason{};
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
        : navigation_(width, height), building_(resources_, navigation_) {
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

    ecs::Entity createUnit(Position position, MoveSpeed speed) {
        const auto entity = world_.create();
        world_.emplace<Position>(entity, position);
        world_.emplace<MoveSpeed>(entity, speed);
        world_.emplace<OrderQueue>(entity, OrderQueue{});
        world_.emplace<MovementAgent>(entity, MovementAgent{});
        return entity;
    }

    bool submit(TickCommand command) { return commands_.submit(command); }
    bool setBlocked(GridPoint point, bool blocked) { return navigation_.setBlocked(point, blocked); }
    const NavigationGrid& navigation() const noexcept { return navigation_; }
    const ResourceLedger& resources() const noexcept { return resources_; }

    void step(std::uint64_t tick) {
        events_.clear();
        activeCommands_ = commands_.consume(tick);

        runStage(tick, ecs::Stage::Command);
        runStage(tick, ecs::Stage::Navigation);
        runStage(tick, ecs::Stage::Simulation);
        runStage(tick, ecs::Stage::Cleanup);
        scheduler_.run_stage(world_, tick, ecs::Stage::Snapshot);
    }

    const WorldSnapshot& snapshot() const noexcept { return snapshot_; }
    const std::vector<DomainEvent>& events() const noexcept { return events_; }
    const ecs::World& world() const noexcept { return world_; }

private:
    template<class Definition>
    static void replaceDefinition(std::vector<Definition>& definitions, Definition definition) {
        auto iterator = std::find_if(definitions.begin(), definitions.end(), [&](const Definition& value) {
            return value.id == definition.id;
        });
        if (iterator == definitions.end()) definitions.push_back(definition);
        else *iterator = definition;
        std::sort(definitions.begin(), definitions.end(), [](const Definition& a, const Definition& b) {
            return a.id < b.id;
        });
    }

    const BuildingDefinition* buildingDefinition(std::uint32_t id) const {
        const auto iterator = std::find_if(buildingDefinitions_.begin(), buildingDefinitions_.end(),
                                           [id](const BuildingDefinition& value) { return value.id == id; });
        return iterator == buildingDefinitions_.end() ? nullptr : &*iterator;
    }

    const UnitDefinition* unitDefinition(std::uint32_t id) const {
        const auto iterator = std::find_if(unitDefinitions_.begin(), unitDefinitions_.end(),
                                           [id](const UnitDefinition& value) { return value.id == id; });
        return iterator == unitDefinitions_.end() ? nullptr : &*iterator;
    }

    void runStage(std::uint64_t tick, ecs::Stage stage) {
        scheduler_.run_stage(world_, tick, stage);
        structuralCommands_.commit_through(world_, stage);
    }

    void installSystems() {
        scheduler_.add(ecs::Stage::Command, 0, 100, [this](ecs::World& world, const ecs::SystemContext& context) {
            for (const auto& command : activeCommands_) processCommand(world, context, command);
        });

        scheduler_.add(ecs::Stage::Navigation, 0, 200, [this](ecs::World& world, const ecs::SystemContext& context) {
            updateNavigation(world, context);
        });

        scheduler_.add(ecs::Stage::Simulation, 0, 300, [this](ecs::World& world, const ecs::SystemContext& context) {
            std::vector<ConstructionId> completing;
            for (const auto entity : world.view<ConstructionSite>()) {
                const auto* site = world.try_get<ConstructionSite>(entity);
                if (site && site->progressTicks + 1 >= site->requiredTicks) completing.push_back(site->id);
            }
            building_.advance(context, structuralCommands_, world);
            for (const auto id : completing) {
                events_.push_back({context.tick, DomainEventType::ConstructionCompleted, {}, id, 0});
            }
        });

        scheduler_.add(ecs::Stage::Simulation, 10, 310, [this](ecs::World& world, const ecs::SystemContext& context) {
            advanceProduction(world, context);
        });

        scheduler_.add(ecs::Stage::Simulation, 20, 320, [this](ecs::World& world, const ecs::SystemContext& context) {
            updateMovement(world, context);
        });

        scheduler_.add(ecs::Stage::Snapshot, 0, 400, [this](ecs::World& world, const ecs::SystemContext& context) {
            buildSnapshot(world, context.tick);
        });
    }

    void processCommand(ecs::World& world, const ecs::SystemContext& context, const TickCommand& command) {
        switch (command.type) {
        case CommandType::Move:
        case CommandType::Stop:
            processOrderCommand(world, context, command);
            break;
        case CommandType::Build: {
            const auto* definition = buildingDefinition(command.definitionId);
            BuildResult result;
            if (definition) {
                result = building_.begin(context, structuralCommands_, *definition,
                                         {command.targetX, command.targetY},
                                         requiredPathStart_, requiredPathGoal_);
            } else {
                result = {false, BuildFailure::InvalidDefinition, 0};
            }
            events_.push_back({context.tick,
                               result.accepted ? DomainEventType::ConstructionAccepted
                                               : DomainEventType::ConstructionRejected,
                               {}, result.constructionId,
                               static_cast<std::uint32_t>(result.failure)});
            break;
        }
        case CommandType::CancelConstruction: {
            const auto failure = building_.cancel(context, structuralCommands_, world, command.objectId);
            events_.push_back({context.tick,
                               failure == BuildFailure::None ? DomainEventType::ConstructionCancelled
                                                             : DomainEventType::ConstructionRejected,
                               {}, command.objectId, static_cast<std::uint32_t>(failure)});
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
                events_.push_back({context.tick, DomainEventType::RallyPointChanged, command.subject, 0, 0});
            }
            break;
        }
        }
    }

    void processOrderCommand(ecs::World& world, const ecs::SystemContext& context,
                             const TickCommand& command) {
        if (!world.alive(command.subject)) return;
        auto* queue = world.try_get<OrderQueue>(command.subject);
        auto* agent = world.try_get<MovementAgent>(command.subject);
        if (!queue || !agent) return;

        if (command.type == CommandType::Stop) {
            queue->pending.clear();
            agent->path.clear();
            agent->nextPoint = 0;
            events_.push_back({context.tick, DomainEventType::OrderStopped, command.subject, 0, 0});
            return;
        }

        if (!command.append) {
            queue->pending.clear();
            agent->path.clear();
            agent->nextPoint = 0;
        }
        queue->pending.push_back({OrderType::Move, {command.targetX, command.targetY}});
        events_.push_back({context.tick, DomainEventType::MoveAccepted, command.subject, 0, 0});
    }

    void beginProduction(ecs::World& world, const ecs::SystemContext& context,
                         const TickCommand& command) {
        auto* queue = world.try_get<ProductionQueue>(command.subject);
        const auto* building = world.try_get<Building>(command.subject);
        const auto* definition = unitDefinition(command.definitionId);
        if (!queue || !building || !building->producer || !definition ||
            definition->id == 0 || definition->cost < 0 || !resources_.reserve(definition->cost)) {
            events_.push_back({context.tick, DomainEventType::ProductionRejected,
                               command.subject, 0, command.definitionId});
            return;
        }

        const ProductionId id = ++nextProductionId_;
        queue->items.push_back({id, definition->id, definition->cost, 0,
                                std::max<std::uint32_t>(1, definition->trainTicks)});
        events_.push_back({context.tick, DomainEventType::ProductionAccepted,
                           command.subject, id, definition->id});
    }

    void cancelProduction(ecs::World& world, const ecs::SystemContext& context,
                          const TickCommand& command) {
        auto* queue = world.try_get<ProductionQueue>(command.subject);
        if (!queue) {
            events_.push_back({context.tick, DomainEventType::ProductionRejected,
                               command.subject, command.objectId, 0});
            return;
        }
        const auto iterator = std::find_if(queue->items.begin(), queue->items.end(), [&](const ProductionItem& item) {
            return item.id == command.objectId;
        });
        if (iterator == queue->items.end()) {
            events_.push_back({context.tick, DomainEventType::ProductionRejected,
                               command.subject, command.objectId, 0});
            return;
        }
        resources_.release(iterator->reservedCost);
        queue->items.erase(iterator);
        events_.push_back({context.tick, DomainEventType::ProductionCancelled,
                           command.subject, command.objectId, 0});
    }

    void advanceProduction(ecs::World& world, const ecs::SystemContext& context) {
        for (const auto entity : world.view<Building, ProductionQueue, RallyPoint>()) {
            auto* queue = world.try_get<ProductionQueue>(entity);
            const auto* rally = world.try_get<RallyPoint>(entity);
            if (!queue || !rally || queue->items.empty()) continue;

            auto& item = queue->items.front();
            ++item.progressTicks;
            if (item.progressTicks < item.requiredTicks) continue;

            const auto* definition = unitDefinition(item.unitDefinitionId);
            if (!definition) {
                resources_.release(item.reservedCost);
                queue->items.erase(queue->items.begin());
                events_.push_back({context.tick, DomainEventType::ProductionRejected,
                                   entity, item.id, item.unitDefinitionId});
                continue;
            }

            resources_.commit(item.reservedCost);
            const auto producedId = item.id;
            const auto deferred = structuralCommands_.create(context);
            structuralCommands_.add(context, deferred, Position{rally->point.x, rally->point.y});
            structuralCommands_.add(context, deferred, MoveSpeed{definition->cellsPerTick});
            structuralCommands_.add(context, deferred, OrderQueue{});
            structuralCommands_.add(context, deferred, MovementAgent{});
            queue->items.erase(queue->items.begin());
            events_.push_back({context.tick, DomainEventType::ProductionCompleted,
                               entity, producedId, definition->id});
        }
    }

    void updateNavigation(ecs::World& world, const ecs::SystemContext& context) {
        for (const auto entity : world.view<Position, OrderQueue, MovementAgent>()) {
            auto* position = world.try_get<Position>(entity);
            auto* queue = world.try_get<OrderQueue>(entity);
            auto* agent = world.try_get<MovementAgent>(entity);
            if (!position || !queue || !agent || queue->pending.empty()) continue;

            if (agent->pathRevision != navigation_.revision()) {
                agent->path.clear();
                agent->nextPoint = 0;
            }
            if (!agent->path.empty()) continue;

            const auto target = queue->pending.front().target;
            if (position->x == target.x && position->y == target.y) {
                queue->pending.erase(queue->pending.begin());
                events_.push_back({context.tick, DomainEventType::MoveCompleted, entity, 0, 0});
                continue;
            }

            const auto path = GridPathfinder::find(navigation_, {position->x, position->y}, target);
            agent->pathRevision = navigation_.revision();
            if (!path.found) {
                queue->pending.erase(queue->pending.begin());
                events_.push_back({context.tick, DomainEventType::PathFailed, entity, 0, 0});
                continue;
            }
            agent->path = path.points;
            agent->nextPoint = 0;
            events_.push_back({context.tick, DomainEventType::PathReady, entity, 0, 0});
        }
    }

    void updateMovement(ecs::World& world, const ecs::SystemContext& context) {
        for (const auto entity : world.view<Position, MoveSpeed, OrderQueue, MovementAgent>()) {
            auto* position = world.try_get<Position>(entity);
            const auto* speed = world.try_get<MoveSpeed>(entity);
            auto* queue = world.try_get<OrderQueue>(entity);
            auto* agent = world.try_get<MovementAgent>(entity);
            if (!position || !speed || !queue || !agent || agent->path.empty()) continue;

            const auto amount = std::max<std::int32_t>(1, speed->cellsPerTick);
            for (std::int32_t step = 0; step < amount && agent->nextPoint < agent->path.size(); ++step) {
                const auto point = agent->path[agent->nextPoint++];
                position->x = point.x;
                position->y = point.y;
            }
            if (agent->nextPoint == agent->path.size()) {
                agent->path.clear();
                agent->nextPoint = 0;
                if (!queue->pending.empty()) queue->pending.erase(queue->pending.begin());
                events_.push_back({context.tick, DomainEventType::MoveCompleted, entity, 0, 0});
            }
        }
    }

    void buildSnapshot(ecs::World& world, std::uint64_t tick) {
        snapshot_.tick = tick;
        snapshot_.resources = resources_;
        snapshot_.entities.clear();

        foundation::CanonicalHash hash;
        hash.WriteU64(tick);
        hash.WriteI32(resources_.available);
        hash.WriteI32(resources_.reserved);
        hash.WriteI32(resources_.spent);
        hash.WriteU64(navigation_.revision());
        for (const auto blocked : navigation_.blockers()) hash.WriteU8(blocked);

        for (const auto entity : world.view<Position, OrderQueue, MovementAgent>()) {
            const auto* position = world.try_get<Position>(entity);
            const auto* queue = world.try_get<OrderQueue>(entity);
            const auto* agent = world.try_get<MovementAgent>(entity);
            const bool moving = !agent->path.empty() || !queue->pending.empty();
            snapshot_.entities.push_back({entity, position->x, position->y, moving,
                                          static_cast<std::uint32_t>(queue->pending.size()),
                                          SnapshotKind::Unit, 0, 0, 0, 0});
            hash.WriteU8(static_cast<std::uint8_t>(SnapshotKind::Unit));
            hashUnit(hash, entity, *position, *queue, *agent, moving);
        }

        for (const auto entity : world.view<ConstructionSite, BuildingFootprint>()) {
            const auto* site = world.try_get<ConstructionSite>(entity);
            const auto* footprint = world.try_get<BuildingFootprint>(entity);
            snapshot_.entities.push_back({entity, footprint->origin.x, footprint->origin.y, false, 0,
                                          SnapshotKind::Construction, site->definitionId,
                                          site->progressTicks, site->requiredTicks, 0});
            hash.WriteU8(static_cast<std::uint8_t>(SnapshotKind::Construction));
            hash.WriteU32(entity.index); hash.WriteU32(entity.generation);
            hash.WriteU32(site->id); hash.WriteU32(site->definitionId);
            hash.WriteI32(site->reservedCost);
            hash.WriteU32(site->progressTicks); hash.WriteU32(site->requiredTicks);
            hashFootprint(hash, *footprint);
        }

        for (const auto entity : world.view<Building, BuildingFootprint>()) {
            const auto* building = world.try_get<Building>(entity);
            const auto* footprint = world.try_get<BuildingFootprint>(entity);
            const auto* queue = world.try_get<ProductionQueue>(entity);
            const auto queueSize = queue ? static_cast<std::uint32_t>(queue->items.size()) : 0;
            snapshot_.entities.push_back({entity, footprint->origin.x, footprint->origin.y, false, 0,
                                          SnapshotKind::Building, building->definitionId, 0, 0, queueSize});
            hash.WriteU8(static_cast<std::uint8_t>(SnapshotKind::Building));
            hash.WriteU32(entity.index); hash.WriteU32(entity.generation);
            hash.WriteU32(building->definitionId); hash.WriteBool(building->producer);
            hashFootprint(hash, *footprint);
            hash.WriteU32(queueSize);
            if (queue) {
                for (const auto& item : queue->items) {
                    hash.WriteU32(item.id); hash.WriteU32(item.unitDefinitionId);
                    hash.WriteI32(item.reservedCost); hash.WriteU32(item.progressTicks);
                    hash.WriteU32(item.requiredTicks);
                }
            }
            if (const auto* rally = world.try_get<RallyPoint>(entity)) {
                hash.WriteBool(true); hash.WriteI32(rally->point.x); hash.WriteI32(rally->point.y);
            } else hash.WriteBool(false);
        }

        std::sort(snapshot_.entities.begin(), snapshot_.entities.end(), [](const SnapshotEntity& a, const SnapshotEntity& b) {
            return a.entity < b.entity;
        });
        snapshot_.worldHash = hash.Value();
    }

    static void hashUnit(foundation::CanonicalHash& hash, ecs::Entity entity,
                         const Position& position, const OrderQueue& queue,
                         const MovementAgent& agent, bool moving) {
        hash.WriteU32(entity.index); hash.WriteU32(entity.generation);
        hash.WriteI32(position.x); hash.WriteI32(position.y); hash.WriteBool(moving);
        hash.WriteU32(static_cast<std::uint32_t>(queue.pending.size()));
        for (const auto& order : queue.pending) {
            hash.WriteU8(static_cast<std::uint8_t>(order.type));
            hash.WriteI32(order.target.x); hash.WriteI32(order.target.y);
        }
        hash.WriteU64(agent.pathRevision);
        hash.WriteU64(static_cast<std::uint64_t>(agent.nextPoint));
        hash.WriteU64(static_cast<std::uint64_t>(agent.path.size()));
        for (const auto point : agent.path) {
            hash.WriteI32(point.x); hash.WriteI32(point.y);
        }
    }

    static void hashFootprint(foundation::CanonicalHash& hash, const BuildingFootprint& footprint) {
        hash.WriteI32(footprint.origin.x); hash.WriteI32(footprint.origin.y);
        hash.WriteI32(footprint.width); hash.WriteI32(footprint.height);
    }

    ecs::World world_;
    ecs::Scheduler scheduler_;
    ecs::EntityCommandBuffer structuralCommands_;
    TickCommandStream commands_;
    NavigationGrid navigation_;
    ResourceLedger resources_;
    BaseBuildingRuntime building_;
    std::vector<BuildingDefinition> buildingDefinitions_;
    std::vector<UnitDefinition> unitDefinitions_;
    GridPoint requiredPathStart_{};
    GridPoint requiredPathGoal_{};
    ProductionId nextProductionId_{};
    std::vector<TickCommand> activeCommands_;
    std::vector<DomainEvent> events_;
    WorldSnapshot snapshot_;
};

} // namespace rts::gameplay
