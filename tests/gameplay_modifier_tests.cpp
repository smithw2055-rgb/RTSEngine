#include <RTSEngine/Rts/Simulation.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace rts;
using namespace rts::gameplay;

TeamModifierProfile upgradedProfile() {
    TeamModifierProfile profile;
    profile.unitHealth = 1500;
    profile.unitDamage = 2000;
    profile.unitArmorAdd = 2;
    profile.unitMoveSpeed = 1500;
    profile.buildingHealth = 2000;
    profile.buildingDamage = 1500;
    profile.constructionSpeed = 2000;
    profile.productionSpeed = 2500;
    profile.bountyMultiplier = 2000;
    return profile;
}

void assertUpgradedUnit(const RtsSimulation& simulation,
                        ecs::Entity entity) {
    const auto* health = simulation.world().try_get<Health>(entity);
    const auto* armor = simulation.world().try_get<Armor>(entity);
    const auto* weapon = simulation.world().try_get<Weapon>(entity);
    const auto* speed = simulation.world().try_get<MoveSpeed>(entity);
    assert(health && health->current == 15 && health->maximum == 15);
    assert(armor && armor->value == 3);
    assert(weapon && weapon->damage == 8);
    assert(speed && speed->cellsPerTick == 3);
}

void testExistingAndFutureUnits() {
    RtsSimulation simulation(12, 8);
    const CombatStats base{10, 1, 4, 3, 2, 0};
    const auto existing = simulation.createUnit({1, 1}, {2}, 1, base);

    simulation.setTeamModifierProfile(1, upgradedProfile());
    assertUpgradedUnit(simulation, existing);

    const auto future = simulation.createUnit({2, 1}, {2}, 1, base);
    assertUpgradedUnit(simulation, future);
    assert(simulation.snapshot().teamModifiers.empty());

    simulation.step(0);
    assert(simulation.snapshot().teamModifiers.size() == 1);
    assert(simulation.snapshot().teamModifiers.front().teamId == 1);
}

void testConstructionAndProductionDurations() {
    RtsSimulation simulation(12, 8);
    simulation.setResources(100);
    simulation.setRequiredRoute({0, 0}, {0, 0});
    simulation.setTeamModifierProfile(1, upgradedProfile());

    BuildingDefinition factory;
    factory.id = 10;
    factory.cost = 0;
    factory.buildTicks = 10;
    factory.width = 1;
    factory.height = 1;
    factory.producer = true;
    factory.combat = CombatStats{10, 0, 2, 4, 1, 0};
    simulation.registerBuilding(factory);

    UnitDefinition unit;
    unit.id = 20;
    unit.cost = 0;
    unit.trainTicks = 10;
    unit.cellsPerTick = 1;
    unit.combat = CombatStats{5, 0, 1, 1, 1, 0};
    simulation.registerUnit(unit);

    TickCommand build;
    build.targetTick = 0;
    build.issuer = 1;
    build.sequence = 1;
    build.type = CommandType::Build;
    build.definitionId = factory.id;
    build.targetX = 4;
    build.targetY = 2;
    assert(simulation.submit(build));
    simulation.step(0);

    const auto sites = simulation.world().view<ConstructionSite>();
    assert(sites.size() == 1);
    const auto* site = simulation.world().try_get<ConstructionSite>(sites.front());
    assert(site && site->baseRequiredTicks == 10);
    assert(site->requiredTicks == 5);

    for (std::uint64_t tick = 1; tick < 5; ++tick) {
        simulation.step(tick);
    }

    const auto factories =
        simulation.world().view<Building, ProductionQueue>();
    assert(factories.size() == 1);
    const auto factoryEntity = factories.front();
    const auto* health = simulation.world().try_get<Health>(factoryEntity);
    const auto* weapon = simulation.world().try_get<Weapon>(factoryEntity);
    assert(health && health->maximum == 20);
    assert(weapon && weapon->damage == 3);

    TickCommand train;
    train.targetTick = 5;
    train.issuer = 1;
    train.sequence = 2;
    train.type = CommandType::Train;
    train.subject = factoryEntity;
    train.definitionId = unit.id;
    assert(simulation.submit(train));
    simulation.step(5);

    const auto* queue =
        simulation.world().try_get<ProductionQueue>(factoryEntity);
    assert(queue && queue->items.size() == 1);
    assert(queue->items.front().baseRequiredTicks == 10);
    assert(queue->items.front().requiredTicks == 4);
}

void testExistingConstructionIsRetimed() {
    RtsSimulation simulation(8, 6);
    simulation.setResources(10);
    simulation.setRequiredRoute({0, 0}, {0, 0});

    BuildingDefinition building;
    building.id = 1;
    building.cost = 0;
    building.buildTicks = 10;
    simulation.registerBuilding(building);

    TickCommand command;
    command.targetTick = 0;
    command.issuer = 1;
    command.sequence = 1;
    command.type = CommandType::Build;
    command.definitionId = 1;
    command.targetX = 3;
    command.targetY = 3;
    assert(simulation.submit(command));
    simulation.step(0);

    auto sites = simulation.world().view<ConstructionSite>();
    assert(sites.size() == 1);
    assert(simulation.world().try_get<ConstructionSite>(sites.front())
               ->requiredTicks == 10);

    auto profile = upgradedProfile();
    simulation.setTeamModifierProfile(1, profile);
    assert(simulation.world().try_get<ConstructionSite>(sites.front())
               ->requiredTicks == 5);
}

std::vector<std::uint64_t> runBountyScenario() {
    RtsSimulation simulation(8, 6);
    simulation.setPlayerTeam(1);
    simulation.setTeamModifierProfile(1, upgradedProfile());
    simulation.createUnit(
        {1, 1}, {1}, 1, CombatStats{10, 0, 10, 6, 1, 0});
    simulation.createUnit(
        {3, 1}, {1}, 2, CombatStats{1, 0, 0, 0, 1, 5});

    std::vector<std::uint64_t> hashes;
    for (std::uint64_t tick = 0; tick < 3; ++tick) {
        simulation.step(tick);
        hashes.push_back(simulation.snapshot().worldHash);
    }
    assert(simulation.resources().available == 10);
    return hashes;
}

} // namespace

int main() {
    testExistingAndFutureUnits();
    testConstructionAndProductionDurations();
    testExistingConstructionIsRetimed();
    const auto first = runBountyScenario();
    const auto second = runBountyScenario();
    assert(first == second);
    std::cout << "gameplay modifier tests passed\n";
    return 0;
}
