#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/World.h>

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

class SpatialIndex {
public:
    SpatialIndex(std::int32_t width = 64, std::int32_t height = 64, std::int32_t cellSize = 4)
        : width_(std::max<std::int32_t>(1, width)),
          height_(std::max<std::int32_t>(1, height)),
          cellSize_(std::max<std::int32_t>(1, cellSize)),
          columns_((width_ + cellSize_ - 1) / cellSize_),
          rows_((height_ + cellSize_ - 1) / cellSize_),
          buckets_(static_cast<std::size_t>(columns_ * rows_)) {}

    void clear() {
        for (auto& bucket : buckets_) bucket.clear();
    }

    template<class Position>
    void rebuild(const ecs::World& world) {
        clear();
        for (const auto entity : world.view<Position, Team, Health>()) {
            const auto* position = world.try_get<Position>(entity);
            const auto* health = world.try_get<Health>(entity);
            if (!position || !health || health->current <= 0) continue;
            const auto bucket = bucketIndex(position->x, position->y);
            if (bucket >= 0) buckets_[static_cast<std::size_t>(bucket)].push_back(entity);
        }
        for (auto& bucket : buckets_) std::sort(bucket.begin(), bucket.end());
    }

    template<class Position>
    std::vector<ecs::Entity> query(const ecs::World& world,
                                   std::int32_t x,
                                   std::int32_t y,
                                   std::int32_t range) const {
        std::vector<ecs::Entity> result;
        const auto minX = std::max<std::int32_t>(0, (x - range) / cellSize_);
        const auto maxX = std::min<std::int32_t>(columns_ - 1, (x + range) / cellSize_);
        const auto minY = std::max<std::int32_t>(0, (y - range) / cellSize_);
        const auto maxY = std::min<std::int32_t>(rows_ - 1, (y + range) / cellSize_);
        for (std::int32_t by = minY; by <= maxY; ++by) {
            for (std::int32_t bx = minX; bx <= maxX; ++bx) {
                const auto& bucket = buckets_[static_cast<std::size_t>(by * columns_ + bx)];
                for (const auto entity : bucket) {
                    const auto* position = world.try_get<Position>(entity);
                    if (!position) continue;
                    const auto entityDistance = abs(position->x - x) + abs(position->y - y);
                    if (entityDistance <= range) result.push_back(entity);
                }
            }
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

private:
    static std::int32_t abs(std::int32_t value) noexcept { return value < 0 ? -value : value; }

    std::int32_t bucketIndex(std::int32_t x, std::int32_t y) const noexcept {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return -1;
        return (y / cellSize_) * columns_ + (x / cellSize_);
    }

    std::int32_t width_{};
    std::int32_t height_{};
    std::int32_t cellSize_{};
    std::int32_t columns_{};
    std::int32_t rows_{};
    std::vector<std::vector<ecs::Entity>> buckets_;
};

class CombatRuntime {
public:
    using DeathCallback = std::function<void(ecs::Entity)>;

    CombatRuntime(std::int32_t worldWidth = 64, std::int32_t worldHeight = 64)
        : spatial_(worldWidth, worldHeight) {}

    template<class Position>
    void advance(const ecs::SystemContext& context,
                 ecs::EntityCommandBuffer& commands,
                 ecs::World& world,
                 DeathCallback onDeath = {}) {
        events_.clear();
        damage_.clear();
        spatial_.template rebuild<Position>(world);
        acquireTargets<Position>(context, world);
        fireWeapons<Position>(context, world);
        resolveDamage(context, world);
        cleanupDead(context, commands, world, std::move(onDeath));
    }

    const std::vector<CombatEvent>& events() const noexcept { return events_; }
    const std::vector<DamageRequest>& pendingDamage() const noexcept { return damage_; }

private:
    static std::int32_t distance(std::int32_t ax, std::int32_t ay,
                                 std::int32_t bx, std::int32_t by) noexcept {
        const auto dx = ax > bx ? ax - bx : bx - ax;
        const auto dy = ay > by ? ay - by : by - ay;
        return dx + dy;
    }

    template<class Position>
    void acquireTargets(const ecs::SystemContext& context, ecs::World& world) {
        for (const auto entity : world.view<Position, Team, Health, Weapon, CombatTarget>()) {
            const auto* position = world.try_get<Position>(entity);
            const auto* team = world.try_get<Team>(entity);
            const auto* health = world.try_get<Health>(entity);
            const auto* weapon = world.try_get<Weapon>(entity);
            auto* target = world.try_get<CombatTarget>(entity);
            if (!position || !team || !health || !weapon || !target || health->current <= 0) continue;

            bool keep = false;
            if (world.alive(target->entity)) {
                const auto* targetPosition = world.try_get<Position>(target->entity);
                const auto* targetTeam = world.try_get<Team>(target->entity);
                const auto* targetHealth = world.try_get<Health>(target->entity);
                keep = targetPosition && targetTeam && targetHealth && targetHealth->current > 0 &&
                       targetTeam->id != team->id &&
                       distance(position->x, position->y, targetPosition->x, targetPosition->y) <= weapon->range;
            }
            if (keep) continue;

            target->entity = {};
            ecs::Entity best{};
            std::int32_t bestDistance = std::numeric_limits<std::int32_t>::max();
            for (const auto candidate : spatial_.template query<Position>(world, position->x, position->y, weapon->range)) {
                if (candidate == entity) continue;
                const auto* candidateTeam = world.try_get<Team>(candidate);
                const auto* candidateHealth = world.try_get<Health>(candidate);
                const auto* candidatePosition = world.try_get<Position>(candidate);
                if (!candidateTeam || !candidateHealth || !candidatePosition ||
                    candidateTeam->id == team->id || candidateHealth->current <= 0) continue;
                const auto candidateDistance = distance(position->x, position->y,
                                                        candidatePosition->x, candidatePosition->y);
                if (candidateDistance < bestDistance ||
                    (candidateDistance == bestDistance && (!best.valid() || candidate < best))) {
                    best = candidate;
                    bestDistance = candidateDistance;
                }
            }
            if (best.valid()) {
                target->entity = best;
                events_.push_back({context.tick, CombatEventType::TargetAcquired, entity, best, 0});
            }
        }
    }

    template<class Position>
    void fireWeapons(const ecs::SystemContext& context, ecs::World& world) {
        std::uint32_t sequence = 0;
        for (const auto entity : world.view<Position, Health, Weapon, CombatTarget>()) {
            const auto* position = world.try_get<Position>(entity);
            const auto* health = world.try_get<Health>(entity);
            auto* weapon = world.try_get<Weapon>(entity);
            auto* target = world.try_get<CombatTarget>(entity);
            if (!position || !health || !weapon || !target || health->current <= 0) continue;
            if (weapon->cooldownRemaining > 0) --weapon->cooldownRemaining;
            if (weapon->cooldownRemaining > 0 || !world.alive(target->entity)) continue;

            const auto* targetPosition = world.try_get<Position>(target->entity);
            const auto* targetHealth = world.try_get<Health>(target->entity);
            if (!targetPosition || !targetHealth || targetHealth->current <= 0 ||
                distance(position->x, position->y, targetPosition->x, targetPosition->y) > weapon->range) {
                target->entity = {};
                continue;
            }

            damage_.push_back({entity, target->entity, std::max<std::int32_t>(0, weapon->damage), sequence++});
            weapon->cooldownRemaining = std::max<std::uint32_t>(1, weapon->cooldownTicks);
            events_.push_back({context.tick, CombatEventType::WeaponFired,
                               entity, target->entity, weapon->damage});
        }
    }

    void resolveDamage(const ecs::SystemContext& context, ecs::World& world) {
        std::stable_sort(damage_.begin(), damage_.end(), [](const DamageRequest& a, const DamageRequest& b) {
            if (a.target != b.target) return a.target < b.target;
            if (a.source != b.source) return a.source < b.source;
            return a.sequence < b.sequence;
        });

        for (const auto& request : damage_) {
            auto* health = world.try_get<Health>(request.target);
            const auto* armor = world.try_get<Armor>(request.target);
            if (!health || health->current <= 0) continue;
            const auto mitigated = std::max<std::int32_t>(0, request.amount - (armor ? armor->value : 0));
            health->current = std::max<std::int32_t>(0, health->current - mitigated);
            events_.push_back({context.tick, CombatEventType::DamageApplied,
                               request.source, request.target, mitigated});
        }
    }

    void cleanupDead(const ecs::SystemContext& context,
                     ecs::EntityCommandBuffer& commands,
                     ecs::World& world,
                     DeathCallback onDeath) {
        for (const auto entity : world.view<Health>()) {
            const auto* health = world.try_get<Health>(entity);
            if (!health || health->current > 0) continue;
            if (onDeath) onDeath(entity);
            commands.destroy(context, entity);
            events_.push_back({context.tick, CombatEventType::EntityDied, {}, entity, 0});
        }
    }

    SpatialIndex spatial_;
    std::vector<DamageRequest> damage_;
    std::vector<CombatEvent> events_;
};

} // namespace rts::gameplay
