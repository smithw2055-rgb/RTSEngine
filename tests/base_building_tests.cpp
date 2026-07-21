#include <RTSEngine/Rts/BaseBuilding.h>

#include <cassert>
#include <iostream>

int main() {
    using namespace rts;
    using namespace rts::gameplay;

    ecs::World world;
    ecs::EntityCommandBuffer commands;
    NavigationGrid navigation(8, 5);
    ResourceLedger ledger{100, 0, 0};
    BaseBuildingRuntime runtime(ledger, navigation);

    const BuildingDefinition tower{1, 60, 2, 1, 4};
    const ecs::SystemContext commandContext{1, 0, ecs::Stage::Command};
    const ecs::SystemContext simulationContext{1, 1, ecs::Stage::Simulation};
    const ecs::SystemContext cleanupContext{1, 2, ecs::Stage::Cleanup};

    const auto accepted = runtime.begin(commandContext, commands, tower, {3, 0}, {0, 2}, {7, 2});
    assert(accepted.accepted);
    assert(ledger.available == 40);
    assert(ledger.reserved == 60);
    assert(navigation.blocked({3, 1}));

    const auto oversold = runtime.begin(commandContext, commands, tower, {5, 0}, {0, 2}, {7, 2});
    assert(!oversold.accepted);
    assert(oversold.failure == BuildFailure::InsufficientResources);
    assert(ledger.available == 40);
    assert(ledger.reserved == 60);

    commands.commit_through(world, ecs::Stage::Command);
    auto sites = world.view<ConstructionSite, BuildingFootprint>();
    assert(sites.size() == 1);

    runtime.advance(simulationContext, commands, world);
    commands.commit_through(world, ecs::Stage::Simulation);
    assert(world.view<ConstructionSite>().size() == 1);

    runtime.advance(simulationContext, commands, world);
    commands.commit_through(world, ecs::Stage::Simulation);
    assert(world.view<ConstructionSite>().empty());
    assert((world.view<Building, BuildingFootprint>().size() == 1));
    assert(ledger.reserved == 0);
    assert(ledger.spent == 60);

    ledger.available += 30;
    const BuildingDefinition small{2, 20, 10, 1, 1};
    const auto cancellable = runtime.begin(commandContext, commands, small, {1, 1}, {0, 2}, {7, 2});
    assert(cancellable.accepted);
    commands.commit_through(world, ecs::Stage::Command);
    assert(navigation.blocked({1, 1}));
    assert(runtime.cancel(cleanupContext, commands, world, cancellable.constructionId) == BuildFailure::None);
    commands.commit_through(world, ecs::Stage::Cleanup);
    assert(!navigation.blocked({1, 1}));
    assert(ledger.reserved == 0);
    assert(ledger.available == 70);

    const BuildingDefinition wall{3, 10, 1, 1, 5};
    const auto blocksRoute = runtime.begin(commandContext, commands, wall, {4, 0}, {0, 2}, {7, 2});
    assert(!blocksRoute.accepted);
    assert(blocksRoute.failure == BuildFailure::BlocksRequiredPath);

    const auto occupied = runtime.begin(commandContext, commands, small, {3, 1}, {0, 2}, {7, 2});
    assert(!occupied.accepted);
    assert(occupied.failure == BuildFailure::Occupied);

    std::cout << "base building tests passed\n";
    return 0;
}
