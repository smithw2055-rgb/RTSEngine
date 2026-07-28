#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <RTSEngine/Rts/TeamEconomy.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

struct UnitArchetype final {
    std::uint32_t definitionId{};
};

struct ResourceNode final {
    std::uint32_t id{};
    ResourceTypeId resourceType{};
    ResourceAmount remaining{};
};

enum class HarvestState : std::uint8_t {
    Idle,
    MovingToNode,
    Harvesting,
    MovingToDropOff
};

struct WorkerHarvester final {
    ResourceAmount cargoCapacity{};
    ResourceAmount harvestAmount{1};
    std::uint32_t harvestTicks{1};
    ResourceAmount cargo{};
    ResourceTypeId cargoType{};
    ecs::Entity targetNode{};
    std::uint32_t progressTicks{};
    HarvestState state{HarvestState::Idle};
};

class HarvestCommandSystem final {
public:
    static bool handles(CommandType type) noexcept {
        return type == CommandType::Gather;
    }

    static void observeStop(
        ecs::World& world,
        const TickCommand& command) {
        if (command.type != CommandType::Stop ||
            !world.alive(command.subject)) {
            return;
        }
        auto* worker = world.try_get<WorkerHarvester>(command.subject);
        const auto* team = world.try_get<Team>(command.subject);
        if (!worker || !team || team->id != command.issuer) return;
        worker->targetNode = {};
        worker->progressTicks = 0;
        worker->state = HarvestState::Idle;
    }

    static void process(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        std::vector<DomainEvent>& events) {
        auto* worker = world.try_get<WorkerHarvester>(command.subject);
        auto* queue = world.try_get<OrderQueue>(command.subject);
        auto* agent = world.try_get<MovementAgent>(command.subject);
        const auto* team = world.try_get<Team>(command.subject);
        const auto* node = world.try_get<ResourceNode>(command.targetEntity);
        const auto* nodePosition = world.try_get<Position>(command.targetEntity);
        if (!world.alive(command.subject) || !world.alive(command.targetEntity) ||
            !worker || !queue || !agent || !team ||
            team->id != command.issuer || !node || !nodePosition ||
            node->remaining <= 0) {
            events.push_back(
                {context.tick,
                 DomainEventType::GatherRejected,
                 command.subject,
                 command.objectId,
                 static_cast<std::uint32_t>(
                     CommandRejectionReason::InvalidTarget),
                 command.targetEntity,
                 0});
            return;
        }

        worker->targetNode = command.targetEntity;
        worker->progressTicks = 0;
        if (worker->cargo > 0) {
            worker->state = HarvestState::MovingToDropOff;
            clearMovement(*queue, *agent);
        } else {
            worker->state = HarvestState::MovingToNode;
            setMove(*queue, *agent, {nodePosition->x, nodePosition->y});
        }
        events.push_back(
            {context.tick,
             DomainEventType::GatherAccepted,
             command.subject,
             node->id,
             0,
             command.targetEntity,
             0});
    }

private:
    static void clearMovement(OrderQueue& queue, MovementAgent& agent) {
        queue.pending.clear();
        resetPath(agent);
    }

    static void setMove(
        OrderQueue& queue,
        MovementAgent& agent,
        GridPoint target) {
        queue.pending.clear();
        queue.pending.push_back({OrderType::Move, target});
        resetPath(agent);
    }

    static void resetPath(MovementAgent& agent) {
        agent.path.clear();
        agent.nextPoint = 0;
        agent.pathRevision = 0;
        agent.pathGoal = {};
        agent.hasPathGoal = false;
        agent.combatPath = false;
        agent.flowFieldPath = false;
        agent.flowContext = nullptr;
        agent.flowSample = nullptr;
        agent.chaseTarget = {};
        agent.chaseTargetPosition = {};
        agent.blockedTicks = 0;
        agent.yieldOrdinal = 0;
    }

    friend class HarvestingRuntime;
};

class HarvestingRuntime final {
public:
    void reserveWorkers(std::size_t count) {
        dropOffs_.reserve(std::max(dropOffs_.capacity(), count / 8u + 1u));
    }

