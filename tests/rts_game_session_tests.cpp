#include <RTSEngine/Rts/RtsGameSession.h>

#include <cstdlib>
#include <iostream>

namespace {

using namespace rts::gameplay;
namespace ecs = rts::ecs;

void require(bool condition) {
    if (!condition) std::abort();
}

CombatStats combatProfile() {
    CombatStats result;
    result.maximumHealth = 20;
    result.weaponDamage = 4;
    result.weaponRange = 8;
    result.cooldownTicks = 1;
    return result;
}

void testDiplomacyNormalization() {
    DiplomacyRuntime diplomacy;
    require(diplomacy.hostile(1, 2));
    require(diplomacy.setRelation(2, 1, DiplomaticRelation::Allied));
    require(diplomacy.allied(1, 2));
    require(diplomacy.entries().size() == 1);
    require(diplomacy.entries().front().firstTeam == 1);
    require(diplomacy.entries().front().secondTeam == 2);
    require(diplomacy.relation(0, 2) == DiplomaticRelation::Neutral);
    require(!diplomacy.setRelation(1, 1, DiplomaticRelation::Hostile));
}

void testAlliedAttackAndAiFallback() {
    RtsGameSession session(16, 8);
    const auto first = session.createUnit(
        {2, 3}, {1}, 1, combatProfile(), 16);
    const auto second = session.createUnit(
        {10, 3}, {1}, 2, combatProfile(), 16);
    require(first.valid() && second.valid());
    require(session.setRelation(1, 2, DiplomaticRelation::Allied));
    require(session.registerAiTeam(2, {0, 3}, 1));

    TickCommand attack;
    attack.targetTick = 0;
    attack.issuer = 1;
    attack.sequence = 1;
    attack.type = CommandType::Attack;
    attack.subject = first;
    attack.targetEntity = second;
    require(session.submitDetailed(attack) == SessionCommandResult::AlliedTarget);

    require(session.step(0));
    const auto pending = session.simulation().commandStreamState().pending;
    require(pending.size() == 1);
    require(pending.front().targetTick == 1);
    require(pending.front().issuer == 2);
    require(pending.front().subject == second);
    require(pending.front().type == CommandType::AttackMove);
    require(pending.front().targetX == 0 && pending.front().targetY == 3);
}

void testAiAttacksVisibleHostileTarget() {
    RtsGameSession session(16, 8);
    const auto first = session.createUnit(
        {2, 3}, {1}, 1, combatProfile(), 16);
    const auto second = session.createUnit(
        {10, 3}, {1}, 2, combatProfile(), 16);
    require(first.valid() && second.valid());
    require(session.registerAiTeam(2, {0, 3}, 1));

    require(session.step(0));
    const auto pending = session.simulation().commandStreamState().pending;
    require(pending.size() == 1);
    require(pending.front().type == CommandType::Attack);
    require(pending.front().subject == second);
    require(pending.front().targetEntity == first);
    require(session.step(1));

    bool accepted = false;
    for (const auto& event : session.simulation().events()) {
        if (event.type == DomainEventType::AttackAccepted &&
            event.entity == second && event.secondary == first) {
            accepted = true;
        }
    }
    require(accepted);
}

struct ProducerFixture final {
    RtsGameSession session{16, 8};
    ecs::Entity producer{};

    ProducerFixture(std::uint32_t queueCapacity, std::uint32_t supplyCapacity) {
        BuildingDefinition building;
        building.id = 20;
        building.cost = 0;
        building.buildTicks = 1;
        building.producer = true;
        require(session.registerBuilding(building));

        UnitDefinition unit;
        unit.id = 10;
        unit.cost = 0;
        unit.trainTicks = 2;
        unit.cellsPerTick = 1;
        require(session.registerUnit(unit));

        UnitDefinition blocked = unit;
        blocked.id = 11;
        require(session.registerUnit(blocked));

        require(session.registerProducerPolicy(
            ProducerPolicy{20, queueCapacity, {10}}));
        require(session.setTeamSupplyCapacity(1, supplyCapacity));
        session.setResources(100);

        TickCommand build;
        build.targetTick = 0;
        build.issuer = 1;
        build.sequence = 1;
        build.type = CommandType::Build;
        build.definitionId = 20;
        build.targetX = 3;
        build.targetY = 3;
        require(session.submit(build));
        require(session.step(0));

        for (const auto& entity : session.simulation().snapshot().entities) {
            if (entity.kind == SnapshotKind::Building &&
                entity.definitionId == 20) {
                producer = entity.entity;
                break;
            }
        }
        require(producer.valid());
    }

    TickCommand train(std::uint32_t sequence, std::uint32_t definitionId = 10) {
        TickCommand command;
        command.targetTick = 1;
        command.issuer = 1;
        command.sequence = sequence;
        command.type = CommandType::Train;
        command.subject = producer;
        command.definitionId = definitionId;
        return command;
    }
};

void testProducerPolicyAndQueueReservation() {
    ProducerFixture fixture(1, 10);
    require(fixture.session.submitDetailed(fixture.train(2, 11)) ==
            SessionCommandResult::ProducerRestricted);
    require(fixture.session.submitDetailed(fixture.train(3)) ==
            SessionCommandResult::Accepted);
    require(fixture.session.pendingTrainReservations() == 1);
    require(fixture.session.submitDetailed(fixture.train(3)) ==
            SessionCommandResult::Accepted);
    require(fixture.session.pendingTrainReservations() == 1);
    require(fixture.session.submitDetailed(fixture.train(4)) ==
            SessionCommandResult::QueueFull);
    require(fixture.session.step(1));
    require(fixture.session.pendingTrainReservations() == 0);
    require(fixture.session.usedSupply(1) == 1);
}

void testSupplyReservationAcrossSameTick() {
    ProducerFixture fixture(4, 1);
    require(fixture.session.submitDetailed(fixture.train(2)) ==
            SessionCommandResult::Accepted);
    require(fixture.session.usedSupply(1) == 1);
    require(fixture.session.submitDetailed(fixture.train(3)) ==
            SessionCommandResult::SupplyBlocked);
}

} // namespace

int main() {
    testDiplomacyNormalization();
    testAlliedAttackAndAiFallback();
    testAiAttacksVisibleHostileTarget();
    testProducerPolicyAndQueueReservation();
    testSupplyReservationAcrossSameTick();
    std::cout << "RTS game session tests passed\n";
    return EXIT_SUCCESS;
}
