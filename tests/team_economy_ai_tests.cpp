#include <RTSEngine/Rts/AiCommander.h>
#include <RTSEngine/Rts/Simulation.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace rts;
using namespace rts::gameplay;

void require(bool condition) {
    if (!condition) std::abort();
}

const TeamEconomySnapshot& economy(
    const WorldSnapshot& snapshot,
    std::uint32_t teamId) {
    const auto found = std::find_if(
        snapshot.teamEconomies.begin(),
        snapshot.teamEconomies.end(),
        [teamId](const TeamEconomySnapshot& value) {
            return value.teamId == teamId;
        });
    require(found != snapshot.teamEconomies.end());
    return *found;
}

const DomainEvent* rejection(
    const std::vector<DomainEvent>& events,
    CommandRejectionReason reason) {
    const auto found = std::find_if(
        events.begin(), events.end(),
        [reason](const DomainEvent& event) {
            return event.type == DomainEventType::CommandRejected &&
                   event.reason == static_cast<std::uint32_t>(reason);
        });
    return found == events.end() ? nullptr : &*found;
}

void testTeamEconomyRuntime() {
    TeamEconomyRuntime economies;
    require(economies.setResources(2, 75));
    require(economies.setResources(1, 100));
    require(economies.accounts().size() == 2);
    require(economies.accounts()[0].teamId == 1);
    require(economies.accounts()[1].teamId == 2);

    require(economies.reserveResources(1, 30));
    require(economies.resources(1).available == 70);
    require(economies.resources(2).available == 75);
    require(economies.commitResources(1, 10));
    require(economies.releaseResources(1, 20));
    require(economies.resources(1).available == 90);
    require(economies.resources(1).spent == 10);

    economies.beginSupplyRebuild();
    require(economies.addSupplyCapacity(1, 5));
    require(economies.reserveSupply(1, 3));
    require(!economies.reserveSupply(1, 3));
    require(economies.commitSupply(1, 2));
    require(economies.releaseReservedSupply(1, 1));
    const auto* account = economies.find(1);
    require(account != nullptr);
    require(account->supplyUsed == 2);
    require(account->supplyReserved == 0);
    require(account->supplyCapacity == 5);
}

BuildingDefinition producerDefinition() {
    BuildingDefinition definition;
    definition.id = 10;
    definition.cost = 0;
    definition.buildTicks = 1;
    definition.width = 2;
    definition.height = 2;
    definition.producer = true;
    definition.supplyProvided = 4;
    definition.productionQueueCapacity = 1;
    definition.trainableUnits = {1, 3};
    return definition;
}

UnitDefinition unitDefinition(
    std::uint32_t id,
    std::int32_t cost,
    std::uint32_t supply,
    std::uint32_t ticks = 2) {
    UnitDefinition definition;
    definition.id = id;
    definition.cost = cost;
    definition.supplyCost = supply;
    definition.trainTicks = ticks;
    definition.cellsPerTick = 1;
    return definition;
}

TickCommand buildCommand(std::uint64_t tick) {
    TickCommand command;
    command.targetTick = tick;
    command.issuer = 1;
    command.sequence = 1;
    command.type = CommandType::Build;
    command.definitionId = 10;
    command.targetX = 2;
    command.targetY = 2;
    return command;
}

TickCommand trainCommand(
    std::uint64_t tick,
    std::uint32_t sequence,
    ecs::Entity building,
    std::uint32_t definitionId) {
    TickCommand command;
    command.targetTick = tick;
    command.issuer = 1;
    command.sequence = sequence;
    command.type = CommandType::Train;
    command.subject = building;
    command.definitionId = definitionId;
    return command;
}

void testProductionSupplyAndRules() {
    RtsSimulation simulation(16, 12);
    simulation.setResources(100);
    require(simulation.registerBuilding(producerDefinition()));
    require(simulation.registerUnit(unitDefinition(1, 10, 2)));
    require(simulation.registerUnit(unitDefinition(2, 5, 1)));
    require(simulation.registerUnit(unitDefinition(3, 5, 8)));

    require(simulation.submit(buildCommand(0)));
    require(simulation.step(0));
    const auto producer = std::find_if(
        simulation.snapshot().entities.begin(),
        simulation.snapshot().entities.end(),
        [](const SnapshotEntity& entity) {
            return entity.kind == SnapshotKind::Building &&
                   entity.definitionId == 10;
        });
    require(producer != simulation.snapshot().entities.end());
    const auto producerEntity = producer->entity;
    const auto& initialEconomy = economy(simulation.snapshot(), 1);
    require(initialEconomy.supplyCapacity == 4);
    require(initialEconomy.supplyUsed == 0);

    require(simulation.submit(trainCommand(1, 2, producerEntity, 2)));
    require(simulation.step(1));
    require(rejection(
        simulation.events(),
        CommandRejectionReason::UnsupportedUnit) != nullptr);

    require(simulation.submit(trainCommand(2, 3, producerEntity, 3)));
    require(simulation.step(2));
    require(rejection(
        simulation.events(),
        CommandRejectionReason::SupplyBlocked) != nullptr);

    require(simulation.submit(trainCommand(3, 4, producerEntity, 1)));
    require(simulation.step(3));
    const auto& reserved = economy(simulation.snapshot(), 1);
    require(reserved.resources.available == 90);
    require(reserved.resources.reserved == 10);
    require(reserved.supplyReserved == 2);

    require(simulation.submit(trainCommand(4, 5, producerEntity, 1)));
    require(simulation.step(4));
    require(rejection(
        simulation.events(),
        CommandRejectionReason::QueueFull) != nullptr);
    const auto& completed = economy(simulation.snapshot(), 1);
    require(completed.resources.reserved == 0);
    require(completed.resources.spent == 10);
    require(completed.supplyReserved == 0);
    require(completed.supplyUsed == 2);

    const auto unit = std::find_if(
        simulation.snapshot().entities.begin(),
        simulation.snapshot().entities.end(),
        [producerEntity](const SnapshotEntity& entity) {
            return entity.kind == SnapshotKind::Unit &&
                   entity.entity != producerEntity &&
                   entity.supplyCost == 2;
        });
    require(unit != simulation.snapshot().entities.end());
}

