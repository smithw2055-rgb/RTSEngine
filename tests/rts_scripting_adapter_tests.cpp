#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Assets/CookedAsset.h>
#include <RTSEngine/Assets/Vfs.h>
#include <RTSEngine/Rts/RtsGameSession.h>
#include <RTSEngine/RtsScripting/RtsScriptSession.h>
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
            std::cerr << "RS2 assertion failed at line " << __LINE__          \
                      << ": " #condition "\n";                               \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

void writeCooked(
    assets::MemoryVfs& vfs,
    const std::string& path,
    assets::CookedAsset asset) {
    const auto bytes = assets::EncodeCookedAsset(std::move(asset));
    require(!bytes.empty());
    require(vfs.write(path, bytes));
}

CombatStats combatProfile() {
    CombatStats result;
    result.maximumHealth = 100;
    result.weaponDamage = 5;
    result.weaponRange = 2;
    result.cooldownTicks = 1;
    return result;
}

const char* teamAiSource = R"(
module Game.TeamAi;
import Engine.Rts;

class TeamBrain
{
    int team;
    int observedEvents;

    void OnCreate(int teamId)
    {
        team = teamId;
    }

    void OnStart()
    {
        observedEvents = 0;
    }

    void OnEvent(int eventType, long entity, long secondary, int value)
    {
        observedEvents = observedEvents + 1;
    }

    void OnThink(long tick)
    {
        long unit = FindIdleUnit();
        if (unit != 0)
        {
            long enemy = FindNearestVisibleEnemy(unit);
            if (enemy != 0)
            {
                bool queued = Attack(unit, enemy);
            }
        }
    }
}
)";

struct ScriptFixture final {
    assets::MemoryVfs vfs;
    assets::AssetManager assets{vfs, 32u * 1024u * 1024u};
    rs::RtsScriptApi api;
    rts::scripting::RealScriptHost host{assets, api.gameApi()};
    assets::AssetKey bundleKey{assets::AssetType::ScriptBundle, 700};
    std::shared_ptr<rts::scripting::ScriptProgram> program;

