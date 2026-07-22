#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/OrderSystem.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <algorithm>
#include <vector>

namespace rts::gameplay {

struct MovementSystemDependencies {
    std::vector<DomainEvent>& events;
};

class MovementSystem final {
public:
    static void run(
        ecs::World& world,
        const ecs::SystemContext& context,
        MovementSystemDependencies dependencies) {
        for (const auto entity :
             world.view<Position, MoveSpeed, OrderQueue, MovementAgent>()) {
            auto* position = world.try_get<Position>(entity);
            const auto* speed = world.try_get<MoveSpeed>(entity);
            auto* queue = world.try_get<OrderQueue>(entity);
            auto* agent = world.try_get<MovementAgent>(entity);
            if (!position || !speed || !queue || !agent ||
                agent->path.empty()) {
                continue;
            }
            if (shouldPauseForCombat(world, entity, *position)) {
                continue;
            }

            const auto amount =
                std::max<std::int32_t>(1, speed->cellsPerTick);
            for (std::int32_t step = 0;
                 step < amount && agent->nextPoint < agent->path.size();
                 ++step) {
                const auto point = agent->path[agent->nextPoint++];
                position->x = point.x;
                position->y = point.y;
            }
            if (agent->nextPoint != agent->path.size()) {
                continue;
            }

            const bool completedCombatPath = agent->combatPath;
            OrderSystem::clearPath(*agent);
            if (completedCombatPath) continue;

            if (!queue->pending.empty()) {
                queue->pending.erase(queue->pending.begin());
            }
            OrderSystem::applyFrontOrderMode(world, entity, *queue);
            dependencies.events.push_back(
                {context.tick,
                 DomainEventType::MoveCompleted,
                 entity,
                 0,
                 0});
        }
    }

private:
    static std::int32_t distance(GridPoint a, GridPoint b) noexcept {
        const auto dx = a.x > b.x ? a.x - b.x : b.x - a.x;
        const auto dy = a.y > b.y ? a.y - b.y : b.y - a.y;
        return dx + dy;
    }

    static bool shouldPauseForCombat(
        const ecs::World& world,
        ecs::Entity entity,
        const Position& position) {
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
};

} // namespace rts::gameplay
