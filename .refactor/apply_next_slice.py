from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def write(relative: str, text: str) -> None:
    (ROOT / relative).write_text(text, encoding="utf-8")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


def replace_between(text: str, start: str, end: str, replacement: str,
                    label: str) -> str:
    start_index = text.find(start)
    if start_index < 0:
        raise RuntimeError(f"{label}: start marker not found")
    end_index = text.find(end, start_index)
    if end_index < 0:
        raise RuntimeError(f"{label}: end marker not found")
    return text[:start_index] + replacement + text[end_index:]


def refactor_rts_simulation() -> None:
    path = "framework/rts/include/RTSEngine/Rts/Simulation.h"
    text = read(path)
    marker = "class RtsSimulation {"
    class_index = text.find(marker)
    if class_index < 0:
        raise RuntimeError("RtsSimulation marker not found")

    prefix = """#pragma once

#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/Scheduler.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/Combat.h>
#include <RTSEngine/Rts/DefinitionCatalog.h>
#include <RTSEngine/Rts/GameplayModifierSystem.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rts::gameplay {

"""
    text = prefix + text[class_index:]

    text = replace_once(
        text,
        """    void registerBuilding(BuildingDefinition definition) {
        replaceDefinition(buildingDefinitions_, definition);
    }

    void registerUnit(UnitDefinition definition) {
        replaceDefinition(unitDefinitions_, definition);
    }
""",
        """    void registerBuilding(BuildingDefinition definition) {
        buildingDefinitions_.replace(std::move(definition));
    }

    void registerUnit(UnitDefinition definition) {
        unitDefinitions_.replace(std::move(definition));
    }
""",
        "definition registration")

    text = replace_once(
        text,
        """    void setPlayerTeam(std::uint32_t teamId) noexcept {
        playerTeamId_ = teamId;
    }
""",
        """    void setPlayerTeam(std::uint32_t teamId) noexcept {
        playerTeamId_ = teamId;
    }

    bool setTeamModifierProfile(std::uint32_t teamId,
                                TeamModifierProfile profile) {
        return modifiers_.setProfile<MoveSpeed>(
            world_, teamId, profile);
    }

    const TeamModifierProfile& teamModifierProfile(
        std::uint32_t teamId) const noexcept {
        return modifiers_.profile(teamId);
    }
""",
        "team modifier public API")

    create_start = "    ecs::Entity createUnit("
    create_end = "    bool submit(TickCommand command)"
    create_block = """    ecs::Entity createUnit(Position position,
                           MoveSpeed speed,
                           std::uint32_t teamId = 1,
                           CombatStats combat = {}) {
        const auto entity = world_.create();
        world_.emplace<Position>(entity, position);
        world_.emplace<MoveSpeed>(entity, speed);
        world_.emplace<OrderQueue>(entity, OrderQueue{});
        world_.emplace<MovementAgent>(entity, MovementAgent{});
        world_.emplace<Team>(entity, Team{teamId});
        attachCombatProfile(
            entity, combat, false, speed.cellsPerTick);
        modifiers_.applyEntity<MoveSpeed>(world_, entity);
        return entity;
    }

"""
    text = replace_between(
        text, create_start, create_end, create_block,
        "createUnit extraction")

    template_start = "    template<class Definition>\n    static void replaceDefinition"
    distance_marker = "    static std::int32_t distance"
    text = replace_between(
        text, template_start, distance_marker, "",
        "remove definition helpers")

    lookup_start = "    const BuildingDefinition* buildingDefinition"
    run_stage_marker = "    void runStage"
    lookup_block = """    const BuildingDefinition* buildingDefinition(
        std::uint32_t id) const noexcept {
        return buildingDefinitions_.find(id);
    }

    const UnitDefinition* unitDefinition(
        std::uint32_t id) const noexcept {
        return unitDefinitions_.find(id);
    }

"""
    text = replace_between(
        text, lookup_start, run_stage_marker, lookup_block,
        "definition lookup extraction")

    navigation_marker = """        scheduler_.add(ecs::Stage::Navigation, -10, 190,
"""
    navigation_system = """        scheduler_.add(ecs::Stage::Navigation, -20, 180,
                       [this](ecs::World& world,
                              const ecs::SystemContext&) {
                           synchronizeTeamModifiers(world);
                       });

"""
    text = replace_once(
        text, navigation_marker, navigation_system + navigation_marker,
        "modifier synchronization system")

    build_case = text.find("        case CommandType::Build: {")
    if build_case < 0:
        raise RuntimeError("build case not found")
    definition_start = text.find("            const auto* definition =", build_case)
    event_start = text.find("            events_.push_back(", definition_start)
    if definition_start < 0 or event_start < 0:
        raise RuntimeError("build definition block not found")
    replacement = """            const auto* definition =
                buildingDefinition(command.definitionId);
            BuildResult result;
            if (definition) {
                auto resolvedDefinition = *definition;
                resolvedDefinition.buildTicks =
                    modifiers_.constructionTicks(
                        command.issuer, definition->buildTicks);
                result = building_.begin(
                    context,
                    structuralCommands_,
                    resolvedDefinition,
                    {command.targetX, command.targetY},
                    requiredPathStart_,
                    requiredPathGoal_,
                    command.issuer,
                    definition->buildTicks);
            } else {
                result = {false, BuildFailure::InvalidDefinition, 0};
            }
"""
    text = text[:definition_start] + replacement + text[event_start:]

    production_start = text.find("    void beginProduction(")
    production_if = text.find("        if (!queue", production_start)
    queue_start = text.find("        auto* queue", production_start)
    if min(production_start, production_if, queue_start) < 0:
        raise RuntimeError("beginProduction prelude not found")
    production_prelude = """        auto* queue = world.try_get<ProductionQueue>(command.subject);
        const auto* building = world.try_get<Building>(command.subject);
        const auto* ownerTeam = world.try_get<Team>(command.subject);
        const auto* definition = unitDefinition(command.definitionId);
"""
    text = text[:queue_start] + production_prelude + text[production_if:]

    push_start = text.find("        queue->items.push_back(", production_start)
    push_end = text.find("        events_.push_back(", push_start)
    if push_start < 0 or push_end < 0:
        raise RuntimeError("production queue insertion not found")
    push_block = """        const auto baseTicks =
            std::max<std::uint32_t>(1, definition->trainTicks);
        const auto teamId = ownerTeam ? ownerTeam->id : 0;
        const auto requiredTicks =
            modifiers_.productionTicks(teamId, baseTicks);
        queue->items.push_back(
            {id,
             definition->id,
             definition->cost,
             0,
             requiredTicks,
             baseTicks});
"""
    text = text[:push_start] + push_block + text[push_end:]

    text = replace_once(
        text,
        """                MoveSpeed{definition->cellsPerTick});
""",
        """                MoveSpeed{std::max<std::int32_t>(
                    0,
                    ScaleGameplayValue(
                        definition->cellsPerTick,
                        modifiers_.profile(
                            ownerTeam ? ownerTeam->id : 0)
                            .unitMoveSpeed))});
""",
        "production movement modifier")

    text = replace_once(
        text,
        """            queueCombatProfile(context, deferred, definition->combat);
""",
        """            queueCombatProfile(
                context,
                deferred,
                definition->combat,
                false,
                definition->cellsPerTick,
                ownerTeam ? ownerTeam->id : 0);
""",
        "production combat profile")

    text = replace_once(
        text,
        """            if (definition && definition->combat.maximumHealth > 0 &&
                !world.try_get<Health>(entity)) {
                queueCombatProfile(
                    context, entity, definition->combat);
            }
""",
        """            if (definition &&
                !world.try_get<TunableStats>(entity)) {
                queueCombatProfile(
                    context,
                    entity,
                    definition->combat,
                    true,
                    0,
                    site->ownerTeam);
            }
""",
        "construction tunable profile")

    sync_marker = "    void synchronizeConstructionCombat("
    sync_function = """    void synchronizeTeamModifiers(ecs::World& world) {
        for (const auto entity : world.view<Team, TunableStats>()) {
            modifiers_.applyEntity<MoveSpeed>(world, entity);
        }
    }

"""
    text = replace_once(
        text, sync_marker, sync_function + sync_marker,
        "team modifier synchronization helper")

    attach_start = "    void attachCombatProfile("
    queue_marker = "    template<class Target>\n    void queueCombatProfile"
    attach_block = """    void attachCombatProfile(ecs::Entity entity,
                             const CombatStats& profile,
                             bool building,
                             std::int32_t baseMoveSpeed) {
        world_.emplace<TunableStats>(
            entity, TunableStats{building, baseMoveSpeed, profile});
        if (profile.maximumHealth <= 0) return;
        world_.emplace<Health>(
            entity,
            Health{profile.maximumHealth, profile.maximumHealth});
        world_.emplace<Armor>(
            entity, Armor{std::max<std::int32_t>(0, profile.armor)});
        if (profile.bounty > 0) {
            world_.emplace<Bounty>(entity, Bounty{profile.bounty});
        }
        if (profile.attackCapable()) {
            world_.emplace<Weapon>(
                entity,
                Weapon{profile.weaponDamage,
                       profile.weaponRange,
                       std::max<std::uint32_t>(1, profile.cooldownTicks),
                       0});
            world_.emplace<CombatTarget>(entity, CombatTarget{});
            world_.emplace<CombatDirective>(entity, CombatDirective{});
        }
    }

"""
    text = replace_between(
        text, attach_start, queue_marker, attach_block,
        "attach combat profile")

    queue_start_marker = "    template<class Target>\n    void queueCombatProfile"
    death_marker = "    void handleDeath("
    queue_block = """    template<class Target>
    void queueCombatProfile(
        const ecs::SystemContext& context,
        Target target,
        const CombatStats& profile,
        bool building,
        std::int32_t baseMoveSpeed,
        std::uint32_t teamId) {
        structuralCommands_.add(
            context,
            target,
            TunableStats{building, baseMoveSpeed, profile});
        if (profile.maximumHealth <= 0) return;

        const auto& teamProfile = modifiers_.profile(teamId);
        const auto healthMultiplier = building
            ? teamProfile.buildingHealth
            : teamProfile.unitHealth;
        const auto damageMultiplier = building
            ? teamProfile.buildingDamage
            : teamProfile.unitDamage;
        const auto maximumHealth = std::max<std::int32_t>(
            1, ScaleGameplayValue(
                   profile.maximumHealth, healthMultiplier));
        const auto armor = std::max<std::int32_t>(
            0, profile.armor +
                   (building ? 0 : teamProfile.unitArmorAdd));

        structuralCommands_.add(
            context, target,
            Health{maximumHealth, maximumHealth});
        structuralCommands_.add(context, target, Armor{armor});
        if (profile.bounty > 0) {
            structuralCommands_.add(
                context, target, Bounty{profile.bounty});
        }
        if (profile.attackCapable()) {
            structuralCommands_.add(
                context,
                target,
                Weapon{
                    std::max<std::int32_t>(
                        0, ScaleGameplayValue(
                               profile.weaponDamage,
                               damageMultiplier)),
                    std::max<std::int32_t>(0, profile.weaponRange),
                    std::max<std::uint32_t>(1, profile.cooldownTicks),
                    0});
            structuralCommands_.add(
                context, target, CombatTarget{});
            structuralCommands_.add(
                context, target, CombatDirective{});
        }
    }

"""
    text = replace_between(
        text, queue_start_marker, death_marker, queue_block,
        "queue combat profile")

    bounty_start = text.find("        const auto* bounty =", text.find("    void handleDeath("))
    forward_marker = "    void forwardCombatEvents()"
    bounty_end = text.find(forward_marker, bounty_start)
    if bounty_start < 0 or bounty_end < 0:
        raise RuntimeError("death bounty block not found")
    bounty_block = """        const auto* bounty = world.try_get<Bounty>(victim);
        const auto* killerTeam = world.try_get<Team>(killer);
        const auto* victimTeam = world.try_get<Team>(victim);
        if (bounty && bounty->amount > 0 && killerTeam &&
            victimTeam &&
            killerTeam->id == playerTeamId_ &&
            killerTeam->id != victimTeam->id) {
            const auto awarded =
                modifiers_.bounty(killerTeam->id, bounty->amount);
            if (awarded > 0) {
                resources_.available += awarded;
                deathSideEffects_.push_back(
                    {context.tick,
                     DomainEventType::BountyAwarded,
                     killer,
                     0,
                     0,
                     victim,
                     awarded});
            }
        }
    }

"""
    text = text[:bounty_start] + bounty_block + text[bounty_end:]

    text = replace_once(
        text,
        """        const auto* bounty = world.try_get<Bounty>(entity);
        hash.WriteBool(bounty != nullptr);
        if (bounty) hash.WriteI32(bounty->amount);
""",
        """        const auto* bounty = world.try_get<Bounty>(entity);
        hash.WriteBool(bounty != nullptr);
        if (bounty) hash.WriteI32(bounty->amount);

        const auto* tunable = world.try_get<TunableStats>(entity);
        hash.WriteBool(tunable != nullptr);
        if (tunable) {
            hash.WriteBool(tunable->building);
            hash.WriteI32(tunable->baseMoveSpeed);
            hash.WriteI32(tunable->baseCombat.maximumHealth);
            hash.WriteI32(tunable->baseCombat.armor);
            hash.WriteI32(tunable->baseCombat.weaponDamage);
            hash.WriteI32(tunable->baseCombat.weaponRange);
            hash.WriteU32(tunable->baseCombat.cooldownTicks);
            hash.WriteI32(tunable->baseCombat.bounty);
        }
""",
        "hash tunable stats")

    text = replace_once(
        text,
        """        snapshot_.resources = resources_;
        snapshot_.entities.clear();
""",
        """        snapshot_.resources = resources_;
        snapshot_.teamModifiers = modifiers_.entries();
        snapshot_.entities.clear();
""",
        "snapshot modifier profiles")

    text = replace_once(
        text,
        """        hash.WriteI32(resources_.spent);
        hash.WriteU64(navigation_.revision());
""",
        """        hash.WriteI32(resources_.spent);
        modifiers_.appendHash(hash);
        hash.WriteU64(navigation_.revision());
""",
        "hash modifier profiles")

    text = replace_once(
        text,
        """            hash.WriteU32(site->requiredTicks);
            hash.WriteBool(site->producer);
""",
        """            hash.WriteU32(site->requiredTicks);
            hash.WriteU32(site->baseRequiredTicks);
            hash.WriteBool(site->producer);
""",
        "hash construction base duration")

    text = replace_once(
        text,
        """                    hash.WriteU32(item.requiredTicks);
""",
        """                    hash.WriteU32(item.requiredTicks);
                    hash.WriteU32(item.baseRequiredTicks);
""",
        "hash production base duration")

    text = replace_once(
        text,
        """    BaseBuildingRuntime building_;
    CombatRuntime combat_;
""",
        """    BaseBuildingRuntime building_;
    CombatRuntime combat_;
    GameplayModifierSystem modifiers_;
""",
        "modifier system member")

    text = replace_once(
        text,
        """    std::vector<BuildingDefinition> buildingDefinitions_;
    std::vector<UnitDefinition> unitDefinitions_;
""",
        """    DefinitionCatalog<BuildingDefinition> buildingDefinitions_;
    DefinitionCatalog<UnitDefinition> unitDefinitions_;
""",
        "definition catalog members")

    write(path, text)


