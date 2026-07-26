#include <RTSEngine/Rts/Simulation.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace rts;
using namespace rts::gameplay;

bool hasEvent(const RtsSimulation& simulation, DomainEventType type) {
    return std::any_of(
        simulation.events().begin(),
        simulation.events().end(),
        [type](const DomainEvent& event) {
            return event.type == type;
        });
}

TickCommand makeAttack(std::uint64_t tick,
                       std::uint32_t sequence,
                       ecs::Entity subject,
                       ecs::Entity target) {
    TickCommand command;
    command.targetTick = tick;
    command.issuer = 1;
    command.sequence = sequence;
    command.type = CommandType::Attack;
    command.subject = subject;
    command.targetEntity = target;
    return command;
}

TickCommand makeAttackMove(std::uint64_t tick,
                           std::uint32_t sequence,
                           ecs::Entity subject,
                           GridPoint target) {
    TickCommand command;
    command.targetTick = tick;
    command.issuer = 1;
    command.sequence = sequence;
    command.type = CommandType::AttackMove;
    command.subject = subject;
    command.targetX = target.x;
    command.targetY = target.y;
    return command;
}

std::vector<std::uint64_t> runAttackScenario() {
    RtsSimulation simulation(16, 8);
    simulation.setPlayerTeam(1);

    const auto attacker = simulation.createUnit(
        {0, 1},
        {1},
        1,
        CombatStats{12, 0, 3, 1, 1, 0});
    const auto defender = simulation.createUnit(
        {6, 1},
        {1},
        2,
        CombatStats{6, 0, 0, 0, 1, 7});

    assert(simulation.submit(makeAttack(0, 1, attacker, defender)));

    bool accepted = false;
    bool fired = false;
    bool damaged = false;
    bool died = false;
    bool bounty = false;
    std::vector<std::uint64_t> hashes;
    for (std::uint64_t tick = 0; tick < 16; ++tick) {
        simulation.step(tick);
        hashes.push_back(simulation.snapshot().worldHash);
        accepted = accepted || hasEvent(simulation, DomainEventType::AttackAccepted);
        fired = fired || hasEvent(simulation, DomainEventType::WeaponFired);
        damaged = damaged || hasEvent(simulation, DomainEventType::DamageApplied);
        died = died || hasEvent(simulation, DomainEventType::EntityDied);
        bounty = bounty || hasEvent(simulation, DomainEventType::BountyAwarded);
    }

    assert(accepted);
    assert(fired);
    assert(damaged);
    assert(died);
    assert(bounty);
    assert(!simulation.world().alive(defender));
    assert(simulation.world().alive(attacker));
    assert(simulation.resources().available == 7);
    return hashes;
}

void testAttackMoveResumesAfterCombat() {
    RtsSimulation simulation(16, 8);
    const auto attacker = simulation.createUnit(
        {0, 0},
        {1},
        1,
        CombatStats{10, 0, 4, 1, 1, 0});
    const auto defender = simulation.createUnit(
        {3, 0},
        {1},
        2,
        CombatStats{4, 0, 0, 0, 1, 2});

    assert(simulation.submit(makeAttackMove(0, 1, attacker, {8, 0})));

    bool attackMoveAccepted = false;
    for (std::uint64_t tick = 0; tick < 20; ++tick) {
        simulation.step(tick);
        attackMoveAccepted =
            attackMoveAccepted ||
            hasEvent(simulation, DomainEventType::AttackMoveAccepted);
    }

    assert(attackMoveAccepted);
    assert(!simulation.world().alive(defender));
    const auto iterator = std::find_if(
        simulation.snapshot().entities.begin(),
        simulation.snapshot().entities.end(),
        [attacker](const SnapshotEntity& entity) {
            return entity.entity == attacker;
        });
    assert(iterator != simulation.snapshot().entities.end());
    assert(iterator->x == 8);
    assert(iterator->y == 0);
    assert(!iterator->moving);
    assert(iterator->combatMode == CombatMode::Guard);
}

void testHoldPositionDoesNotChase() {
    RtsSimulation simulation(16, 8);
    const auto defender = simulation.createUnit(
        {4, 4},
        {1},
        1,
        CombatStats{10, 0, 2, 2, 1, 0});
    simulation.createUnit(
        {8, 4},
        {1},
        2,
        CombatStats{10, 0, 0, 0, 1, 0});

    TickCommand hold;
    hold.targetTick = 0;
    hold.issuer = 1;
    hold.sequence = 1;
    hold.type = CommandType::HoldPosition;
    hold.subject = defender;
    assert(simulation.submit(hold));

    for (std::uint64_t tick = 0; tick < 8; ++tick) {
        simulation.step(tick);
    }

    const auto iterator = std::find_if(
        simulation.snapshot().entities.begin(),
        simulation.snapshot().entities.end(),
        [defender](const SnapshotEntity& entity) {
            return entity.entity == defender;
        });
    assert(iterator != simulation.snapshot().entities.end());
    assert(iterator->x == 4);
    assert(iterator->y == 4);
    assert(iterator->combatMode == CombatMode::HoldPosition);
}

void testBuildingDeathReleasesFootprint() {
    RtsSimulation simulation(10, 6);
    simulation.setResources(100);
    simulation.setRequiredRoute({0, 0}, {0, 0});

    BuildingDefinition tower;
    tower.id = 11;
    tower.cost = 0;
    tower.buildTicks = 1;
    tower.width = 1;
    tower.height = 1;
    tower.combat = CombatStats{4, 0, 0, 0, 1, 0};
    simulation.registerBuilding(tower);

    simulation.createUnit(
        {3, 2},
        {1},
        2,
        CombatStats{10, 0, 5, 1, 1, 0});

    TickCommand build;
    build.targetTick = 0;
    build.issuer = 1;
    build.sequence = 1;
    build.type = CommandType::Build;
    build.targetX = 4;
    build.targetY = 2;
    build.definitionId = tower.id;
    assert(simulation.submit(build));

    simulation.step(0);

    assert(hasEvent(simulation, DomainEventType::ConstructionAccepted));
    assert(hasEvent(simulation, DomainEventType::ConstructionCompleted));
    assert(hasEvent(simulation, DomainEventType::EntityDied));
    assert(!simulation.navigation().blocked({4, 2}));
    const auto building = std::find_if(
        simulation.snapshot().entities.begin(),
        simulation.snapshot().entities.end(),
        [](const SnapshotEntity& entity) {
            return entity.kind == SnapshotKind::Building;
        });
    assert(building == simulation.snapshot().entities.end());
}

} // namespace

int main() {
    const auto first = runAttackScenario();
    const auto second = runAttackScenario();
    assert(first == second);

    testAttackMoveResumesAfterCombat();
    testHoldPositionDoesNotChase();
    testBuildingDeathReleasesFootprint();

    std::cout << "rts combat integration tests passed\n";
    return 0;
}
