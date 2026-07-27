#pragma once

#include <RTSEngine/Ecs/Scheduler.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <RTSEngine/Rts/Vision.h>

#include <vector>

namespace rts::gameplay {

struct OrderSystemDependencies {
    std::vector<DomainEvent>& events;
    const VisionRuntime* vision{};
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
            processMoveOrStop(world, context, command, dependencies);
            break;
        case CommandType::Attack:
            processAttack(world, context, command, dependencies);
            break;
        case CommandType::AttackMove:
            processAttackMove(world, context, command, dependencies);
            break;
        case CommandType::HoldPosition:
            processHoldPosition(world, context, command, dependencies);
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
    static constexpr std::uint32_t kInternalIssuerMask = 0x80000000u;

    static bool isInternalIssuer(std::uint32_t issuer) noexcept {
        return (issuer & kInternalIssuerMask) != 0;
    }

    static bool owns(const ecs::World& world,
                     ecs::Entity entity,
                     std::uint32_t issuer) noexcept {
        const auto* team = world.try_get<Team>(entity);
        return team &&
               (team->id == issuer || isInternalIssuer(issuer));
    }

    static void reject(const ecs::SystemContext& context,
                       const TickCommand& command,
                       CommandRejectionReason reason,
                       std::vector<DomainEvent>& events) {
        events.push_back(
            {context.tick,
             DomainEventType::CommandRejected,
             command.subject,
             command.objectId,
             static_cast<std::uint32_t>(reason),
             command.targetEntity,
             static_cast<std::int32_t>(command.type)});
    }

    static void processMoveOrStop(
        ecs::World& world,
        const ecs::SystemContext& context,
        const TickCommand& command,
        OrderSystemDependencies dependencies) {
        if (!world.alive(command.subject)) {
            reject(context, command, CommandRejectionReason::InvalidEntity,
                   dependencies.events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(context, command, CommandRejectionReason::NotOwner,
                   dependencies.events);
            return;
        }

        auto* queue = world.try_get<OrderQueue>(command.subject);
        auto* agent = world.try_get<MovementAgent>(command.subject);
        if (!queue || !agent) {
            reject(context, command, CommandRejectionReason::MissingCapability,
                   dependencies.events);
            return;
        }

        if (command.type == CommandType::Stop) {
            queue->pending.clear();
            clearPath(*agent);
            clearTarget(world, command.subject);
            if (auto* directive =
                    world.try_get<CombatDirective>(command.subject)) {
                directive->mode = CombatMode::Guard;
                directive->forcedTarget = {};
            }
            dependencies.events.push_back(
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
        dependencies.events.push_back(
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
        OrderSystemDependencies dependencies) {
        if (!world.alive(command.subject)) {
            reject(context, command, CommandRejectionReason::InvalidEntity,
                   dependencies.events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(context, command, CommandRejectionReason::NotOwner,
                   dependencies.events);
            return;
        }

        auto* queue = world.try_get<OrderQueue>(command.subject);
        auto* agent = world.try_get<MovementAgent>(command.subject);
        auto* directive =
            world.try_get<CombatDirective>(command.subject);
        auto* target = world.try_get<CombatTarget>(command.subject);
        const auto* weapon = world.try_get<Weapon>(command.subject);
        if (!queue || !agent || !directive || !target || !weapon) {
            reject(context, command, CommandRejectionReason::MissingCapability,
                   dependencies.events);
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
        dependencies.events.push_back(
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
        OrderSystemDependencies dependencies) {
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

        if (!world.alive(command.subject) || !attackerTeam) {
            reject(context, command, CommandRejectionReason::InvalidEntity,
                   dependencies.events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(context, command, CommandRejectionReason::NotOwner,
                   dependencies.events);
            return;
        }
        if (!queue || !agent || !directive || !target || !weapon) {
            reject(context, command, CommandRejectionReason::MissingCapability,
                   dependencies.events);
            return;
        }

        const bool validTarget =
            world.alive(command.targetEntity) && targetTeam && targetHealth &&
            targetPosition && targetHealth->current > 0 &&
            targetTeam->id != attackerTeam->id;
        if (!validTarget) {
            reject(context, command, CommandRejectionReason::InvalidTarget,
                   dependencies.events);
            return;
        }

        if (dependencies.vision && dependencies.vision->layerCount() != 0 &&
            !dependencies.vision->visible(
                attackerTeam->id,
                {targetPosition->x, targetPosition->y})) {
            reject(context, command,
                   CommandRejectionReason::TargetNotVisible,
                   dependencies.events);
            return;
        }

        queue->pending.clear();
        clearPath(*agent);
        directive->mode = CombatMode::AttackTarget;
        directive->forcedTarget = command.targetEntity;
        target->entity = command.targetEntity;
        dependencies.events.push_back(
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
        OrderSystemDependencies dependencies) {
        if (!world.alive(command.subject)) {
            reject(context, command, CommandRejectionReason::InvalidEntity,
                   dependencies.events);
            return;
        }
        if (!owns(world, command.subject, command.issuer)) {
            reject(context, command, CommandRejectionReason::NotOwner,
                   dependencies.events);
            return;
        }

        auto* queue = world.try_get<OrderQueue>(command.subject);
        auto* agent = world.try_get<MovementAgent>(command.subject);
        auto* directive =
            world.try_get<CombatDirective>(command.subject);
        auto* target = world.try_get<CombatTarget>(command.subject);
        if (!queue || !agent || !directive || !target) {
            reject(context, command, CommandRejectionReason::MissingCapability,
                   dependencies.events);
            return;
        }

        queue->pending.clear();
        clearPath(*agent);
        directive->mode = CombatMode::HoldPosition;
        directive->forcedTarget = {};
        target->entity = {};
        dependencies.events.push_back(
            {context.tick,
             DomainEventType::HoldPositionAccepted,
             command.subject,
             0,
             0});
    }
};

} // namespace rts::gameplay
