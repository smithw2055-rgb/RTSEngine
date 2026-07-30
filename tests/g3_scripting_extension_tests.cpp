#include <RTSEngine/Rts/G3GameSession.h>
#include <RTSEngine/RtsScripting/G3ScriptExtension.h>

#include <realscript/game/GameScripting.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

using namespace rts;
using namespace rts::gameplay;
namespace rs = rts::gameplay::scripting;

#define require(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "G3 script assertion failed at line " << __LINE__   \
                      << ": " #condition "\n";                               \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

CombatStats combat() {
    CombatStats value;
    value.maximumHealth = 100;
    value.cooldownTicks = 1;
    return value;
}

const char* source = R"(
module Game.G3Brain;
import Engine.G3;

class Brain
{
    void Queue(long caster, long target)
    {
        bool submitted = CastEntity(caster, 401, target);
    }

    bool Slowed(long entity)
    {
        return HasStatus(entity, 402);
    }
}
)";

void testG3ScriptExtension() {
    RtsG3GameSession game(24, 12);

    StatusEffectDefinition status;
    status.id = 402;
    status.durationTicks = 8;
    require(game.registerStatusEffect(status));

    AbilityDefinition ability;
    ability.id = 401;
    ability.cooldownTicks = 2;
    ability.castTicks = 0;
    ability.rangeQ16 =
        static_cast<std::uint32_t>(
            FixedPosition2D::kOne * 12);
    ability.targetKind = AbilityTargetKind::Entity;
    ability.targetEnemies = true;
    AbilityEffectDefinition damage;
    damage.kind = AbilityEffectKind::Damage;
    damage.amount = 15;
    damage.damageType = G3DamageType::TrueDamage;
    ability.effects.push_back(damage);
    require(game.registerAbility(ability));

    const auto caster = game.base().createUnit(
        {2, 2}, {1}, 1, combat());
    const auto target = game.base().createUnit(
        {7, 2}, {1}, 2, combat());
    require(caster.valid() && target.valid());
    require(game.grantAbility(caster, ability.id));
    require(game.applyStatus(caster, status.id, caster));

    rs::RtsScriptApi api;
    rs::RtsG3ScriptExtension extension(game, api);

    // Installing the extension before compilation makes Engine.G3 part of the
    // stable Host API contract and allows normal RealScript imports.
    realscript::game::GameScriptCompiler compiler(api.gameApi());
    const auto compiled = compiler.compile(
        {{"game/g3_brain.rs", source}});
    require(compiled.succeeded());

    require(extension.hasStatus(
        rs::packScriptEntity(caster), status.id));
    require(extension.statusStacks(
        rs::packScriptEntity(caster), status.id) == 1);

    require(extension.beginScope(1, 0, 8));
    require(extension.queueEntity(
        rs::packScriptEntity(caster),
        ability.id,
        rs::packScriptEntity(target)));
    require(extension.commit());
    require(!extension.active());
    require(extension.outcomes().size() == 1);
    require(extension.outcomes().front().result ==
            AbilitySubmitResult::Accepted);

    const auto state = extension.encodeState();
    const auto stateHash = extension.authoritativeHash();
    require(!state.empty());
    require(stateHash != 0);
    require(extension.restoreState(state));
    require(extension.authoritativeHash() == stateHash);

    require(game.step(0));
    const auto* health =
        game.base().simulation().world().
            try_get<Health>(target);
    require(health && health->current == 85);
}

} // namespace

int main() {
    testG3ScriptExtension();
    return 0;
}
