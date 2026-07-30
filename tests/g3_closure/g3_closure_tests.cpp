#include <RTSEngine/Rts/G3Content.h>
#include <RTSEngine/Rts/G3Lockstep.h>
#include <RTSEngine/RtsPresentation/G3PresentationBridge.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace rts;
using namespace rts::gameplay;

#define require(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "G3 closure assertion failed at line " << __LINE__  \
                      << ": " #condition "\n";                               \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

CombatStats combat(std::int32_t health = 100) {
    CombatStats value;
    value.maximumHealth = health;
    value.armor = 0;
    value.weaponDamage = 0;
    value.weaponRange = 0;
    value.cooldownTicks = 1;
    value.bounty = 0;
    return value;
}

G3ContentBundle contentBundle() {
    G3ContentBundle content;

    StatusEffectDefinition slow;
    slow.id = 11;
    slow.durationTicks = 8;
    slow.maxStacks = 2;
    slow.stacking = StatusStackingPolicy::AddStacks;
    slow.moveScalePermille = 600;
    content.statuses.push_back(slow);

    ProjectileDefinition bolt;
    bolt.id = 21;
    bolt.speedQ16 =
        static_cast<std::uint32_t>(
            FixedPosition2D::kOne / 4);
    bolt.lifetimeTicks = 40;
    bolt.hitRadiusQ16 =
        static_cast<std::uint32_t>(
            FixedPosition2D::kOne / 8);
    bolt.damage = 9;
    bolt.statusEffectId = slow.id;
    content.projectiles.push_back(bolt);

    AbilityDefinition strike;
    strike.id = 31;
    strike.cooldownTicks = 5;
    strike.castTicks = 0;
    strike.rangeQ16 =
        static_cast<std::uint32_t>(
            FixedPosition2D::kOne * 12);
    strike.targetKind = AbilityTargetKind::Entity;
    strike.targetEnemies = true;
    AbilityEffectDefinition effect;
    effect.kind = AbilityEffectKind::SpawnProjectile;
    effect.projectileDefinitionId = bolt.id;
    strike.effects.push_back(effect);
    content.abilities.push_back(strike);

    content.unitProjectileBindings.push_back({1001, bolt.id});
    content.buildingProjectileBindings.push_back({2001, bolt.id});
    return content;
}

void testContentCodecAndCookedEnvelope() {
    auto content = contentBundle();
    require(G3ContentCodec::validate(content));
    const auto hash = G3ContentCodec::canonicalHash(content);
    require(hash != 0);

    const auto bytes = G3ContentCodec::encode(content);
    require(!bytes.empty());
    G3ContentBundle decoded;
    require(G3ContentCodec::decode(bytes, decoded));
    require(G3ContentCodec::canonicalHash(decoded) == hash);

    auto cooked = G3ContentCodec::cookedAsset(7001, content);
    require(cooked.key.valid());
    require(cooked.key.type == assets::AssetType::Binary);
    require(cooked.schemaVersion ==
            G3ContentCodec::kSchemaVersion);
    G3ContentBundle cookedDecoded;
    require(G3ContentCodec::decodeCookedAsset(
        cooked, cookedDecoded));
    require(G3ContentCodec::canonicalHash(cookedDecoded) == hash);

    RtsG3GameSession game(32, 16);
    require(G3ContentCodec::apply(game, decoded));
}

struct CombatFixture final {
    RtsG3GameSession game{32, 16};
    ecs::Entity caster{};
    ecs::Entity target{};

    CombatFixture() {
        AbilityDefinition ability;
        ability.id = 101;
        ability.cooldownTicks = 2;
        ability.castTicks = 0;
        ability.rangeQ16 =
            static_cast<std::uint32_t>(
                FixedPosition2D::kOne * 16);
        ability.targetKind = AbilityTargetKind::Entity;
        ability.targetEnemies = true;
        AbilityEffectDefinition effect;
        effect.kind = AbilityEffectKind::Damage;
        effect.amount = 12;
        effect.damageType = G3DamageType::TrueDamage;
        ability.effects.push_back(effect);
        require(game.registerAbility(ability));

        caster = game.base().createUnit(
            {2, 2}, {1}, 1, combat());
        target = game.base().createUnit(
            {8, 2}, {1}, 2, combat());
        require(caster.valid() && target.valid());
        require(game.grantAbility(caster, ability.id));
    }
};

