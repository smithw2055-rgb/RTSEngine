#include <RTSEngine/Rts/SimulationArchive.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            std::cerr << "CHECK failed: " #expression \
                      << " at line " << __LINE__ << '\n'; \
            return EXIT_FAILURE; \
        } \
    } while (false)

using namespace rts;
using namespace rts::gameplay;

void registerContent(RtsSimulation& simulation, std::int32_t unitCost = 40) {
    BuildingDefinition building;
    building.id = 1;
    building.cost = 100;
    building.buildTicks = 3;
    building.width = 2;
    building.height = 2;
    building.producer = true;
    building.combat.maximumHealth = 80;
    building.combat.armor = 1;
    simulation.registerBuilding(building);

    UnitDefinition unit;
    unit.id = 1;
    unit.cost = unitCost;
    unit.trainTicks = 4;
    unit.cellsPerTick = 1;
    unit.combat.maximumHealth = 20;
    unit.combat.weaponDamage = 4;
    unit.combat.weaponRange = 2;
    unit.combat.cooldownTicks = 2;
    unit.combat.bounty = 3;
    simulation.registerUnit(unit);
}

TickCommand command(
    std::uint64_t tick,
    std::uint32_t sequence,
    CommandType type) {
    TickCommand value;
    value.targetTick = tick;
    value.issuer = 1;
    value.sequence = sequence;
    value.type = type;
    return value;
}

} // namespace

int main() {
    RtsSimulation original(16, 10);
    registerContent(original);
    original.setResources(700);
    original.setRequiredRoute({0, 5}, {15, 5});
    original.setPlayerTeam(1);
    CHECK(original.setBlocked({7, 7}, true));

    TeamModifierProfile profile;
    profile.unitHealth = 1250;
    profile.unitDamage = 1500;
    profile.unitMoveSpeed = 1000;
    profile.constructionSpeed = 1000;
    profile.productionSpeed = 1000;
    profile.bountyMultiplier = 1200;
    CHECK(original.setTeamModifierProfile(1, profile));

    CombatStats defenderStats;
    defenderStats.maximumHealth = 24;
    defenderStats.armor = 1;
    defenderStats.weaponDamage = 5;
    defenderStats.weaponRange = 2;
    defenderStats.cooldownTicks = 2;
    const auto defender = original.createUnit(
        {1, 1}, {1}, 1, defenderStats);

    CombatStats enemyStats;
    enemyStats.maximumHealth = 30;
    enemyStats.weaponDamage = 2;
    enemyStats.weaponRange = 1;
    enemyStats.cooldownTicks = 3;
    enemyStats.bounty = 5;
    original.createUnit({13, 1}, {0}, 2, enemyStats);

    auto build = command(0, 1, CommandType::Build);
    build.definitionId = 1;
    build.targetX = 3;
    build.targetY = 1;
    CHECK(original.submit(build));

    auto move = command(0, 2, CommandType::Move);
    move.subject = defender;
    move.targetX = 9;
    move.targetY = 1;
    CHECK(original.submit(move));

    original.step(0);
    original.step(1);
    original.step(2);

    const auto buildingIterator = std::find_if(
        original.snapshot().entities.begin(),
        original.snapshot().entities.end(),
        [](const SnapshotEntity& entity) {
            return entity.kind == SnapshotKind::Building &&
                   entity.definitionId == 1;
        });
    CHECK(buildingIterator != original.snapshot().entities.end());
    const auto producer = buildingIterator->entity;

    auto rally = command(3, 3, CommandType::SetRally);
    rally.subject = producer;
    rally.targetX = 11;
    rally.targetY = 7;
    CHECK(original.submit(rally));

    auto train = command(3, 4, CommandType::Train);
    train.subject = producer;
    train.definitionId = 1;
    CHECK(original.submit(train));

    auto secondBuild = command(5, 5, CommandType::Build);
    secondBuild.definitionId = 1;
    secondBuild.targetX = 8;
    secondBuild.targetY = 2;
    CHECK(original.submit(secondBuild));

    auto secondMove = command(6, 6, CommandType::Move);
    secondMove.subject = defender;
    secondMove.targetX = 10;
    secondMove.targetY = 4;
    CHECK(original.submit(secondMove));

    auto secondTrain = command(7, 7, CommandType::Train);
    secondTrain.subject = producer;
    secondTrain.definitionId = 1;
    CHECK(original.submit(secondTrain));

    original.step(3);
    const auto savedHash = original.snapshot().worldHash;
    const auto archive = EncodeRtsSimulation(original);
    CHECK(!archive.empty());

    RtsSimulation restored(16, 10);
    registerContent(restored);
    CHECK(DecodeRtsSimulation(archive, restored));
    CHECK(restored.lastCompletedTick() == 3);
    CHECK(restored.snapshot().worldHash == savedHash);
    CHECK(restored.resources().available == original.resources().available);
    CHECK(restored.resources().reserved == original.resources().reserved);
    CHECK(restored.navigation().revision() == original.navigation().revision());
    CHECK(EncodeRtsSimulation(restored) == archive);

    for (std::uint64_t tick = 4; tick <= 13; ++tick) {
        original.step(tick);
        restored.step(tick);
        CHECK(restored.snapshot().worldHash == original.snapshot().worldHash);
        CHECK(restored.resources().available == original.resources().available);
        CHECK(restored.resources().reserved == original.resources().reserved);
        CHECK(restored.resources().spent == original.resources().spent);
        CHECK(restored.navigation().revision() == original.navigation().revision());
    }

    CHECK(EncodeRtsSimulation(restored) == EncodeRtsSimulation(original));

    RtsSimulation incompatible(16, 10);
    registerContent(incompatible, 41);
    const auto incompatibleSentinel = incompatible.createUnit({1, 8}, {1});
    CHECK(!DecodeRtsSimulation(archive, incompatible));
    CHECK(incompatible.world().alive(incompatibleSentinel));

    auto truncated = archive;
    truncated.pop_back();
    RtsSimulation preserved(16, 10);
    registerContent(preserved);
    const auto preservedSentinel = preserved.createUnit({2, 8}, {1});
    CHECK(!DecodeRtsSimulation(truncated, preserved));
    CHECK(preserved.world().alive(preservedSentinel));

    RtsSimulation wrongDimensions(15, 10);
    registerContent(wrongDimensions);
    CHECK(!DecodeRtsSimulation(archive, wrongDimensions));

    std::cout << "rts simulation persistence tests passed\n";
    return EXIT_SUCCESS;
}
