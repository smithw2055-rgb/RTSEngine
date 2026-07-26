#include <RTSEngine/Rts/Simulation.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace rts::gameplay;

bool hasEvent(const RtsSimulation& simulation, DomainEventType type) {
    return std::any_of(simulation.events().begin(), simulation.events().end(),
                       [type](const DomainEvent& event) { return event.type == type; });
}

std::uint32_t eventObject(const RtsSimulation& simulation, DomainEventType type) {
    const auto iterator = std::find_if(simulation.events().begin(), simulation.events().end(),
                                       [type](const DomainEvent& event) { return event.type == type; });
    return iterator == simulation.events().end() ? 0 : iterator->objectId;
}

std::vector<std::uint64_t> runScenario() {
    RtsSimulation simulation(12, 8);
    simulation.setResources(500);
    simulation.setRequiredRoute({0, 4}, {11, 4});
    simulation.registerBuilding({1, 100, 2, 2, 2, true});
    simulation.registerUnit({1, 40, 2, 1});

    TickCommand build;
    build.targetTick = 0;
    build.issuer = 1;
    build.sequence = 1;
    build.type = CommandType::Build;
    build.targetX = 3;
    build.targetY = 1;
    build.definitionId = 1;
    assert(simulation.submit(build));

    std::vector<std::uint64_t> hashes;
    simulation.step(0);
    hashes.push_back(simulation.snapshot().worldHash);
    assert(hasEvent(simulation, DomainEventType::ConstructionAccepted));
    assert(simulation.snapshot().resources.available == 400);
    assert(simulation.snapshot().resources.reserved == 100);

    simulation.step(1);
    hashes.push_back(simulation.snapshot().worldHash);
    assert(hasEvent(simulation, DomainEventType::ConstructionCompleted));
    assert(simulation.snapshot().resources.reserved == 0);
    assert(simulation.snapshot().resources.spent == 100);

    const auto buildingIterator = std::find_if(
        simulation.snapshot().entities.begin(), simulation.snapshot().entities.end(),
        [](const SnapshotEntity& entity) { return entity.kind == SnapshotKind::Building; });
    assert(buildingIterator != simulation.snapshot().entities.end());
    const auto factory = buildingIterator->entity;

    TickCommand rally;
    rally.targetTick = 2;
    rally.issuer = 1;
    rally.sequence = 2;
    rally.type = CommandType::SetRally;
    rally.subject = factory;
    rally.targetX = 8;
    rally.targetY = 6;
    assert(simulation.submit(rally));

    TickCommand train;
    train.targetTick = 2;
    train.issuer = 1;
    train.sequence = 3;
    train.type = CommandType::Train;
    train.subject = factory;
    train.definitionId = 1;
    assert(simulation.submit(train));

    simulation.step(2);
    hashes.push_back(simulation.snapshot().worldHash);
    assert(hasEvent(simulation, DomainEventType::RallyPointChanged));
    assert(hasEvent(simulation, DomainEventType::ProductionAccepted));
    assert(simulation.snapshot().resources.available == 360);
    assert(simulation.snapshot().resources.reserved == 40);

    simulation.step(3);
    hashes.push_back(simulation.snapshot().worldHash);
    assert(hasEvent(simulation, DomainEventType::ProductionCompleted));
    assert(simulation.snapshot().resources.reserved == 0);
    assert(simulation.snapshot().resources.spent == 140);

    const auto produced = std::find_if(
        simulation.snapshot().entities.begin(), simulation.snapshot().entities.end(),
        [](const SnapshotEntity& entity) {
            return entity.kind == SnapshotKind::Unit && entity.x == 8 && entity.y == 6;
        });
    assert(produced != simulation.snapshot().entities.end());

    TickCommand trainToCancel = train;
    trainToCancel.targetTick = 4;
    trainToCancel.sequence = 4;
    assert(simulation.submit(trainToCancel));
    simulation.step(4);
    hashes.push_back(simulation.snapshot().worldHash);
    const auto productionId = eventObject(simulation, DomainEventType::ProductionAccepted);
    assert(productionId != 0);
    assert(simulation.snapshot().resources.reserved == 40);

    TickCommand cancel;
    cancel.targetTick = 5;
    cancel.issuer = 1;
    cancel.sequence = 5;
    cancel.type = CommandType::CancelProduction;
    cancel.subject = factory;
    cancel.objectId = productionId;
    assert(simulation.submit(cancel));
    simulation.step(5);
    hashes.push_back(simulation.snapshot().worldHash);
    assert(hasEvent(simulation, DomainEventType::ProductionCancelled));
    assert(simulation.snapshot().resources.available == 360);
    assert(simulation.snapshot().resources.reserved == 0);
    assert(simulation.snapshot().resources.spent == 140);

    return hashes;
}

} // namespace

int main() {
    const auto first = runScenario();
    const auto second = runScenario();
    assert(first == second);
    std::cout << "rts economy production tests passed\n";
    return 0;
}
