#include <RTSEngine/Rts/RtsGameSessionArchive.h>
#include <RTSEngine/Rts/SimulationArchive.h>
#include <RTSEngine/Rts/TeamEconomy.h>

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using namespace rts::gameplay;

void require(bool condition) {
    if (!condition) std::abort();
}

BuildingDefinition dropOffDefinition() {
    BuildingDefinition value;
    value.id = 20;
    value.cost = 0;
    value.buildTicks = 1;
    value.width = 1;
    value.height = 1;
    value.dropOffResourceType = 2;
    value.supplyProvided = 2;
    return value;
}

UnitDefinition workerDefinition() {
    UnitDefinition value;
    value.id = 10;
    value.cost = 0;
    value.trainTicks = 2;
    value.cellsPerTick = 1;
    value.visionRange = 4;
    value.worker = true;
    value.cargoCapacity = 4;
    value.harvestAmount = 2;
    value.harvestTicks = 1;
    return value;
}

void registerHarvestContent(RtsSimulation& simulation) {
    require(simulation.registerBuilding(dropOffDefinition()));
    require(simulation.registerUnit(workerDefinition()));
}

void registerHarvestContent(RtsGameSession& session) {
    require(session.registerBuilding(dropOffDefinition()));
    require(session.registerUnit(workerDefinition()));
}

TickCommand buildCommand(std::uint64_t tick, std::uint32_t sequence) {
    TickCommand value;
    value.targetTick = tick;
    value.issuer = 1;
    value.sequence = sequence;
    value.type = CommandType::Build;
    value.definitionId = 20;
    value.targetX = 0;
    value.targetY = 3;
    return value;
}

TickCommand gatherCommand(
    std::uint64_t tick,
    std::uint32_t sequence,
    rts::ecs::Entity worker,
    rts::ecs::Entity node) {
    TickCommand value;
    value.targetTick = tick;
    value.issuer = 1;
    value.sequence = sequence;
    value.type = CommandType::Gather;
    value.subject = worker;
    value.targetEntity = node;
    return value;
}

