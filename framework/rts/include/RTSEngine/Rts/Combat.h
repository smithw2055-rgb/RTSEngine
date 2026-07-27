#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/SpatialIndex.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

struct Team {
    std::uint32_t id{};
};

struct Health {
    std::int32_t current{};
    std::int32_t maximum{};
};

struct Armor {
    std::int32_t value{};
};

struct Weapon {
    std::int32_t damage{1};
    std::int32_t range{1};
    std::uint32_t cooldownTicks{1};
    std::uint32_t cooldownRemaining{};
};

struct CombatTarget {
    ecs::Entity entity{};
};

enum class CombatMode : std::uint8_t {
    Guard,
    PassiveMove,
    AttackMove,
    AttackTarget,
    HoldPosition
};

struct CombatDirective {
    CombatMode mode{CombatMode::Guard};
    ecs::Entity forcedTarget{};
};

struct Bounty {
    std::int32_t amount{};
};

struct CombatStats {
    std::int32_t maximumHealth{};
    std::int32_t armor{};
    std::int32_t weaponDamage{};
    std::int32_t weaponRange{};
    std::uint32_t cooldownTicks{1};
    std::int32_t bounty{};

    bool attackCapable() const noexcept {
        return maximumHealth > 0 && weaponDamage > 0 && weaponRange >= 0;
    }
};

struct DamageRequest {
    ecs::Entity source{};
    ecs::Entity target{};
    std::int32_t amount{};
    std::uint32_t sequence{};
};

enum class CombatEventType : std::uint8_t {
    TargetAcquired,
    WeaponFired,
    DamageApplied,
    EntityDied
};

struct CombatEvent {
    std::uint64_t tick{};
    CombatEventType type{};
    ecs::Entity source{};
    ecs::Entity target{};
    std::int32_t value{};
};

class CombatRuntime {
public:
    using DeathCallback =
        std::function<void(ecs::Entity victim, ecs::Entity killer)>;
    using VisibilityCallback = bool(*)(
        const void* context,
        std::uint32_t observerTeam,
        ecs::Entity target,
        std::int32_t targetX,
        std::int32_t targetY);

    CombatRuntime(
        std::int32_t worldWidth = 64,
        std::int32_t worldHeight = 64)
        : spatial_(worldWidth, worldHeight) {}

    void reserveCombatants(std::size_t count) {
        damage_.reserve(count);
        deaths_.reserve(count);
        events_.reserve(count * 2u);
    }

    void setVisibilityFilter(
        const void* context,
        VisibilityCallback callback) noexcept {
        visibilityContext_ = context;
        visibilityCallback_ = callback;
    }

    void clearVisibilityFilter() noexcept {
        visibilityContext_ = nullptr;
        visibilityCallback_ = nullptr;
    }

    template<class Position>
    void advance(
        const ecs::SystemContext& context,
        ecs::EntityCommandBuffer& commands,
        ecs::World& world,
        DeathCallback onDeath = {}) {
        events_.clear();
        damage_.clear();
        deaths_.clear();
        rebuildSpatial<Position>(world);
        acquireTargets<Position>(context, world);
        fireWeapons<Position>(context, world);
        resolveDamage(context, world);
        cleanupDead(context, commands, world, std::move(onDeath));
    }

    const std::vector<CombatEvent>& events() const noexcept {
        return events_;
    }

    const std::vector<DamageRequest>& pendingDamage() const noexcept {
        return damage_;
    }

    const FixedGridSpatialIndex& spatialIndex() const noexcept {
        return spatial_;
    }

private:
    struct DeathRecord {
        ecs::Entity victim{};
        ecs::Entity killer{};
    };

    static std::int32_t distance(
        std::int32_t ax,
        std::int32_t ay,
        std::int32_t bx,
        std::int32_t by) noexcept {
        const auto dx = ax > bx ? ax - bx : bx - ax;
        const auto dy = ay > by ? ay - by : by - ay;
        return dx + dy;
    }

    bool visible(
        std::uint32_t observerTeam,
        ecs::Entity target,
        std::int32_t x,
        std::int32_t y) const noexcept {
        return !visibilityCallback_ ||
               visibilityCallback_(
                   visibilityContext_, observerTeam, target, x, y);
    }

