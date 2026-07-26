#include <RTSEngine/Rts/Simulation.h>

#include <algorithm>
#include <cstdint>
#include <iostream>

int main() {
    using namespace rts::gameplay;

    RtsSimulation simulation(16, 10);
    simulation.setResources(500);
    simulation.setRequiredRoute({0, 5}, {15, 5});
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
    simulation.submit(build);

    simulation.step(0);
    simulation.step(1);

    const auto factoryIterator = std::find_if(
        simulation.snapshot().entities.begin(), simulation.snapshot().entities.end(),
        [](const SnapshotEntity& entity) { return entity.kind == SnapshotKind::Building; });
    if (factoryIterator == simulation.snapshot().entities.end()) return 1;
    const auto factory = factoryIterator->entity;

    TickCommand rally;
    rally.targetTick = 2;
    rally.issuer = 1;
    rally.sequence = 2;
    rally.type = CommandType::SetRally;
    rally.subject = factory;
    rally.targetX = 8;
    rally.targetY = 7;
    simulation.submit(rally);

    TickCommand train;
    train.targetTick = 2;
    train.issuer = 1;
    train.sequence = 3;
    train.type = CommandType::Train;
    train.subject = factory;
    train.definitionId = 1;
    simulation.submit(train);

    simulation.step(2);
    simulation.step(3);

    const auto unitIterator = std::find_if(
        simulation.snapshot().entities.begin(), simulation.snapshot().entities.end(),
        [](const SnapshotEntity& entity) { return entity.kind == SnapshotKind::Unit; });
    if (unitIterator == simulation.snapshot().entities.end()) return 1;

    TickCommand move;
    move.targetTick = 4;
    move.issuer = 1;
    move.sequence = 4;
    move.type = CommandType::Move;
    move.subject = unitIterator->entity;
    move.targetX = 13;
    move.targetY = 7;
    simulation.submit(move);

    for (std::uint64_t tick = 4; tick < 16; ++tick) simulation.step(tick);

    const auto& snapshot = simulation.snapshot();
    const auto finalUnit = std::find_if(snapshot.entities.begin(), snapshot.entities.end(),
        [](const SnapshotEntity& entity) { return entity.kind == SnapshotKind::Unit; });
    if (finalUnit == snapshot.entities.end()) return 1;

    std::cout << "tick=" << snapshot.tick << '\n';
    std::cout << "entities=" << snapshot.entities.size() << '\n';
    std::cout << "unit_position=" << finalUnit->x << ',' << finalUnit->y << '\n';
    std::cout << "resources=" << snapshot.resources.available << ','
              << snapshot.resources.reserved << ',' << snapshot.resources.spent << '\n';
    std::cout << "navigation_revision=" << simulation.navigation().revision() << '\n';
    std::cout << "hash=" << snapshot.worldHash << '\n';

    return finalUnit->x == 13 && finalUnit->y == 7 &&
           snapshot.resources.available == 360 &&
           snapshot.resources.reserved == 0 &&
           snapshot.resources.spent == 140 ? 0 : 1;
}