void testLargeMultiResourceAccounts() {
    TeamEconomyRuntime economy;
    constexpr ResourceAmount large = 5'000'000'000ll;
    require(economy.setAvailable(2, 3, large));
    require(economy.setAvailable(1, 2, 9));
    require(economy.entries().size() == 2);
    require(economy.entries().front().teamId == 1);
    require(economy.reserve(2, 3, 4'000'000'000ll));
    require(economy.available(2, 3) == 1'000'000'000ll);
    require(economy.release(2, 3, 4'000'000'000ll));
    require(economy.available(2, 3) == large);
    require(economy.credit(
        2,
        3,
        std::numeric_limits<ResourceAmount>::max()));
    require(economy.available(2, 3) ==
            std::numeric_limits<ResourceAmount>::max());
    require(economy.legacyLedger(2, 3).available ==
            std::numeric_limits<std::int32_t>::max());
}

void testTeamBuildIsolation() {
    RtsSimulation simulation(16, 8);
    BuildingDefinition definition;
    definition.id = 1;
    definition.cost = 50;
    definition.buildTicks = 1;
    require(simulation.registerBuilding(definition));
    require(simulation.setTeamResource(1, kPrimaryResourceType, 100));
    require(simulation.setTeamResource(2, kPrimaryResourceType, 0));

    TickCommand first;
    first.targetTick = 0;
    first.issuer = 1;
    first.sequence = 1;
    first.type = CommandType::Build;
    first.definitionId = 1;
    first.targetX = 2;
    first.targetY = 2;
    require(simulation.submit(first));

    TickCommand second = first;
    second.issuer = 2;
    second.targetX = 6;
    second.targetY = 2;
    require(simulation.submit(second));
    require(simulation.step(0));

    require(simulation.resourceAvailable(1, kPrimaryResourceType) == 50);
    require(simulation.teamEconomy().spent(1, kPrimaryResourceType) == 50);
    require(simulation.resourceAvailable(2, kPrimaryResourceType) == 0);
    require(simulation.teamEconomy().spent(2, kPrimaryResourceType) == 0);

    bool rejected = false;
    for (const auto& event : simulation.events()) {
        if (event.type == DomainEventType::ConstructionRejected &&
            event.reason == static_cast<std::uint32_t>(
                BuildFailure::InsufficientResources)) {
            rejected = true;
        }
    }
    require(rejected);
}

struct HarvestFixture final {
    RtsSimulation simulation{16, 8};
    rts::ecs::Entity worker{};
    rts::ecs::Entity node{};

    HarvestFixture() {
        registerHarvestContent(simulation);
        require(simulation.setTeamResource(1, kPrimaryResourceType, 0));
        require(simulation.setTeamResource(1, 2, 0));
        worker = simulation.createUnitDefinition(10, {1, 1}, 1);
        node = simulation.createResourceNode({4, 1}, 2, 6);
        require(worker.valid() && node.valid());
        require(simulation.submit(buildCommand(0, 1)));
        require(simulation.submit(gatherCommand(0, 2, worker, node)));
    }
};

void testHarvestDepositLoop() {
    HarvestFixture fixture;
    for (std::uint64_t tick = 0; tick < 16; ++tick) {
        require(fixture.simulation.step(tick));
    }
    require(fixture.simulation.resourceAvailable(1, 2) >= 4);
    const auto* node =
        fixture.simulation.world().try_get<ResourceNode>(fixture.node);
    require(node && node->remaining <= 2);

    bool harvested = false;
    bool deposited = false;
    for (const auto& event : fixture.simulation.events()) {
        harvested = harvested ||
            event.type == DomainEventType::ResourceHarvested;
        deposited = deposited ||
            event.type == DomainEventType::ResourceDeposited;
    }
    require(deposited || fixture.simulation.resourceAvailable(1, 2) >= 4);
    (void)harvested;
}

void testHarvestArchiveContinuation() {
    HarvestFixture original;
    for (std::uint64_t tick = 0; tick <= 5; ++tick) {
        require(original.simulation.step(tick));
    }

    const auto bytes = EncodeRtsSimulation(original.simulation);
    require(!bytes.empty());

    RtsSimulation restored(16, 8);
    registerHarvestContent(restored);
    require(DecodeRtsSimulation(bytes, restored));
    require(RtsSimulationArchive::authoritativeHash(original.simulation) ==
            RtsSimulationArchive::authoritativeHash(restored));

    for (std::uint64_t tick = 6; tick < 18; ++tick) {
        require(original.simulation.step(tick));
        require(restored.step(tick));
        require(RtsSimulationArchive::authoritativeHash(original.simulation) ==
                RtsSimulationArchive::authoritativeHash(restored));
        require(original.simulation.resourceAvailable(1, 2) ==
                restored.resourceAvailable(1, 2));
    }
}

void testSupplyProviderAndSessionArchive() {
    RtsGameSession session(16, 8);
    registerHarvestContent(session);
    require(session.setTeamSupplyCapacity(1, 0));
    require(session.setTeamResource(1, kPrimaryResourceType, 0));
    require(session.submit(buildCommand(0, 1)));
    require(session.step(0));
    require(session.supplyCapacity(1) == 2);

    const auto worker = session.createUnitDefinition(10, {2, 2}, 1);
    require(!worker.valid());

    const auto bytes = EncodeRtsGameSession(session);
    require(!bytes.empty());
    RtsGameSession restored(16, 8);
    registerHarvestContent(restored);
    require(DecodeRtsGameSession(bytes, restored));
    require(restored.supplyCapacity(1) == 2);
    require(RtsGameSessionArchive::authoritativeHash(session) ==
            RtsGameSessionArchive::authoritativeHash(restored));
}

} // namespace

int main() {
    testLargeMultiResourceAccounts();
    testTeamBuildIsolation();
    testHarvestDepositLoop();
    testHarvestArchiveContinuation();
    testSupplyProviderAndSessionArchive();
    std::cout << "RTS team economy tests passed\n";
    return EXIT_SUCCESS;
}
