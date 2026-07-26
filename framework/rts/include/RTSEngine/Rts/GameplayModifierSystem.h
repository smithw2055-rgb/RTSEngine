#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/GameplayModifiers.h>

#include <algorithm>
#include <cstdint>

namespace rts::gameplay {

class GameplayModifierSystem final {
public:
    template<class MoveSpeedComponent>
    bool setProfile(ecs::World& world,
                    std::uint32_t teamId,
                    TeamModifierProfile profile) {
        const bool changed = table_.set(teamId, profile);
        applyTeam<MoveSpeedComponent>(world, teamId);
        retimeTeam(world, teamId);
        return changed;
    }

    const TeamModifierProfile& profile(std::uint32_t teamId) const noexcept {
        return table_.profile(teamId);
    }

    const std::vector<TeamModifierEntry>& entries() const noexcept {
        return table_.entries();
    }

    std::uint32_t constructionTicks(std::uint32_t teamId,
                                    std::uint32_t baseTicks) const noexcept {
        return ScaleGameplayDuration(
            baseTicks, profile(teamId).constructionSpeed);
    }

    std::uint32_t productionTicks(std::uint32_t teamId,
                                  std::uint32_t baseTicks) const noexcept {
        return ScaleGameplayDuration(
            baseTicks, profile(teamId).productionSpeed);
    }

    std::int32_t bounty(std::uint32_t teamId,
                        std::int32_t baseValue) const noexcept {
        return std::max<std::int32_t>(
            0, ScaleGameplayValue(baseValue,
                                  profile(teamId).bountyMultiplier));
    }

    template<class MoveSpeedComponent>
    void applyTeam(ecs::World& world, std::uint32_t teamId) const {
        for (const auto entity : world.view<Team, TunableStats>()) {
            const auto* team = world.try_get<Team>(entity);
            if (team && team->id == teamId) {
                applyEntity<MoveSpeedComponent>(world, entity);
            }
        }
    }

    template<class MoveSpeedComponent>
    void applyEntity(ecs::World& world, ecs::Entity entity) const {
        const auto* team = world.try_get<Team>(entity);
        const auto* base = world.try_get<TunableStats>(entity);
        if (!team || !base) return;

        const auto& modifiers = profile(team->id);
        const auto healthMultiplier = base->building
            ? modifiers.buildingHealth
            : modifiers.unitHealth;
        const auto damageMultiplier = base->building
            ? modifiers.buildingDamage
            : modifiers.unitDamage;

        if (auto* health = world.try_get<Health>(entity)) {
            const auto missing = std::max<std::int32_t>(
                0, health->maximum - health->current);
            const auto resolvedMaximum = std::max<std::int32_t>(
                1, ScaleGameplayValue(
                       base->baseCombat.maximumHealth,
                       healthMultiplier));
            health->maximum = resolvedMaximum;
            health->current = std::clamp(
                resolvedMaximum - missing, 0, resolvedMaximum);
        }

        if (auto* armor = world.try_get<Armor>(entity)) {
            const auto bonus = base->building ? 0 : modifiers.unitArmorAdd;
            armor->value = std::max<std::int32_t>(
                0, base->baseCombat.armor + bonus);
        }

        if (auto* weapon = world.try_get<Weapon>(entity)) {
            weapon->damage = std::max<std::int32_t>(
                0, ScaleGameplayValue(
                       base->baseCombat.weaponDamage,
                       damageMultiplier));
            weapon->range = std::max<std::int32_t>(
                0, base->baseCombat.weaponRange);
            weapon->cooldownTicks = std::max<std::uint32_t>(
                1, base->baseCombat.cooldownTicks);
            weapon->cooldownRemaining = std::min(
                weapon->cooldownRemaining, weapon->cooldownTicks);
        }

        if (auto* speed = world.try_get<MoveSpeedComponent>(entity)) {
            const auto multiplier = base->building
                ? kGameplayModifierScale
                : modifiers.unitMoveSpeed;
            speed->cellsPerTick = std::max<std::int32_t>(
                0, ScaleGameplayValue(base->baseMoveSpeed, multiplier));
        }

        if (auto* bountyValue = world.try_get<Bounty>(entity)) {
            bountyValue->amount = std::max<std::int32_t>(
                0, base->baseCombat.bounty);
        }
    }

    void retimeTeam(ecs::World& world, std::uint32_t teamId) const {
        for (const auto entity : world.view<ConstructionSite>()) {
            auto* site = world.try_get<ConstructionSite>(entity);
            if (!site || site->ownerTeam != teamId) continue;
            const auto base = site->baseRequiredTicks == 0
                ? site->requiredTicks
                : site->baseRequiredTicks;
            site->baseRequiredTicks = std::max<std::uint32_t>(1, base);
            site->requiredTicks = constructionTicks(
                teamId, site->baseRequiredTicks);
        }

        for (const auto entity : world.view<Team, ProductionQueue>()) {
            const auto* team = world.try_get<Team>(entity);
            auto* queue = world.try_get<ProductionQueue>(entity);
            if (!team || team->id != teamId || !queue) continue;
            for (auto& item : queue->items) {
                const auto base = item.baseRequiredTicks == 0
                    ? item.requiredTicks
                    : item.baseRequiredTicks;
                item.baseRequiredTicks = std::max<std::uint32_t>(1, base);
                item.requiredTicks = productionTicks(
                    teamId, item.baseRequiredTicks);
            }
        }
    }

    void appendHash(foundation::CanonicalHash& hash) const noexcept {
        table_.appendHash(hash);
    }

private:
    TeamModifierTable table_;
};

} // namespace rts::gameplay