def refactor_tower_defense() -> None:
    path = "framework/tower_defense/include/RTSEngine/TowerDefense/Simulation.h"
    text = read(path)
    text = replace_once(
        text,
        """#include <rts/foundation/CanonicalHash.h>
""",
        """#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/DeterministicCommandStream.h>
""",
        "tower command stream include")

    stream_start = "class TickCommandStream {"
    event_marker = "enum class EventType"
    text = replace_between(
        text, stream_start, event_marker,
        "using TickCommandStream =\n    sim::DeterministicCommandStream<TickCommand>;\n\n",
        "tower command stream")

    text = replace_once(
        text,
        """    void setPlayerTeam(std::uint32_t teamId) noexcept {
        playerTeamId_ = teamId;
        rts_.setPlayerTeam(teamId);
    }
""",
        """    void setPlayerTeam(std::uint32_t teamId) noexcept {
        playerTeamId_ = teamId;
        rts_.setPlayerTeam(teamId);
    }

    bool setTeamModifierProfile(
        std::uint32_t teamId,
        gameplay::TeamModifierProfile profile) {
        return rts_.setTeamModifierProfile(teamId, profile);
    }
""",
        "tower modifier forwarding")
    write(path, text)


def refactor_roguelite() -> None:
    path = "framework/roguelite/include/RTSEngine/Roguelite/RunSimulation.h"
    text = read(path)
    text = replace_once(
        text,
        """#include <RTSEngine/Roguelite/ModifierRuntime.h>
""",
        """#include <RTSEngine/Roguelite/GameplayStats.h>
#include <RTSEngine/Roguelite/ModifierRuntime.h>
""",
        "roguelite gameplay stats include")
    text = replace_once(
        text,
        """#include <rts/foundation/CanonicalHash.h>
""",
        """#include <rts/foundation/CanonicalHash.h>
#include <rts/sim/DeterministicCommandStream.h>
""",
        "roguelite command stream include")

    stream_start = "class TickCommandStream {"
    failure_marker = "enum class RunFailure"
    text = replace_between(
        text, stream_start, failure_marker,
        "using TickCommandStream =\n    sim::DeterministicCommandStream<TickCommand>;\n\n",
        "roguelite command stream")

    text = replace_once(
        text,
        """    std::int32_t waveCompletionResourceBonus{};
    std::vector<ModifierStack> modifiers;
""",
        """    std::int32_t waveCompletionResourceBonus{};
    gameplay::TeamModifierProfile gameplayProfile{};
    std::vector<ModifierStack> modifiers;
""",
        "run snapshot gameplay profile")

    text = replace_once(
        text,
        """    void setPlayerTeam(std::uint32_t teamId) noexcept {
        tower_.setPlayerTeam(teamId);
    }
""",
        """    void setPlayerTeam(std::uint32_t teamId) noexcept {
        playerTeamId_ = teamId;
        tower_.setPlayerTeam(teamId);
        synchronizeGameplayModifiers();
    }
""",
        "run player team profile")

    text = replace_once(
        text,
        """        if (result.accepted) {
            events_.push_back(
""",
        """        if (result.accepted) {
            synchronizeGameplayModifiers();
            events_.push_back(
""",
        "modifier application synchronization")

    snapshot_marker = "    void buildSnapshot(std::uint64_t tick)"
    synchronize = """    void synchronizeGameplayModifiers() {
        tower_.setTeamModifierProfile(
            playerTeamId_, ResolveGameplayProfile(modifiers_));
    }

"""
    text = replace_once(
        text, snapshot_marker, synchronize + snapshot_marker,
        "gameplay modifier bridge")

    text = replace_once(
        text,
        """        snapshot_.waveCompletionResourceBonus =
            modifiers_.resolve(WaveCompletionResourceStat(), 0);
        snapshot_.modifiers = modifiers_.stacks();
""",
        """        snapshot_.waveCompletionResourceBonus =
            modifiers_.resolve(WaveCompletionResourceStat(), 0);
        snapshot_.gameplayProfile =
            ResolveGameplayProfile(modifiers_);
        snapshot_.modifiers = modifiers_.stacks();
""",
        "snapshot resolved gameplay profile")

    text = replace_once(
        text,
        """        hash.WriteI32(snapshot_.waveCompletionResourceBonus);

        const auto* run = findById(runs_, state_.runId);
""",
        """        hash.WriteI32(snapshot_.waveCompletionResourceBonus);
        hash.WriteU32(playerTeamId_);
        hash.WriteI32(snapshot_.gameplayProfile.unitHealth);
        hash.WriteI32(snapshot_.gameplayProfile.unitDamage);
        hash.WriteI32(snapshot_.gameplayProfile.unitArmorAdd);
        hash.WriteI32(snapshot_.gameplayProfile.unitMoveSpeed);
        hash.WriteI32(snapshot_.gameplayProfile.buildingHealth);
        hash.WriteI32(snapshot_.gameplayProfile.buildingDamage);
        hash.WriteI32(snapshot_.gameplayProfile.constructionSpeed);
        hash.WriteI32(snapshot_.gameplayProfile.productionSpeed);
        hash.WriteI32(snapshot_.gameplayProfile.bountyMultiplier);

        const auto* run = findById(runs_, state_.runId);
""",
        "hash resolved gameplay profile")

    text = replace_once(
        text,
        """    std::uint32_t nextInternalSequence_{1};
    bool hasStepped_{};
""",
        """    std::uint32_t nextInternalSequence_{1};
    std::uint32_t playerTeamId_{1};
    bool hasStepped_{};
""",
        "run player team member")
    write(path, text)


def update_tests() -> None:
    path = "tests/CMakeLists.txt"
    text = read(path)
    block = """

add_executable(rts_command_stream_tests deterministic_command_stream_tests.cpp)
target_link_libraries(rts_command_stream_tests PRIVATE RTSEngine::Sim)
add_test(NAME rts_command_stream_tests COMMAND rts_command_stream_tests)

add_executable(rts_gameplay_modifier_tests gameplay_modifier_tests.cpp)
target_link_libraries(rts_gameplay_modifier_tests PRIVATE RTSEngine::Rts)
add_test(NAME rts_gameplay_modifier_tests COMMAND rts_gameplay_modifier_tests)
"""
    if "rts_command_stream_tests" not in text:
        text += block
    write(path, text)


refactor_rts_simulation()
refactor_tower_defense()
refactor_roguelite()
update_tests()
print("next slice refactor applied")