void testBountyRoutesToKillerTeam() {
    RtsSimulation simulation(8, 4);
    simulation.setResources(0);
    require(simulation.setTeamResources(2, 20));
    CombatStats attacker;
    attacker.maximumHealth = 100;
    attacker.weaponDamage = 100;
    attacker.weaponRange = 1;
    attacker.cooldownTicks = 1;
    CombatStats victim;
    victim.maximumHealth = 10;
    victim.bounty = 7;

    const auto first = simulation.createUnit({1, 1}, {1}, 1, attacker, 4);
    const auto second = simulation.createUnit({2, 1}, {1}, 2, victim, 4);
    TickCommand attack;
    attack.targetTick = 0;
    attack.issuer = 1;
    attack.sequence = 1;
    attack.type = CommandType::Attack;
    attack.subject = first;
    attack.targetEntity = second;
    require(simulation.submit(attack));
    require(simulation.step(0));
    require(simulation.resources().available == 7);
    require(simulation.economies().resources(2).available == 20);
}

WorldSnapshot aiSnapshot(bool enemyVisible) {
    WorldSnapshot snapshot;
    snapshot.tick = 4;
    snapshot.visibilityWidth = 6;
    snapshot.visibilityHeight = 4;
    snapshot.teamEconomies.push_back(
        {1, ResourceLedger{100, 0, 0}, 0, 0, 8});

    TeamVisibilitySnapshot visibility;
    visibility.teamId = 1;
    visibility.current.assign(24, 0);
    visibility.explored.assign(24, 0);
    if (enemyVisible) {
        visibility.current[2 * 6 + 4] = 1;
    }
    snapshot.visibility.push_back(std::move(visibility));

    SnapshotEntity building;
    building.entity = {1, 1};
    building.kind = SnapshotKind::Building;
    building.teamId = 1;
    building.productionQueueSize = 0;
    building.healthCurrent = 100;
    snapshot.entities.push_back(building);

    SnapshotEntity first;
    first.entity = {2, 1};
    first.kind = SnapshotKind::Unit;
    first.teamId = 1;
    first.healthCurrent = 10;
    snapshot.entities.push_back(first);

    SnapshotEntity second = first;
    second.entity = {3, 1};
    snapshot.entities.push_back(second);

    SnapshotEntity enemy;
    enemy.entity = {4, 1};
    enemy.kind = SnapshotKind::Unit;
    enemy.teamId = 2;
    enemy.x = 4;
    enemy.y = 2;
    enemy.healthCurrent = 10;
    snapshot.entities.push_back(enemy);
    return snapshot;
}

void testDeterministicAiCommands() {
    AiCommanderConfig config;
    config.teamId = 1;
    config.trainUnitDefinitionId = 5;
    config.attackGoal = {5, 3};
    config.thinkIntervalTicks = 2;
    config.maximumUnitOrdersPerThink = 2;
    config.maximumProducerQueue = 1;
    config.minimumResourcesToTrain = 10;

    DeterministicAiCommander commander(config);
    std::vector<TickCommand> commands;
    require(commander.emit(aiSnapshot(false), 5, commands));
    require(commands.size() == 3);
    require(commands[0].type == CommandType::Train);
    require(commands[1].type == CommandType::AttackMove);
    require(commands[2].type == CommandType::AttackMove);
    require(commands[0].sequence == 1);
    require(commands[2].sequence == 3);

    const auto saved = commander.state();
    require(!commander.emit(aiSnapshot(true), 6, commands));
    require(commands.empty());
    require(commander.emit(aiSnapshot(true), 7, commands));
    require(commands.size() == 3);
    require(commands[1].type == CommandType::Attack);
    require(commands[1].targetEntity == ecs::Entity{4, 1});

    DeterministicAiCommander restored(config);
    require(restored.restore(saved));
    std::vector<TickCommand> replayed;
    require(restored.emit(aiSnapshot(true), 7, replayed));
    require(replayed.size() == commands.size());
    for (std::size_t index = 0; index < commands.size(); ++index) {
        require(replayed[index].targetTick == commands[index].targetTick);
        require(replayed[index].issuer == commands[index].issuer);
        require(replayed[index].sequence == commands[index].sequence);
        require(replayed[index].type == commands[index].type);
        require(replayed[index].subject == commands[index].subject);
        require(replayed[index].targetEntity == commands[index].targetEntity);
    }
}

} // namespace

int main() {
    testTeamEconomyRuntime();
    testProductionSupplyAndRules();
    testBountyRoutesToKillerTeam();
    testDeterministicAiCommands();
    std::cout << "team economy and AI tests passed\n";
    return EXIT_SUCCESS;
}