    void advance(
        ecs::World& world,
        const ecs::SystemContext& context,
        TeamEconomyRuntime& economy,
        std::vector<DomainEvent>& events) {
        rebuildDropOffs(world);
        world.eachRef<Position, Team, OrderQueue, MovementAgent, WorkerHarvester>(
            [&](ecs::Entity entity,
                Position& position,
                Team& team,
                OrderQueue& queue,
                MovementAgent& agent,
                WorkerHarvester& worker) {
                advanceWorker(
                    world,
                    context,
                    economy,
                    events,
                    entity,
                    position,
                    team,
                    queue,
                    agent,
                    worker);
            });
    }

    std::size_t dropOffScratchCapacity() const noexcept {
        return dropOffs_.capacity();
    }

private:
    struct DropOffCandidate final {
        ecs::Entity entity{};
        std::uint32_t teamId{};
        ResourceTypeId resourceType{};
        GridPoint point{};
    };

    static std::int32_t distance(GridPoint first, GridPoint second) noexcept {
        const auto dx = first.x > second.x ? first.x - second.x : second.x - first.x;
        const auto dy = first.y > second.y ? first.y - second.y : second.y - first.y;
        return dx + dy;
    }

    static std::int32_t eventAmount(ResourceAmount amount) noexcept {
        return static_cast<std::int32_t>(std::min<ResourceAmount>(
            std::numeric_limits<std::int32_t>::max(),
            std::max<ResourceAmount>(0, amount)));
    }

    static bool at(GridPoint first, GridPoint second) noexcept {
        return first.x == second.x && first.y == second.y;
    }

    void rebuildDropOffs(const ecs::World& world) {
        dropOffs_.clear();
        world.eachRef<Team, ResourceDropOff>(
            [&](ecs::Entity entity,
                const Team& team,
                const ResourceDropOff& dropOff) {
                if (team.id == 0 || dropOff.resourceType == 0) return;
                dropOffs_.push_back(
                    {entity,
                     team.id,
                     dropOff.resourceType,
                     {dropOff.accessX, dropOff.accessY}});
            });
        std::sort(
            dropOffs_.begin(), dropOffs_.end(),
            [](const DropOffCandidate& first,
               const DropOffCandidate& second) {
                return first.entity < second.entity;
            });
    }

    const DropOffCandidate* nearestDropOff(
        std::uint32_t teamId,
        ResourceTypeId resourceType,
        GridPoint origin) const noexcept {
        const DropOffCandidate* best = nullptr;
        auto bestDistance = std::numeric_limits<std::int32_t>::max();
        for (const auto& candidate : dropOffs_) {
            if (candidate.teamId != teamId ||
                candidate.resourceType != resourceType) {
                continue;
            }
            const auto candidateDistance = distance(origin, candidate.point);
            if (!best || candidateDistance < bestDistance ||
                (candidateDistance == bestDistance &&
                 candidate.entity < best->entity)) {
                best = &candidate;
                bestDistance = candidateDistance;
            }
        }
        return best;
    }

    static void ensureMove(
        OrderQueue& queue,
        MovementAgent& agent,
        GridPoint target) {
        const bool alreadyTargeting =
            (!queue.pending.empty() && queue.pending.front().target == target) ||
            (agent.hasPathGoal && agent.pathGoal == target);
        if (alreadyTargeting) return;
        HarvestCommandSystem::setMove(queue, agent, target);
    }

    static void clearMovement(OrderQueue& queue, MovementAgent& agent) {
        HarvestCommandSystem::clearMovement(queue, agent);
    }