void testAbilityLockstepAndReconnect() {
    CombatFixture first;
    RtsG3LockstepConfig config;
    config.sessionId = 0xabcdu;
    config.inputDelayTicks = 0;
    config.maximumPredictionTicks = 0;
    config.checkpointIntervalTicks = 1;
    config.checkpointCapacity = 8;
    config.hashExchangeIntervalTicks = 1;

    RtsG3LockstepSession lockstep(first.game, config);
    sim::LockstepPeer peer;
    peer.peerId = 1;
    peer.playerSlot = 1;
    peer.issuer = 1;
    peer.role = sim::LockstepPeerRole::Player;
    peer.active = true;
    require(lockstep.registerPeer(peer));
    require(lockstep.start() ==
            RtsG3LockstepStartResult::Started);

    AbilityCommand ability;
    ability.caster = first.caster;
    ability.abilityId = 101;
    ability.targetEntity = first.target;

    RtsG3LockstepFrame frame;
    require(lockstep.buildLocalFrame(
        peer.peerId,
        {G3NetworkCommand::fromAbility(ability)},
        frame));
    require(lockstep.receiveFrame(frame) ==
            sim::LockstepFrameSubmitResult::Accepted);
    require(lockstep.advanceOne() ==
            RtsG3LockstepAdvanceResult::Advanced);

    const auto* health =
        first.game.base().simulation().world().
            try_get<Health>(first.target);
    require(health && health->current == 88);
    require(lockstep.hashReportDue(0));

    sim::StateHashReport report;
    require(lockstep.makeHashReport(
        peer.peerId, 0, report));
    require(report.authoritativeHash ==
            first.game.authoritativeHash());

    RtsG3ReconnectSnapshot reconnect;
    require(lockstep.makeReconnectSnapshot(reconnect));

    CombatFixture restored;
    RtsG3LockstepSession restoredLockstep(
        restored.game, config);
    require(restoredLockstep.restoreReconnectSnapshot(
        reconnect));
    require(restored.game.authoritativeHash() ==
            first.game.authoritativeHash());
    require(restored.game.base().simulation().
                nextExpectedTick() == 1);
}

void testPresentationBridge() {
    RtsG3GameSession game(32, 16);

    StatusEffectDefinition status;
    status.id = 201;
    status.durationTicks = 10;
    status.maxStacks = 1;
    require(game.registerStatusEffect(status));

    ProjectileDefinition projectile;
    projectile.id = 202;
    projectile.speedQ16 =
        static_cast<std::uint32_t>(
            FixedPosition2D::kOne / 4);
    projectile.lifetimeTicks = 40;
    projectile.damage = 3;
    require(game.registerProjectile(projectile));

    AbilityDefinition ability;
    ability.id = 203;
    ability.cooldownTicks = 3;
    ability.castTicks = 0;
    ability.rangeQ16 =
        static_cast<std::uint32_t>(
            FixedPosition2D::kOne * 16);
    ability.targetKind = AbilityTargetKind::Entity;
    ability.targetEnemies = true;
    AbilityEffectDefinition spawn;
    spawn.kind = AbilityEffectKind::SpawnProjectile;
    spawn.projectileDefinitionId = projectile.id;
    ability.effects.push_back(spawn);
    require(game.registerAbility(ability));

    const auto caster = game.base().createUnit(
        {2, 4}, {1}, 1, combat());
    const auto target = game.base().createUnit(
        {12, 4}, {1}, 2, combat());
    require(caster.valid() && target.valid());
    require(game.grantAbility(caster, ability.id));
    require(game.applyStatus(caster, status.id, caster));

    AbilityCommand command;
    command.targetTick = 0;
    command.issuer = 1;
    command.sequence = 1;
    command.caster = caster;
    command.abilityId = ability.id;
    command.targetEntity = target;
    require(game.submitAbility(command) ==
            AbilitySubmitResult::Accepted);
    require(game.step(0));

    const auto frame =
        rts_presentation::G3PresentationBridge::extract(
            game, 1);
    require(frame.tick == 0);
    require(!frame.projectiles.empty());
    require(!frame.events.empty());
    require(!frame.telegraphs.empty());
    require(!frame.statuses.empty());
    require(frame.projectiles.front().kind ==
            presentation::SceneEntityKind::Projectile);
    require(frame.telegraphs.front().abilityId ==
            ability.id);
    require(frame.statuses.front().statusId ==
            status.id);
}

} // namespace

int main() {
    testContentCodecAndCookedEnvelope();
    testAbilityLockstepAndReconnect();
    testPresentationBridge();
    return 0;
}
