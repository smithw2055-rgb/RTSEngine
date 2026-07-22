#pragma once

#include <RTSEngine/Ecs/Scheduler.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <vector>

namespace rts::gameplay {

struct OrderSystemDependencies {
    std::vector<DomainEvent>& events;
};

class OrderSystem final {
public:
    static bool handles(CommandType type) noexcept {
        return type == CommandType::Move ||
               type == CommandType::Stop ||
               type == CommandType::Attack ||
               type == CommandType::AttackMove ||
               type == CommandType::HoldPosition;
    }

    static void process(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        OrderSystemDependencies dependencies) {
        switch (command.type) {
        case CommandType::Move:
        case CommandType::Stop:
            processMoveOrStop(world, context, command, dependencies.events);
            break;
        case CommandType::Attack:
            processAttack(world, context, command, dependencies.events);
            break;
        case CommandType::AttackMove:
            processAttackMove(world, context, command, dependencies.events);
            break;
        case CommandType::HoldPosition:
            processHoldPosition(world, context, command, dependencies.events);
            break;
        default:
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

    static void applyFrontOrderMode(
        ecs::World& world,
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

private:
    static void processMoveOrStop(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        std::vector<DomainEvent>& events) {
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
            events.push_back(
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
        events.push_back(
            {context.tick,
             DomainEventType::MoveAccepted,
             command.subject,
             0,
             0});
    }

    static void processAttackMove(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        std::vector<DomainEvent>& events) {
        if (!world.alive(command.subject)) return;
        auto* queue = world.try_get<OrderQueue>(command.subject);
        auto* agent = world.try_get<MovementAgent>(command.subject);
        auto* directive =
            world.try_get<CombatDirective>(command.subject);
        auto* target = world.try_get<CombatTarget>(command.subject);
        const auto* weapon = world.try_get<Weapon>(command.subject);
        if (!queue || !agent || !directive || !target || !weapon) {
            events.push_back(
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
            {OrderType::AttackMove,
             {command.targetX, command.targetY}});
        directive->forcedTarget = {};
        directive->mode = queue->pending.front().type ==
                                  OrderType::AttackMove
            ? CombatMode::AttackMove
            : CombatMode::PassiveMove;
        target->entity = {};
        events.push_back(
            {context.tick,
             DomainEventType::AttackMoveAccepted,
             command.subject,
             0,
             0});
    }

    static void processAttack(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        std::vector<DomainEvent>& events) {
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
            events.push_back(
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
        events.push_back(
            {context.tick,
             DomainEventType::AttackAccepted,
             command.subject,
             0,
             0,
             command.targetEntity,
             0});
    }

    static void processHoldPosition(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        std::vector<DomainEvent>& events) {
        auto* queue = world.try_get<OrderQueue>(command.subject);
        auto* agent = world.try_get<MovementAgent>(command.subject);
        auto* directive =
            world.try_get<CombatDirective>(command.subject);
        auto* target = world.try_get<CombatTarget>(command.subject);
        if (!world.alive(command.subject) || !queue || !agent ||
            !directive || !target) {
            events.push_back(
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
        events.push_back(
            {context.tick,
             DomainEventType::HoldPositionAccepted,
             command.subject,
             0,
             0});
    }
};

} // namespace rts::gameplay
