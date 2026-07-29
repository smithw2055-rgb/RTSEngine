#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Assets/CookedAsset.h>
#include <RTSEngine/Assets/Vfs.h>
#include <RTSEngine/RtsScripting/RtsScriptWorldRuntime.h>
#include <RTSEngine/Scripting/RealScriptHost.h>
#include <RTSEngine/Scripting/ScriptBundle.h>

#include <realscript/bytecode/Bytecode.h>
#include <realscript/game/GameScripting.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace rts;
using namespace rts::gameplay;
namespace rs = rts::gameplay::scripting;

#define require(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "RS4 assertion failed at line " << __LINE__          \
                      << ": " #condition "\n";                               \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

const char* source = R"(
module Game.EntityBehaviors;
import Engine.RtsBehavior;

class UnitBehavior
{
    int starts;
    int events;
    int triggers;
    int thinks;
    int destroyed;
    long lastTrigger;

    void OnCreate()
    {
        starts = 0;
        events = 0;
        triggers = 0;
        thinks = 0;
        destroyed = 0;
        lastTrigger = 0;
    }

    void OnStart()
    {
        starts = starts + 1;
    }

    void OnEvent()
    {
        events = events + 1;
    }

    void OnTrigger()
    {
        triggers = triggers + 1;
        lastTrigger = TriggerId();
    }

    void OnThink()
    {
        thinks = thinks + 1;
        long enemy = FindNearestVisibleEnemy();
        if (enemy != 0)
        {
            bool queued = Attack(enemy);
        }
    }

    void OnDestroy()
    {
        destroyed = destroyed + 1;
    }
}
)";

void writeCooked(
    assets::MemoryVfs& vfs,
    const std::string& path,
    assets::CookedAsset asset) {
    const auto bytes = assets::EncodeCookedAsset(std::move(asset));
    require(!bytes.empty());
    require(vfs.write(path, bytes));
}

struct ScriptFixture final {
    assets::MemoryVfs vfs;
    assets::AssetManager assets{vfs, 32u * 1024u * 1024u};
    rs::RtsScriptApi api;
    rs::RtsEntityBehaviorApi behaviorApi{api};
    rts::scripting::RealScriptHost host{assets, api.gameApi()};
    assets::AssetKey bundleKey{assets::AssetType::ScriptBundle, 940};
    std::shared_ptr<rts::scripting::ScriptProgram> program;

    ScriptFixture() {
        realscript::game::GameScriptCompiler compiler(api.gameApi());
        const auto compiled = compiler.compile({{"entity_behavior.rs", source}});
        require(compiled.succeeded());
        require(!compiled.modules.empty());

        std::vector<assets::CookedAsset> modules;
        std::uint64_t moduleId = 9400;
        for (const auto& module : compiled.modules) {
            modules.push_back(rts::scripting::ScriptAssetCodec::moduleAsset(
                moduleId++, realscript::bytecode::encodeModule(module)));
        }

        rts::scripting::ScriptDiagnostics diagnostics;
        const auto described = host.describeBundle(modules, diagnostics);
        require(described.has_value());
        require(!diagnostics.hasErrors());
        for (const auto& module : modules) {
            const auto path =
                "scripts/modules/" + std::to_string(module.key.id) + ".rta";
            writeCooked(vfs, path, module);
            require(assets.registerAsset({
                module.key,
                path,
                rts::scripting::kScriptModuleSchemaVersion}));
        }
        const auto bundle = rts::scripting::ScriptAssetCodec::bundleAsset(
            bundleKey.id, *described);
        writeCooked(vfs, "scripts/entity_behavior.rta", bundle);
        require(assets.registerAsset({
            bundleKey,
            "scripts/entity_behavior.rta",
            rts::scripting::kScriptBundleSchemaVersion}));
        const auto request = assets.request(bundleKey);
        require(request.valid());
        require(assets.process() == 1);
        const auto loaded = host.load(bundleKey);
        require(loaded.succeeded());
        program = loaded.program;
        require(program && program->valid());
        require(assets.releaseRequest(request));
    }
};

CombatStats combatProfile() {
    CombatStats result;
    result.maximumHealth = 100;
    result.weaponDamage = 4;
    result.weaponRange = 16;
    result.cooldownTicks = 1;
    return result;
}

std::int64_t memberInt(
    const rs::RtsEntityBehaviorRuntime& behaviors,
    ecs::Entity entity,
    const std::string& name) {
    realscript::runtime::RuntimeError error;
    const auto value = behaviors.member(entity, name, error);
    require(value.has_value());
    return std::get<std::int64_t>(*value);
}

struct Scenario final {
    ScriptFixture scripts;
    RtsGameSession session{24, 12};
    ecs::Entity own;
    ecs::Entity enemy;
    rs::RtsScriptSession teams;
    rs::RtsEntityBehaviorRuntime behaviors;
    rs::RtsScriptWorldRuntime world;