    template<class Position>
    bool validVisibleEnemy(
        const ecs::World& world,
        ecs::Entity entity,
        std::uint32_t teamId) const {
        if (!world.alive(entity)) return false;
        const auto* position = world.try_get<Position>(entity);
        const auto* team = world.try_get<Team>(entity);
        const auto* health = world.try_get<Health>(entity);
        return position && team && health && health->current > 0 &&
               team->id != teamId &&
               visible(teamId, entity, position->x, position->y);
    }

    template<class Position>
    void rebuildSpatial(const ecs::World& world) {
        spatial_.clear();
        world.each<Position, Team, Health>(
            [&](ecs::Entity entity) {
                const auto* position = world.try_get<Position>(entity);
                const auto* health = world.try_get<Health>(entity);
                if (!position || !health || health->current <= 0) return;
                spatial_.insert(entity, position->x, position->y);
            });
        spatial_.finalize();
    }

    template<class Position>
    void acquireTargets(
        const ecs::SystemContext& context,
        ecs::World& world) {
        world.each<Position, Team, Health, Weapon, CombatTarget>(
            [&](ecs::Entity entity) {
                const auto* position = world.try_get<Position>(entity);
                const auto* team = world.try_get<Team>(entity);
                const auto* health = world.try_get<Health>(entity);
                const auto* weapon = world.try_get<Weapon>(entity);
                auto* target = world.try_get<CombatTarget>(entity);
                auto* directive = world.try_get<CombatDirective>(entity);
                if (!position || !team || !health || !weapon || !target ||
                    health->current <= 0) {
                    return;
                }

                if (directive &&
                    directive->mode == CombatMode::PassiveMove) {
                    target->entity = {};
                    directive->forcedTarget = {};
                    return;
                }

                if (directive &&
                    directive->mode == CombatMode::AttackTarget) {
                    if (validVisibleEnemy<Position>(
                            world, directive->forcedTarget, team->id)) {
                        if (target->entity != directive->forcedTarget) {
                            target->entity = directive->forcedTarget;
                            events_.push_back(
                                {context.tick,
                                 CombatEventType::TargetAcquired,
                                 entity,
                                 target->entity,
                                 0});
                        }
                        return;
                    }
                    directive->forcedTarget = {};
                    directive->mode = CombatMode::Guard;
                    target->entity = {};
                }

                bool keep = false;
                if (validVisibleEnemy<Position>(
                        world, target->entity, team->id)) {
                    const auto* targetPosition =
                        world.try_get<Position>(target->entity);
                    keep = targetPosition &&
                           distance(
                               position->x,
                               position->y,
                               targetPosition->x,
                               targetPosition->y) <= weapon->range;
                }
                if (keep) return;

                target->entity = {};
                ecs::Entity best{};
                std::int32_t bestDistance =
                    std::numeric_limits<std::int32_t>::max();
                spatial_.visitManhattan(
                    position->x,
                    position->y,
                    weapon->range,
                    [&](const SpatialIndexEntry& candidateEntry) {
                        const auto candidate = candidateEntry.entity;
                        if (candidate == entity ||
                            !validVisibleEnemy<Position>(
                                world, candidate, team->id)) {
                            return;
                        }
                        const auto candidateDistance = distance(
                            position->x,
                            position->y,
                            candidateEntry.x,
                            candidateEntry.y);
                        if (candidateDistance < bestDistance ||
                            (candidateDistance == bestDistance &&
                             (!best.valid() || candidate < best))) {
                            best = candidate;
                            bestDistance = candidateDistance;
                        }
                    });
                if (best.valid()) {
                    target->entity = best;
                    events_.push_back(
                        {context.tick,
                         CombatEventType::TargetAcquired,
                         entity,
                         best,
                         0});
                }
            });
    }

