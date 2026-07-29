#include <RTSEngine/RtsScripting/RtsScriptAdapter.h>

#include <realscript/compiler/Compilation.h>
#include <realscript/game/GameScripting.h>
#include <realscript/runtime/Runtime.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

using namespace rts;
using namespace rts::gameplay;
using namespace rts::gameplay::scripting;

void check(bool value, const char* expression, int line) {
    if (value) return;
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    std::abort();
}

#define CHECK(expression) check((expression), #expression, __LINE__)

const char* source = R"(
module Game.AI;
import Engine.Rts;

int Plan(int team)
{
    long unit = FirstUnit(team);
    long enemy = FirstVisibleEnemy(team);
    if (enemy != 0)
    {
        Attack(unit, enemy);
    }
    else
    {
        AttackMove(unit, 9, 7);
    }
    return UnitCount(team);
}
)";

void testEntityPacking() {
    ecs::Entity entity{42, 7};
    CHECK(UnpackScriptEntity(PackScriptEntity(entity)) == entity);
    CHECK(PackScriptEntity({}) == 0);
    CHECK(!UnpackScriptEntity(0).valid());
}

void testQueriesAndIntentBuffer() {
    RtsGameSession session(16, 16);
    CHECK(session.setRelation(1, 2, DiplomaticRelation::Hostile));
    const CombatStats combat{10, 0, 1, 3, 2, 0};
    const auto friendly = session.createUnit({1, 1}, {1}, 1, combat, 6);
    const auto enemy = session.createUnit({2, 1}, {1}, 2, combat, 6);
    CHECK(friendly.valid() && enemy.valid());
    CHECK(session.step(0));

    auto context = std::make_shared<RtsScriptContext>();
    auto api = CreateRtsScriptApi(context);
    CHECK(api.valid());

    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"team_ai.rs", source}});
    if (!compiled.succeeded()) {
        for (const auto& item : compiled.diagnostics.items()) {
            std::cerr << item.code << ": " << item.message << '\n';
        }
    }
    CHECK(compiled.succeeded());

    realscript::runtime::EngineRuntime runtime(compiled.program.program());
    runtime.setBindings(compiled.program.bindings());
    runtime.setHeap(compiled.program.heap());
    runtime.setNativeHandles(compiled.program.nativeHandles());

    context->begin(session, 1, 1);
    realscript::runtime::ExecutionOptions options;
    options.determinism.mode = realscript::runtime::DeterminismMode::Strict;
    const auto result = runtime.invoke(
        "Game.AI::Plan", {std::int64_t{1}}, options);
    if (!result.succeeded) {
        std::cerr << "runtime error: " << result.error.message << '\n';
    }
    CHECK(result.succeeded);
    CHECK(std::get<std::int64_t>(result.value) == 1);
    CHECK(context->intents.size() == 1);
    CHECK(context->intents.front().subject == friendly);
    CHECK(context->intents.front().type == RtsScriptIntentType::Attack);
    CHECK(context->intents.front().target == enemy);
    context->clear();
}

} // namespace

int main() {
    testEntityPacking();
    testQueriesAndIntentBuffer();
    return 0;
}
