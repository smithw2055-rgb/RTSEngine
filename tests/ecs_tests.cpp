#include <RTSEngine/Ecs/EntityCommandBuffer.h>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

struct Position {
    int x{};
    int y{};
};

struct Health {
    int value{};
};

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            std::cerr << "CHECK failed: " #expression << " at line " << __LINE__ << '\n'; \
            return EXIT_FAILURE; \
        } \
    } while (false)

} // namespace

int main() {
    using namespace rts::ecs;

    World world;
    const Entity first = world.create();
    CHECK(world.alive(first));
    world.emplace<Position>(first, Position{1, 2});
    CHECK(world.try_get<Position>(first)->x == 1);

    CHECK(world.destroy(first));
    CHECK(!world.alive(first));
    const Entity reused = world.create();
    CHECK(reused.index == first.index);
    CHECK(reused.generation != first.generation);
    CHECK(world.try_get<Position>(first) == nullptr);

    const Entity complete = world.create();
    world.emplace<Position>(complete, Position{3, 4});
    world.emplace<Health>(complete, Health{10});
    const Entity positionOnly = world.create();
    world.emplace<Position>(positionOnly, Position{5, 6});

    const auto matching = world.view<Position, Health>();
    CHECK(matching.size() == 1);
    CHECK(matching.front() == complete);

    Scheduler scheduler;
    std::vector<int> executionOrder;
    scheduler.add(Stage::Simulation, 1, 1,
                  [&](World&, const SystemContext&) { executionOrder.push_back(2); });
    scheduler.add(Stage::Simulation, 0, 100,
                  [&](World&, const SystemContext&) { executionOrder.push_back(1); });
    scheduler.run(world, 7);
    CHECK((executionOrder == std::vector<int>{1, 2}));

    EntityCommandBuffer commands;
    const SystemContext simulation{1, 0, Stage::Simulation};
    const DeferredEntity deferred = commands.create(simulation);
    commands.add<Position>(simulation, deferred, Position{9, 9});
    CHECK(world.view<Position>().size() == 2);

    commands.commit_through(world, Stage::Simulation);
    const auto afterCreate = world.view<Position>();
    CHECK(afterCreate.size() == 3);
    CHECK(world.try_get<Position>(afterCreate.back()) != nullptr);

    const Entity gated = world.create();
    const SystemContext cleanup{1, 4, Stage::Cleanup};
    commands.add<Health>(cleanup, gated, Health{7});
    commands.commit_through(world, Stage::Simulation);
    CHECK(world.try_get<Health>(gated) == nullptr);
    commands.commit_through(world, Stage::Cleanup);
    CHECK(world.try_get<Health>(gated)->value == 7);

    std::cout << "ecs tests passed\n";
    return EXIT_SUCCESS;
}