    void advanceWorker(
        ecs::World& world,
        const ecs::SystemContext& context,
        TeamEconomyRuntime& economy,
        std::vector<DomainEvent>& events,
        ecs::Entity entity,
        Position& position,
        Team& team,
        OrderQueue& queue,
        MovementAgent& agent,
        WorkerHarvester& worker) {
        if (worker.state == HarvestState::Idle) return;

        auto* node = world.try_get<ResourceNode>(worker.targetNode);
        const auto* nodePosition = world.try_get<Position>(worker.targetNode);
        const bool validNode = world.alive(worker.targetNode) && node &&
                               nodePosition && node->remaining > 0;
        const GridPoint workerPoint{position.x, position.y};

        if (worker.state == HarvestState::MovingToNode) {
            if (!validNode) {
                worker.targetNode = {};
                worker.state = worker.cargo > 0
                    ? HarvestState::MovingToDropOff
                    : HarvestState::Idle;
                clearMovement(queue, agent);
                return;
            }
            const GridPoint nodePoint{nodePosition->x, nodePosition->y};
            if (!at(workerPoint, nodePoint)) {
                ensureMove(queue, agent, nodePoint);
                return;
            }
            clearMovement(queue, agent);
            worker.progressTicks = 0;
            worker.state = HarvestState::Harvesting;
        }

        if (worker.state == HarvestState::Harvesting) {
            if (!validNode) {
                worker.targetNode = {};
                worker.state = worker.cargo > 0
                    ? HarvestState::MovingToDropOff
                    : HarvestState::Idle;
                return;
            }
            const GridPoint nodePoint{nodePosition->x, nodePosition->y};
            if (!at(workerPoint, nodePoint)) {
                worker.state = HarvestState::MovingToNode;
                ensureMove(queue, agent, nodePoint);
                return;
            }
            if (++worker.progressTicks <
                std::max<std::uint32_t>(1, worker.harvestTicks)) {
                return;
            }
            worker.progressTicks = 0;
            const auto freeCargo = std::max<ResourceAmount>(
                0, worker.cargoCapacity - worker.cargo);
            const auto harvested = std::min(
                {std::max<ResourceAmount>(0, worker.harvestAmount),
                 node->remaining,
                 freeCargo});
            if (harvested > 0) {
                node->remaining -= harvested;
                worker.cargo += harvested;
                worker.cargoType = node->resourceType;
                events.push_back(
                    {context.tick,
                     DomainEventType::ResourceHarvested,
                     entity,
                     node->id,
                     0,
                     worker.targetNode,
                     eventAmount(harvested)});
                if (node->remaining == 0) {
                    events.push_back(
                        {context.tick,
                         DomainEventType::ResourceDepleted,
                         worker.targetNode,
                         node->id,
                         0,
                         entity,
                         0});
                }
            }
            if (worker.cargo >= worker.cargoCapacity ||
                node->remaining == 0 || harvested == 0) {
                worker.state = worker.cargo > 0
                    ? HarvestState::MovingToDropOff
                    : HarvestState::Idle;
            }
        }

        if (worker.state != HarvestState::MovingToDropOff) return;
        if (worker.cargo <= 0 || worker.cargoType == 0) {
            worker.cargo = 0;
            worker.cargoType = 0;
            worker.state = validNode
                ? HarvestState::MovingToNode
                : HarvestState::Idle;
            if (validNode) {
                ensureMove(
                    queue,
                    agent,
                    {nodePosition->x, nodePosition->y});
            }
            return;
        }

        const auto* dropOff = nearestDropOff(
            team.id, worker.cargoType, workerPoint);
        if (!dropOff) {
            clearMovement(queue, agent);
            return;
        }
        if (!at(workerPoint, dropOff->point)) {
            ensureMove(queue, agent, dropOff->point);
            return;
        }

        const auto deposited = worker.cargo;
        if (!economy.credit(team.id, worker.cargoType, deposited)) return;
        events.push_back(
            {context.tick,
             DomainEventType::ResourceDeposited,
             entity,
             worker.cargoType,
             0,
             dropOff->entity,
             eventAmount(deposited)});
        worker.cargo = 0;
        worker.cargoType = 0;
        clearMovement(queue, agent);
        if (validNode) {
            worker.state = HarvestState::MovingToNode;
            ensureMove(
                queue,
                agent,
                {nodePosition->x, nodePosition->y});
        } else {
            worker.targetNode = {};
            worker.state = HarvestState::Idle;
        }
    }

    std::vector<DropOffCandidate> dropOffs_;
};

} // namespace rts::gameplay
