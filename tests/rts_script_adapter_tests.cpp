#include <RTSEngine/RtsScripting/RtsScriptAdapter.h>

#include <realscript/compiler/Compilation.h>
#include <realscript/game/GameScripting.h>
#include <realscript/runtime/Runtime.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

using namespace rts;
using namespace rts::gameplay;
using namespace rts::gameplay::scripting;

void check(bool value) {
    assert(value);
    if (!value) std::abort();
}

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
    check(UnpackScriptEntity(PackScriptEntity(entity)) == entity);
    check(PackScriptEntity({}) == 0);
    check(!UnpackScriptEntity(0).valid());
}

void testQueriesAndIntentBuffer() {
    RtsGameSession session(16, 16);
    check(session.setRelation(1, 2, DiplomaticRelation::Hostile));
    const auto friendly = session.createUnit({1, 1}, {1}, 1);
    const auto enemy = session.createUnit({2, 1}, {1}, 2);
    check(friendly.valid() && enemy.valid());
    check(session.step(0));

    auto context = std::make_shared<RtsScriptContext>();
    auto api = CreateRtsScriptApi(context);
    check(api.valid());

    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"team_ai.rs", source}});
    check(compiled.succeeded());

    realscript::runtime::EngineRuntime runtime(compiled.program.program());
    runtime.setBindings(compiled.program.bindings());
    runtime.setHeap(compiled.program.heap());
    runtime.setNativeHandles(compiled.program.nativeHandles());

    context->begin(session, 1, 1);
    realscript::runtime::ExecutionOptions options;
    options.determinism.mode = realscript::runtime::DeterminismMode::Strict;
    const auto result = runtime.invoke(
        "Game.AI::Plan", {std::int64_t{1}}, options);
    check(result.succeeded);
    check(std::get<std::int64_t>(result.value) == 1);
    check(context->intents.size() == 1);
    check(context->intents.front().subject == friendly);
    check(context->intents.front().type == RtsScriptIntentType::Attack);
    check(context->intents.front().target == enemy);
    context->clear();
}

} // namespace

int main() {
    testEntityPacking();
    testQueriesAndIntentBuffer();
    return 0;
}
