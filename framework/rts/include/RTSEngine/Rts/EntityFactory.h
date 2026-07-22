#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/GameplayModifierSystem.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <algorithm>
#include <cstdint>

namespace rts::gameplay {

class EntityFactory final {
public:
    static ecs::Entity createUnit(
        ecs::World& world,
        const GameplayModifierSystem& modifiers,
        Position position,
        MoveSpeed speed,
        std::uint32_t teamId,
        const CombatStats& combat) {
        const auto entity = world.create();
        world.emplace<Position>(entity, position);
        world.emplace<MoveSpeed>(entity, speed);
        world.emplace<OrderQueue>(entity, OrderQueue{});
        world.emplace<MovementAgent>(entity, MovementAgent{});
        world.emplace<Team>(entity, Team{teamId});
        attachCombatProfile(
            world, entity, combat, false, speed.cellsPerTick);
        modifiers.applyEntity<MoveSpeed>(world, entity);
        return entity;
    }

    template<class Target>
    static void queueUnit(
        const ecs::SystemContext& context,
        ecs::EntityCommandBuffer& commands,
        const GameplayModifierSystem& modifiers,
        Target target,
        Position position,
        std::int32_t baseMoveSpeed,
        std::uint32_t teamId,
        const CombatStats& combat) {
        const auto resolvedSpeed = std::max<std::int32_t>(
            0,
            ScaleGameplayValue(
                baseMoveSpeed,
                modifiers.profile(teamId).unitMoveSpeed));
        commands.add(context, target, position);
        commands.add(context, target, MoveSpeed{resolvedSpeed});
        commands.add(context, target, OrderQueue{});
        commands.add(context, target, MovementAgent{});
        commands.add(context, target, Team{teamId});
        queueCombatProfile(
            context,
            commands,
            modifiers,
            target,
            combat,
            false,
            baseMoveSpeed,
            teamId);
    }

    static void attachCombatProfile(
        ecs::World& world,
        ecs::Entity entity,
        const CombatStats& profile,
        bool building,
        std::int32_t baseMoveSpeed) {
        world.emplace<TunableStats>(
            entity, TunableStats{building, baseMoveSpeed, profile});
        if (profile.maximumHealth <= 0) return;

        world.emplace<Health>(
            entity,
            Health{profile.maximumHealth, profile.maximumHealth});
        world.emplace<Armor>(
            entity, Armor{std::max<std::int32_t>(0, profile.armor)});
        if (profile.bounty > 0) {
            world.emplace<Bounty>(entity, Bounty{profile.bounty});
        }
        if (profile.attackCapable()) {
            world.emplace<Weapon>(
                entity,
                Weapon{
                    profile.weaponDamage,
                    profile.weaponRange,
                    std::max<std::uint32_t>(1, profile.cooldownTicks),
                    0});
            world.emplace<CombatTarget>(entity, CombatTarget{});
            world.emplace<CombatDirective>(entity, CombatDirective{});
        }
    }

    template<class Target>
    static void queueCombatProfile(
        const ecs::SystemContext& context,
        ecs::EntityCommandBuffer& commands,
        const GameplayModifierSystem& modifiers,
        Target target,
        const CombatStats& profile,
        bool building,
        std::int32_t baseMoveSpeed,
        std::uint32_t teamId) {
        commands.add(
            context,
            target,
            TunableStats{building, baseMoveSpeed, profile});
        if (profile.maximumHealth <= 0) return;

        const auto& teamProfile = modifiers.profile(teamId);
        const auto healthMultiplier = building
            ? teamProfile.buildingHealth
            : teamProfile.unitHealth;
        const auto damageMultiplier = building
            ? teamProfile.buildingDamage
            : teamProfile.unitDamage;
        const auto maximumHealth = std::max<std::int32_t>(
            1,
            ScaleGameplayValue(profile.maximumHealth, healthMultiplier));
        const auto armor = std::max<std::int32_t>(
            0,
            profile.armor + (building ? 0 : teamProfile.unitArmorAdd));

        commands.add(
            context, target, Health{maximumHealth, maximumHealth});
        commands.add(context, target, Armor{armor});
        if (profile.bounty > 0) {
            commands.add(context, target, Bounty{profile.bounty});
        }
        if (profile.attackCapable()) {
            commands.add(
                context,
                target,
                Weapon{
                    std::max<std::int32_t>(
                        0,
                        ScaleGameplayValue(
                            profile.weaponDamage, damageMultiplier)),
                    std::max<std::int32_t>(0, profile.weaponRange),
                    std::max<std::uint32_t>(1, profile.cooldownTicks),
                    0});
            commands.add(context, target, CombatTarget{});
            commands.add(context, target, CombatDirective{});
        }
    }
};

} // namespace rts::gameplay
