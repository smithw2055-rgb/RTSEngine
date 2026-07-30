#include <RTSEngine/Rts/G3GameSession.h>

#include <cstdlib>
#include <iostream>

namespace {

using namespace rts::gameplay;

void requireImpl(bool value, const char* expression, int line) {
    if (!value) {
        std::cerr << "G3 require failed at line " << line
                  << ": " << expression << '\n';
        std::abort();
    }
}
#define require(value) requireImpl((value), #value, __LINE__)

CombatStats soldierCombat() {
    CombatStats value;
    value.maximumHealth = 100;
    value.armor = 0;
    value.weaponDamage = 20;
    value.weaponRange = 8;
    value.cooldownTicks = 1;
    return value;
}

void registerCore(RtsG3GameSession& game) {
    require(game.base().setRelation(1, 2, DiplomaticRelation::Hostile));

    ProjectileDefinition bolt;
    bolt.id = 1;
    bolt.speedQ16 = FixedPosition2D::kOne;
    bolt.lifetimeTicks = 16;
    bolt.damage = 30;
    require(game.registerProjectile(bolt));

    StatusEffectDefinition burn;
    burn.id = 1;
    burn.durationTicks = 5;
    burn.periodTicks = 1;
    burn.maxStacks = 3;
    burn.stacking = StatusStackingPolicy::AddStacks;
    burn.periodicHealthDelta = -3;
    burn.damageScalePermille = 800;
    require(game.registerStatusEffect(burn));

    StatusEffectDefinition stun;
    stun.id = 2;
    stun.durationTicks = 2;
    stun.stunned = true;
    stun.moveScalePermille = 0;
    require(game.registerStatusEffect(stun));

    AbilityDefinition fireball;
    fireball.id = 1;
    fireball.cooldownTicks = 4;
    fireball.castTicks = 1;
    fireball.rangeQ16 = 10u * FixedPosition2D::kOne;
    fireball.targetKind = AbilityTargetKind::Entity;
    fireball.targetEnemies = true;
    fireball.effects.push_back(
        {AbilityEffectKind::Damage, 12, 0, G3DamageType::Energy, 0, 0});
    fireball.effects.push_back(
        {AbilityEffectKind::ApplyStatus, 0, 0, G3DamageType::Kinetic, 0, 1});
    require(game.registerAbility(fireball));
}

void testProjectileWeaponIsDelayed() {
    RtsG3GameSession game(16, 4);
    registerCore(game);
    const auto attacker = game.base().createUnit({1, 1}, {1}, 1, soldierCombat());
    const auto target = game.base().createUnit({5, 1}, {0}, 2, soldierCombat());
    require(game.bindEntityProjectile(attacker, 1));

    TickCommand attack;
    attack.targetTick = 0;
    attack.issuer = 1;
    attack.sequence = 1;
    attack.type = CommandType::Attack;
    attack.subject = attacker;
    attack.targetEntity = target;
    require(game.base().submit(attack));

    require(game.step(0));
    const auto* health0 = game.base().simulation().world().try_get<Health>(target);
    require(health0 && health0->current == 100);
    require(!game.projectiles().empty());

    require(game.step(1));
    require(game.step(2));
    require(game.step(3));
    require(game.step(4));
    const auto* health = game.base().simulation().world().try_get<Health>(target);
    require(health && health->current == 70);
}

void testAbilityStatusAndCooldown() {
    RtsG3GameSession game(16, 4);
    registerCore(game);
    auto casterCombat = soldierCombat();
    casterCombat.weaponDamage = 0;
    const auto caster = game.base().createUnit({1, 1}, {1}, 1, casterCombat);
    const auto target = game.base().createUnit({3, 1}, {0}, 2, soldierCombat());
    require(game.grantAbility(caster, 1));

    AbilityCommand command;
    command.targetTick = 0;
    command.issuer = 1;
    command.sequence = 1;
    command.caster = caster;
    command.abilityId = 1;
    command.targetEntity = target;
    require(game.submitAbility(command) == AbilitySubmitResult::Accepted);

    auto duplicate = command;
    require(game.submitAbility(duplicate) == AbilitySubmitResult::Duplicate);
    require(game.step(0));
    require(game.step(1));
    const auto* afterCast = game.base().simulation().world().try_get<Health>(target);
    require(afterCast && afterCast->current == 88);
    require(!game.statuses().empty());

    AbilityCommand cooldown = command;
    cooldown.targetTick = 2;
    cooldown.sequence = 2;
    require(game.submitAbility(cooldown) == AbilitySubmitResult::Accepted);
    require(game.step(2));
    bool rejected = false;
    for (const auto& event : game.events()) {
        rejected = rejected || event.type == G3EventType::AbilityRejected;
    }
    require(rejected);
    const auto* afterBurn = game.base().simulation().world().try_get<Health>(target);
    require(afterBurn && afterBurn->current < 88);
}

void testStunPausesMovementAndCombat() {
    RtsG3GameSession game(16, 4);
    registerCore(game);
    const auto unit = game.base().createUnit({1, 1}, {2}, 1, soldierCombat());
    require(game.applyStatus(unit, 2));

    TickCommand move;
    move.targetTick = 0;
    move.issuer = 1;
    move.sequence = 1;
    move.type = CommandType::Move;
    move.subject = unit;
    move.targetX = 8;
    move.targetY = 1;
    require(game.base().submit(move));
    require(game.step(0));
    const auto* position = game.base().simulation().world().try_get<Position>(unit);
    require(position && position->x == 1);

    require(game.step(1));
    position = game.base().simulation().world().try_get<Position>(unit);
    require(position && position->x == 1);
    require(game.step(2));
    position = game.base().simulation().world().try_get<Position>(unit);
    require(position && position->x > 1);
}

void testSquadAiAndArchiveContinuity() {
    RtsG3GameSession game(20, 8);
    registerCore(game);
    const auto first = game.base().createUnit({1, 1}, {1}, 1, soldierCombat());
    const auto second = game.base().createUnit({1, 2}, {1}, 1, soldierCombat());
    const auto enemy = game.base().createUnit({8, 1}, {0}, 2, soldierCombat());

    SquadState squad;
    squad.teamId = 1;
    squad.formation = FormationKind::Line;
    squad.objectiveKind = SquadObjectiveKind::Assault;
    squad.objective = {12, 1};
    squad.retreatPoint = {0, 1};
    squad.thinkIntervalTicks = 1;
    squad.members = {second, first};
    require(game.createSquad(squad) != 0);

    require(game.step(0));
    require(game.base().simulation().commandStreamState().pending.size() >= 2u);
    const auto hash = game.authoritativeHash();
    const auto bytes = game.encode();
    require(!bytes.empty());

    RtsG3GameSession restored(20, 8);
    registerCore(restored);
    require(restored.decode(bytes));
    require(restored.authoritativeHash() == hash);
    require(restored.encode() == bytes);

    require(game.step(1));
    require(restored.step(1));
    require(restored.authoritativeHash() == game.authoritativeHash());
    (void)enemy;
}

} // namespace

int main() {
    testProjectileWeaponIsDelayed();
    testAbilityStatusAndCooldown();
    testStunPausesMovementAndCombat();
    testSquadAiAndArchiveContinuity();
    std::cout << "Engine G3 tests passed\n";
    return 0;
}