    Scenario()
        : own(session.createUnit(
              {3, 5}, {1}, 1, combatProfile(), 20)),
          enemy(session.createUnit(
              {10, 5}, {1}, 2, combatProfile(), 20)),
          teams(session, scripts.program, scripts.api),
          behaviors(session, scripts.program, scripts.behaviorApi),
          world(session, teams, behaviors) {
        require(own.valid() && enemy.valid());
        rs::RtsEntityBehaviorDefinition definition;
        definition.entity = own;
        definition.scriptType = "Game.EntityBehaviors::UnitBehavior";
        definition.thinkIntervalTicks = 1;
        definition.maximumIntentsPerTick = 8;
        definition.executionPolicy.instructionBudget = 10000;
        require(behaviors.attach(std::move(definition)));

        rs::RtsScriptTriggerDefinition first;
        first.id = 1;
        first.kind = rs::RtsScriptTriggerKind::Tick;
        first.firstTick = 0;
        first.once = true;
        require(behaviors.addTrigger(own, first));

        auto second = first;
        second.id = 2;
        require(behaviors.addTrigger(own, second));

        rs::RtsScriptTriggerDefinition health;
        health.id = 3;
        health.kind = rs::RtsScriptTriggerKind::HealthAtMost;
        health.threshold = 100;
        health.once = true;
        require(behaviors.addTrigger(own, health));

        rs::RtsScriptTriggerDefinition visible;
        visible.id = 4;
        visible.kind = rs::RtsScriptTriggerKind::VisibleEnemy;
        visible.once = true;
        require(behaviors.addTrigger(own, visible));
        require(world.valid());
    }
};

std::uint64_t worldHash(const Scenario& scenario) {
    std::uint64_t hash = 0;
    realscript::runtime::RuntimeError error;
    require(scenario.world.authoritativeHash(hash, error));
    return hash;
}

void testLifecycleTriggersAndCommands() {
    Scenario scenario;
    require(scenario.session.stepDetailed(0) == RtsStepResult::Advanced);
    require(scenario.world.processCompletedTick(0) ==
            rs::RtsScriptWorldTickResult::Processed);
    require(memberInt(scenario.behaviors, scenario.own, "starts") == 1);
    require(memberInt(scenario.behaviors, scenario.own, "thinks") == 1);
    require(memberInt(scenario.behaviors, scenario.own, "triggers") >= 4);
    require(memberInt(scenario.behaviors, scenario.own, "lastTrigger") == 4);
    require(scenario.behaviors.errors().empty());
    require(scenario.behaviors.outcomes().size() == 1);
    require(scenario.behaviors.outcomes().front().sequence >=
            rs::RtsEntityBehaviorRuntime::kFirstBehaviorSequence);
    require(scenario.behaviors.outcomes().front().type == CommandType::Attack);
    require(scenario.behaviors.outcomes().front().result ==
            SessionCommandResult::Accepted);
}

void testWorldArchiveRestoresAndReplays() {
    Scenario scenario;
    require(scenario.session.stepDetailed(0) == RtsStepResult::Advanced);
    require(scenario.world.processCompletedTick(0) ==
            rs::RtsScriptWorldTickResult::Processed);
    const auto tickZeroHash = worldHash(scenario);

    realscript::runtime::RuntimeError error;
    const auto archive = scenario.world.encode(error);
    require(!archive.empty());

    require(scenario.session.stepDetailed(1) == RtsStepResult::Advanced);
    require(scenario.world.processCompletedTick(1) ==
            rs::RtsScriptWorldTickResult::Processed);
    require(memberInt(scenario.behaviors, scenario.own, "thinks") == 2);
    const auto finalHash = worldHash(scenario);

    require(scenario.world.restore(archive, error));
    require(scenario.session.simulation().nextExpectedTick() == 1);
    require(memberInt(scenario.behaviors, scenario.own, "thinks") == 1);
    require(worldHash(scenario) == tickZeroHash);

    require(scenario.session.stepDetailed(1) == RtsStepResult::Advanced);
    require(scenario.world.processCompletedTick(1) ==
            rs::RtsScriptWorldTickResult::Processed);
    require(worldHash(scenario) == finalHash);
}

void testWorldArchiveRebuildsObjectsInAnotherRuntime() {
    Scenario sourceScenario;
    require(sourceScenario.session.stepDetailed(0) == RtsStepResult::Advanced);
    require(sourceScenario.world.processCompletedTick(0) ==
            rs::RtsScriptWorldTickResult::Processed);
    const auto expectedHash = worldHash(sourceScenario);

    realscript::runtime::RuntimeError error;
    const auto archive = sourceScenario.world.encode(error);
    require(!archive.empty());

    ScriptFixture targetScripts;
    RtsGameSession targetSession(24, 12);
    rs::RtsScriptSession targetTeams(
        targetSession, targetScripts.program, targetScripts.api);
    rs::RtsEntityBehaviorRuntime targetBehaviors(
        targetSession, targetScripts.program, targetScripts.behaviorApi);
    rs::RtsScriptWorldRuntime targetWorld(
        targetSession, targetTeams, targetBehaviors);
    require(targetWorld.restore(archive, error));
    require(targetSession.simulation().nextExpectedTick() == 1);
    require(memberInt(targetBehaviors, sourceScenario.own, "starts") == 1);
    require(memberInt(targetBehaviors, sourceScenario.own, "triggers") >= 4);
    std::uint64_t restoredHash = 0;
    require(targetWorld.authoritativeHash(restoredHash, error));
    require(restoredHash == expectedHash);
}

} // namespace

int main() {
    testLifecycleTriggersAndCommands();
    testWorldArchiveRestoresAndReplays();
    testWorldArchiveRebuildsObjectsInAnotherRuntime();
    std::cout << "RTSEngine RealScript Stage RS4 tests passed\n";
    return 0;
}
