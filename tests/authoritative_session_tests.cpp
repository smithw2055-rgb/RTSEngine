#include <RTSEngine/Roguelite/RunSimulation.h>
#include <RTSEngine/Rts/Simulation.h>
#include <RTSEngine/TowerDefense/Simulation.h>

#include <cstdlib>
#include <iostream>

namespace {

using namespace rts;

void require(bool condition) {
    if (!condition) std::abort();
}

void testPublicRtsCannotForgeInternalAuthority() {
    gameplay::RtsSimulation simulation(8, 8);
    const auto unit = simulation.createUnit({1, 1}, {1}, 1);
    require(unit.valid());

    gameplay::TickCommand command;
    command.targetTick = 0;
    command.issuer = 0x80000001u;
    command.sequence = 1;
    command.type = gameplay::CommandType::Move;
    command.subject = unit;
    command.targetX = 4;
    command.targetY = 4;

    require(
        simulation.submitDetailed(command) ==
        sim::CommandSubmitResult::Unauthorized);
    require(!simulation.submit(command));
    require(simulation.commandStreamState().pending.empty());
}

void testRtsConfigurationFreezesAfterStart() {
    gameplay::RtsSimulation simulation(8, 8);
    require(simulation.step(0));
    require(simulation.configurationFrozen());
    require(!simulation.registerUnit({7, 0, 1, 1, {}, 4}));
    require(!simulation.registerBuilding({7, 0, 1, 1, 1, false, {}, 4}));
    require(!simulation.setPlayerTeam(2));
    require(!simulation.setRequiredRoute({0, 0}, {7, 7}));
    require(!simulation.createUnit({1, 1}, {1}, 1).valid());
}

void testTowerRejectedTickDoesNotSplitNestedState() {
    tower_defense::TowerDefenseSimulation simulation(8, 8, 11);
    require(simulation.step(0));

    const auto outerTick = simulation.lastTick();
    const auto innerTick = simulation.rts().lastCompletedTick();
    const auto outerCommands = simulation.commandStreamState();
    const auto innerCommands = simulation.rts().commandStreamState();

    require(!simulation.step(2));
    require(simulation.lastTick() == outerTick);
    require(simulation.rts().lastCompletedTick() == innerTick);
    require(
        simulation.commandStreamState().committedThrough ==
        outerCommands.committedThrough);
    require(
        simulation.rts().commandStreamState().committedThrough ==
        innerCommands.committedThrough);
    require(!simulation.registerWave({}));
}

void testRunRejectedTickDoesNotSplitNestedState() {
    roguelite::RunSimulation simulation(8, 8, 13);
    require(simulation.step(0));

    const auto runTick = simulation.lastTick();
    const auto towerTick = simulation.tower().lastTick();
    const auto rtsTick = simulation.tower().rts().lastCompletedTick();
    const auto runCommands = simulation.commandStreamState();
    const auto towerCommands = simulation.tower().commandStreamState();
    const auto rtsCommands = simulation.tower().rts().commandStreamState();

    require(!simulation.step(2));
    require(simulation.lastTick() == runTick);
    require(simulation.tower().lastTick() == towerTick);
    require(simulation.tower().rts().lastCompletedTick() == rtsTick);
    require(
        simulation.commandStreamState().committedThrough ==
        runCommands.committedThrough);
    require(
        simulation.tower().commandStreamState().committedThrough ==
        towerCommands.committedThrough);
    require(
        simulation.tower().rts().commandStreamState().committedThrough ==
        rtsCommands.committedThrough);
    require(!simulation.registerRun({1, {1}, {}}));
}

} // namespace

int main() {
    testPublicRtsCannotForgeInternalAuthority();
    testRtsConfigurationFreezesAfterStart();
    testTowerRejectedTickDoesNotSplitNestedState();
    testRunRejectedTickDoesNotSplitNestedState();
    std::cout << "authoritative session boundary tests passed\n";
    return 0;
}
