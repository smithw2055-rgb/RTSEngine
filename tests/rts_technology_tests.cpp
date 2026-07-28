#include <RTSEngine/Rts/RtsGameSessionArchive.h>
#include <RTSEngine/Rts/SimulationArchive.h>

#include <cstdlib>
#include <iostream>

namespace {

using namespace rts::gameplay;
namespace ecs = rts::ecs;

void require(bool condition) {
    if (!condition) std::abort();
}

BuildingDefinition laboratoryDefinition() {
    BuildingDefinition value;
    value.id = 20;
    value.cost = 0;
    value.buildTicks = 1;
    value.width = 1;
    value.height = 1;
    value.producer = true;
    value.dropOffResourceType = kPrimaryResourceType;
    return value;
}

BuildingDefinition lockedBuildingDefinition() {
    BuildingDefinition value;
    value.id = 21;
    value.cost = 0;
    value.buildTicks = 1;
    value.width = 1;
    value.height = 1;
    return value;
}

UnitDefinition workerDefinition() {
    UnitDefinition value;
    value.id = 10;
    value.cost = 0;
    value.trainTicks = 1;
    value.cellsPerTick = 1;
    value.combat.maximumHealth = 20;
    value.combat.weaponDamage = 10;
    value.combat.weaponRange = 4;
    value.worker = true;
    value.cargoCapacity = 4;
    value.harvestAmount = 2;
    value.harvestTicks = 1;
    return value;
}

UnitDefinition lockedUnitDefinition() {
    auto value = workerDefinition();
    value.id = 11;
    value.worker = false;
    return value;
}

ResearchDefinition damageResearch(
    ResearchDefinitionId id = 100,
    std::uint32_t ticks = 2) {
    ResearchDefinition value;
    value.id = id;
    value.costs = {{kPrimaryResourceType, 40}, {2, 20}};
    value.researchTicks = ticks;
    value.modifiers.unitDamage = 500;
    return value;
}

void registerTechnologyContent(RtsSimulation& simulation) {
    require(simulation.registerBuilding(laboratoryDefinition()));
    require(simulation.registerBuilding(lockedBuildingDefinition()));
    require(simulation.registerUnit(workerDefinition()));
    require(simulation.registerUnit(lockedUnitDefinition()));
    require(simulation.registerResearch(damageResearch()));
    require(simulation.setBuildingPrerequisites(21, {{100}, {}}));
    require(simulation.setUnitPrerequisites(11, {{100}, {}}));
}

void registerTechnologyContent(RtsGameSession& session) {
    require(session.registerBuilding(laboratoryDefinition()));
    require(session.registerBuilding(lockedBuildingDefinition()));
    require(session.registerUnit(workerDefinition()));
    require(session.registerUnit(lockedUnitDefinition()));
    require(session.registerResearch(damageResearch()));
    require(session.setBuildingPrerequisites(21, {{100}, {}}));
    require(session.setUnitPrerequisites(11, {{100}, {}}));
}

TickCommand build(
    std::uint64_t tick,
    std::uint32_t team,
    std::uint32_t sequence,
    std::uint32_t definition,
    std::int32_t x,
    std::int32_t y) {
    TickCommand value;
    value.targetTick = tick;
    value.issuer = team;
    value.sequence = sequence;
    value.type = CommandType::Build;
    value.definitionId = definition;
    value.targetX = x;
    value.targetY = y;
    return value;
}

TickCommand research(
    std::uint64_t tick,
    std::uint32_t team,
    std::uint32_t sequence,
    ecs::Entity facility,
    ResearchDefinitionId definition) {
    TickCommand value;
    value.targetTick = tick;
    value.issuer = team;
    value.sequence = sequence;
    value.type = CommandType::Research;
    value.subject = facility;
    value.definitionId = definition;
    return value;
}

ecs::Entity findBuilding(
    const RtsSimulation& simulation,
    std::uint32_t definitionId) {
    ecs::Entity result{};
    simulation.world().eachRef<Building>(
        [&](ecs::Entity entity, const Building& building) {
            if (building.definitionId == definitionId &&
                (!result.valid() || entity < result)) {
                result = entity;
            }
        });
    return result;
}

bool hasRejection(
    const RtsSimulation& simulation,
    CommandRejectionReason reason) {
    for (const auto& event : simulation.events()) {
        if (event.type == DomainEventType::CommandRejected &&
            event.reason == static_cast<std::uint32_t>(reason)) {
            return true;
        }
    }
    return false;
}

void testResearchAndPrerequisites() {
    RtsSimulation simulation(16, 8);
    registerTechnologyContent(simulation);
    require(simulation.setTeamResource(1, kPrimaryResourceType, 100));
    require(simulation.setTeamResource(1, 2, 30));
    const auto existing = simulation.createUnitDefinition(10, {8, 5}, 1);
    require(existing.valid());

    require(simulation.submit(build(0, 1, 1, 20, 1, 3)));
    require(simulation.submit(build(0, 1, 2, 21, 5, 3)));
    require(simulation.step(0));
    require(hasRejection(
        simulation, CommandRejectionReason::PrerequisiteMissing));
    const auto laboratory = findBuilding(simulation, 20);
    require(laboratory.valid());
    require(!findBuilding(simulation, 21).valid());

    require(simulation.submit(research(1, 1, 3, laboratory, 100)));
    require(simulation.step(1));
    require(simulation.resourceAvailable(1, kPrimaryResourceType) == 60);
    require(simulation.resourceAvailable(1, 2) == 10);
    require(simulation.teamEconomy().reserved(1, kPrimaryResourceType) == 40);
    require(simulation.teamEconomy().reserved(1, 2) == 20);
    require(!simulation.researchCompleted(1, 100));

    require(simulation.step(2));
    require(simulation.researchCompleted(1, 100));
    require(simulation.teamEconomy().reserved(1, kPrimaryResourceType) == 0);
    require(simulation.teamEconomy().reserved(1, 2) == 0);
    require(simulation.teamEconomy().spent(1, kPrimaryResourceType) == 40);
    require(simulation.teamEconomy().spent(1, 2) == 20);
    const auto* weapon = simulation.world().try_get<Weapon>(existing);
    require(weapon && weapon->damage == 15);

    require(simulation.submit(build(3, 1, 4, 21, 5, 3)));
    require(simulation.step(3));
    require(findBuilding(simulation, 21).valid());
}

void testCancelResearchRefundsAllResources() {
    RtsSimulation simulation(16, 8);
    require(simulation.registerBuilding(laboratoryDefinition()));
    require(simulation.registerResearch(damageResearch(100, 5)));
    require(simulation.setTeamResource(1, kPrimaryResourceType, 100));
    require(simulation.setTeamResource(1, 2, 30));
    require(simulation.submit(build(0, 1, 1, 20, 1, 3)));
    require(simulation.step(0));
    const auto laboratory = findBuilding(simulation, 20);
    require(laboratory.valid());

    require(simulation.submit(research(1, 1, 2, laboratory, 100)));
    require(simulation.step(1));
    const auto* queue =
        simulation.world().try_get<ResearchQueue>(laboratory);
    require(queue && queue->items.size() == 1);

    TickCommand cancel;
    cancel.targetTick = 2;
    cancel.issuer = 1;
    cancel.sequence = 3;
    cancel.type = CommandType::CancelResearch;
    cancel.subject = laboratory;
    cancel.objectId = queue->items.front().id;
    require(simulation.submit(cancel));
    require(simulation.step(2));
    require(simulation.resourceAvailable(1, kPrimaryResourceType) == 100);
    require(simulation.resourceAvailable(1, 2) == 30);
    require(simulation.teamEconomy().reserved(1, kPrimaryResourceType) == 0);
    require(simulation.teamEconomy().reserved(1, 2) == 0);
}

void testResearchArchiveContinuation() {
    RtsSimulation original(16, 8);
    registerTechnologyContent(original);
    require(original.setTeamResource(1, kPrimaryResourceType, 100));
    require(original.setTeamResource(1, 2, 30));
    require(original.submit(build(0, 1, 1, 20, 1, 3)));
    require(original.step(0));
    const auto laboratory = findBuilding(original, 20);
    require(original.submit(research(1, 1, 2, laboratory, 100)));
    require(original.step(1));

    const auto bytes = EncodeRtsSimulation(original);
    require(!bytes.empty());
    RtsSimulation restored(16, 8);
    registerTechnologyContent(restored);
    require(DecodeRtsSimulation(bytes, restored));
    require(RtsSimulationArchive::authoritativeHash(original) ==
            RtsSimulationArchive::authoritativeHash(restored));

    for (std::uint64_t tick = 2; tick < 7; ++tick) {
        require(original.step(tick));
        require(restored.step(tick));
        require(RtsSimulationArchive::authoritativeHash(original) ==
                RtsSimulationArchive::authoritativeHash(restored));
    }
    require(original.researchCompleted(1, 100));
    require(restored.researchCompleted(1, 100));
}

void testSessionResearchReservation() {
    RtsGameSession session(16, 8);
    registerTechnologyContent(session);
    require(session.registerResearch(damageResearch(101, 5)));
    require(session.registerResearchPolicy({20, 1, {100, 101}}));
    require(session.setTeamResource(1, kPrimaryResourceType, 200));
    require(session.setTeamResource(1, 2, 100));
    require(session.submit(build(0, 1, 1, 20, 1, 3)));
    require(session.step(0));
    const auto laboratory = findBuilding(session.simulation(), 20);
    const auto first = research(1, 1, 2, laboratory, 100);
    require(session.submitDetailed(first) == SessionCommandResult::Accepted);
    require(session.submitDetailed(first) == SessionCommandResult::Accepted);
    require(session.pendingResearchReservations() == 1);
    require(session.submitDetailed(
                research(1, 1, 3, laboratory, 101)) ==
            SessionCommandResult::ResearchQueueFull);
    require(session.step(1));
    require(session.pendingResearchReservations() == 0);
}

void configureAiSession(RtsGameSession& session) {
    registerTechnologyContent(session);
    require(session.registerProducerPolicy({20, 4, {10}}));
    require(session.registerResearchPolicy({20, 1, {100}}));
    require(session.setTeamSupplyCapacity(2, 10));
    require(session.setTeamResource(2, kPrimaryResourceType, 100));
    require(session.setTeamResource(2, 2, 30));
    require(session.registerAiTeam(2, {12, 3}, 1));
    AiEconomyPlan plan;
    plan.teamId = 2;
    plan.thinkIntervalTicks = 1;
    plan.resourceType = kPrimaryResourceType;
    plan.workerDefinitionId = 10;
    plan.minimumWorkers = 1;
    plan.preferredResearchId = 100;
    plan.buildAnchor = {1, 1};
    require(session.registerAiEconomyPlan(plan));
}

void testAiEconomyAndSessionArchive() {
    RtsGameSession session(16, 8);
    configureAiSession(session);
    const auto node = session.createResourceNode(
        {6, 3}, kPrimaryResourceType, 20);
    require(node.valid());
    require(session.submit(build(0, 2, 1, 20, 1, 3)));
    require(session.step(0));

    const auto pending = session.simulation().commandStreamState().pending;
    require(pending.size() == 2);
    require(pending[0].targetTick == 1 && pending[1].targetTick == 1);
    require(pending[0].issuer == 2 && pending[1].issuer == 2);
    require(pending[0].sequence != pending[1].sequence);
    require((pending[0].type == CommandType::Train &&
             pending[1].type == CommandType::Research) ||
            (pending[1].type == CommandType::Train &&
             pending[0].type == CommandType::Research));
    require(session.pendingTrainReservations() == 1);
    require(session.pendingResearchReservations() == 1);

    const auto bytes = RtsGameSessionArchive::encode(session);
    require(!bytes.empty());
    RtsGameSession restored(16, 8);
    registerTechnologyContent(restored);
    require(RtsGameSessionArchive::decode(bytes, restored));
    require(restored.pendingTrainReservations() == 1);
    require(restored.pendingResearchReservations() == 1);
    require(RtsGameSessionArchive::authoritativeHash(session) ==
            RtsGameSessionArchive::authoritativeHash(restored));

    require(session.step(1));
    require(restored.step(1));
    require(RtsGameSessionArchive::authoritativeHash(session) ==
            RtsGameSessionArchive::authoritativeHash(restored));
    require(!session.researchCompleted(2, 100));
    require(!restored.researchCompleted(2, 100));

    const auto next = session.simulation().commandStreamState().pending;
    require(!next.empty());
    bool gather = false;
    for (const auto& command : next) {
        gather = gather || command.type == CommandType::Gather;
    }
    require(gather);

    require(session.step(2));
    require(restored.step(2));
    require(RtsGameSessionArchive::authoritativeHash(session) ==
            RtsGameSessionArchive::authoritativeHash(restored));
    require(session.researchCompleted(2, 100));
    require(restored.researchCompleted(2, 100));
}

} // namespace

int main() {
    testResearchAndPrerequisites();
    testCancelResearchRefundsAllResources();
    testResearchArchiveContinuation();
    testSessionResearchReservation();
    testAiEconomyAndSessionArchive();
    std::cout << "RTS technology tests passed\n";
    return EXIT_SUCCESS;
}