    template<class Position>
    void fireWeapons(
        const ecs::SystemContext& context,
        ecs::World& world) {
        std::uint32_t sequence = 0;
        world.each<Position, Team, Health, Weapon, CombatTarget>(
            [&](ecs::Entity entity) {
                const auto* position = world.try_get<Position>(entity);
                const auto* team = world.try_get<Team>(entity);
                const auto* health = world.try_get<Health>(entity);
                auto* weapon = world.try_get<Weapon>(entity);
                auto* target = world.try_get<CombatTarget>(entity);
                const auto* directive =
                    world.try_get<CombatDirective>(entity);
                if (!position || !team || !health || !weapon || !target ||
                    health->current <= 0) {
                    return;
                }
                if (weapon->cooldownRemaining > 0) {
                    --weapon->cooldownRemaining;
                }
                if (weapon->cooldownRemaining > 0 ||
                    !world.alive(target->entity)) {
                    return;
                }

                const auto* targetPosition =
                    world.try_get<Position>(target->entity);
                const auto* targetHealth =
                    world.try_get<Health>(target->entity);
                const bool forced =
                    directive &&
                    directive->mode == CombatMode::AttackTarget &&
                    directive->forcedTarget == target->entity;
                if (!targetPosition || !targetHealth ||
                    targetHealth->current <= 0 ||
                    !visible(
                        team->id,
                        target->entity,
                        targetPosition->x,
                        targetPosition->y) ||
                    distance(
                        position->x,
                        position->y,
                        targetPosition->x,
                        targetPosition->y) > weapon->range) {
                    if (!forced) target->entity = {};
                    return;
                }

                damage_.push_back(
                    {entity,
                     target->entity,
                     std::max<std::int32_t>(0, weapon->damage),
                     sequence++});
                weapon->cooldownRemaining =
                    std::max<std::uint32_t>(1, weapon->cooldownTicks);
                events_.push_back(
                    {context.tick,
                     CombatEventType::WeaponFired,
                     entity,
                     target->entity,
                     weapon->damage});
            });
    }

    void resolveDamage(
        const ecs::SystemContext& context,
        ecs::World& world) {
        std::stable_sort(
            damage_.begin(),
            damage_.end(),
            [](const DamageRequest& a, const DamageRequest& b) {
                if (a.target != b.target) return a.target < b.target;
                if (a.source != b.source) return a.source < b.source;
                return a.sequence < b.sequence;
            });

        for (const auto& request : damage_) {
            auto* health = world.try_get<Health>(request.target);
            const auto* armor = world.try_get<Armor>(request.target);
            if (!health || health->current <= 0) continue;
            const auto mitigated = std::max<std::int32_t>(
                0, request.amount - (armor ? armor->value : 0));
            health->current =
                std::max<std::int32_t>(0, health->current - mitigated);
            events_.push_back(
                {context.tick,
                 CombatEventType::DamageApplied,
                 request.source,
                 request.target,
                 mitigated});
            if (health->current == 0) {
                deaths_.push_back({request.target, request.source});
            }
        }
    }

    void cleanupDead(
        const ecs::SystemContext& context,
        ecs::EntityCommandBuffer& commands,
        ecs::World& world,
        DeathCallback onDeath) {
        std::sort(
            deaths_.begin(), deaths_.end(),
            [](const DeathRecord& a, const DeathRecord& b) {
                if (a.victim != b.victim) return a.victim < b.victim;
                return a.killer < b.killer;
            });

        world.each<Health>(
            [&](ecs::Entity entity) {
                const auto* health = world.try_get<Health>(entity);
                if (!health || health->current > 0) return;

                ecs::Entity killer{};
                const auto record = std::lower_bound(
                    deaths_.begin(), deaths_.end(), entity,
                    [](const DeathRecord& value, ecs::Entity victim) {
                        return value.victim < victim;
                    });
                if (record != deaths_.end() && record->victim == entity) {
                    killer = record->killer;
                }
                if (onDeath) onDeath(entity, killer);
                commands.destroy(context, entity);
                events_.push_back(
                    {context.tick,
                     CombatEventType::EntityDied,
                     killer,
                     entity,
                     0});
            });
    }

    FixedGridSpatialIndex spatial_;
    std::vector<DamageRequest> damage_;
    std::vector<DeathRecord> deaths_;
    std::vector<CombatEvent> events_;
    const void* visibilityContext_{};
    VisibilityCallback visibilityCallback_{};
};

} // namespace rts::gameplay