    ScriptFixture() {
        realscript::game::GameScriptCompiler compiler(api.gameApi());
        const auto compiled = compiler.compile({{"game/team_ai.rs", teamAiSource}});
        require(compiled.succeeded());
        require(!compiled.modules.empty());

        std::vector<assets::CookedAsset> modules;
        std::uint64_t moduleId = 701;
        for (const auto& module : compiled.modules) {
            modules.push_back(rts::scripting::ScriptAssetCodec::moduleAsset(
                moduleId++, realscript::bytecode::encodeModule(module)));
            require(modules.back().key.valid());
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

        const auto bundleAsset = rts::scripting::ScriptAssetCodec::bundleAsset(
            bundleKey.id, *described);
        require(bundleAsset.key == bundleKey);
        writeCooked(vfs, "scripts/team_ai.rta", bundleAsset);
        require(assets.registerAsset({
            bundleKey,
            "scripts/team_ai.rta",
            rts::scripting::kScriptBundleSchemaVersion}));
        const auto request = assets.request(bundleKey);
        require(request.valid());
        require(assets.process() == 1);
        assets::AssetRequestStatus status;
        require(assets.requestStatus(request, status));
        require(status.completed && status.state == assets::AssetState::Ready);
        const auto loaded = host.load(bundleKey);
        require(loaded.succeeded());
        program = loaded.program;
        require(assets.releaseRequest(request));
    }
};

struct ScenarioResult final {
    std::uint64_t hashBeforeScripts{};
    std::uint64_t hashAfterScripts{};
    TickCommand command;
    rs::RtsScriptTickReport report;
};

ScenarioResult runScenario() {
    ScriptFixture scripts;
    RtsGameSession session(24, 12);
    const auto own = session.createUnit(
        {3, 5}, {1}, 1, combatProfile(), 20);
    const auto enemy = session.createUnit(
        {10, 5}, {1}, 2, combatProfile(), 20);
    require(own.valid() && enemy.valid());
    require(session.setTeamResource(1, kPrimaryResourceType, 200));

    rs::RtsScriptSession adapter(session, scripts.program, scripts.api);
    require(adapter.valid());
    rs::RtsTeamScriptDefinition definition;
    definition.teamId = 1;
    definition.scriptType = "Game.TeamAi::TeamBrain";
    definition.thinkIntervalTicks = 1;
    definition.maximumIntentsPerTick = 16;
    definition.executionPolicy.instructionBudget = 10000;
    require(adapter.registerTeam(std::move(definition)));

    require(session.stepDetailed(0) == RtsStepResult::Advanced);
    ScenarioResult result;
    result.hashBeforeScripts = session.simulation().snapshot().worldHash;
    require(adapter.processCompletedTick(0) == rs::RtsScriptTickResult::Processed);
    result.hashAfterScripts = session.simulation().snapshot().worldHash;
    result.report = adapter.lastReport();
    require(result.hashBeforeScripts == result.hashAfterScripts);
    require(adapter.errors().empty());
    require(adapter.outcomes().size() == 1);
    require(adapter.outcomes().front().result == SessionCommandResult::Accepted);

    const auto pending = session.simulation().commandStreamState().pending;
    require(pending.size() == 1);
    result.command = pending.front();
    require(result.command.targetTick == 1);
    require(result.command.issuer == 1);
    require(result.command.sequence == 1);
    require(result.command.type == CommandType::Attack);
    require(result.command.subject == own);
    require(result.command.targetEntity == enemy);
    require(result.report.accepted == 1 && result.report.rejected == 0);
    return result;
}

void testTeamAiEmitsNextTickCommandWithoutDirectMutation() {
    const auto result = runScenario();
    require(result.report.targetTick == 1);
    require(result.report.callbacks >= 2);
    require(result.report.intents == 1);
}

void testDeterministicCommandIdentityAcrossRuns() {
    const auto first = runScenario();
    const auto second = runScenario();
    require(first.hashBeforeScripts == second.hashBeforeScripts);
    require(first.hashAfterScripts == second.hashAfterScripts);
    require(first.command.targetTick == second.command.targetTick);
    require(first.command.issuer == second.command.issuer);
    require(first.command.sequence == second.command.sequence);
    require(first.command.type == second.command.type);
    require(first.command.subject == second.command.subject);
    require(first.command.targetEntity == second.command.targetEntity);
}

void testReadViewDoesNotExposeHiddenEnemy() {
    RtsGameSession session(64, 8);
    const auto own = session.createUnit(
        {2, 3}, {1}, 1, combatProfile(), 4);
    const auto hidden = session.createUnit(
        {50, 3}, {1}, 2, combatProfile(), 4);
    require(own.valid() && hidden.valid());
    require(session.step(0));

    const auto view = rs::RtsScriptReadView::capture(session, 1);
    require(view.entity(rs::packScriptEntity(own)) != nullptr);
    require(view.entity(rs::packScriptEntity(hidden)) == nullptr);
    require(view.findNearestVisibleEnemy(rs::packScriptEntity(own)) == 0);
}

void testScriptAndBuiltInAiCannotOwnSameTeam() {
    ScriptFixture scripts;
    RtsGameSession session(16, 8);
    require(session.registerAiTeam(1, {10, 4}, 1));
    rs::RtsScriptSession adapter(session, scripts.program, scripts.api);
    require(adapter.valid());
    require(!adapter.registerTeam({1, "Game.TeamAi::TeamBrain", 1, 16, {}}));
}

} // namespace

int main() {
    testTeamAiEmitsNextTickCommandWithoutDirectMutation();
    testDeterministicCommandIdentityAcrossRuns();
    testReadViewDoesNotExposeHiddenEnemy();
    testScriptAndBuiltInAiCannotOwnSameTeam();
    std::cout << "RTSEngine RealScript Stage RS2 tests passed\n";
    return 0;
}
