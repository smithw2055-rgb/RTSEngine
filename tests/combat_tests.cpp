#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Rts/Combat.h>
#include <RTSEngine/Rts/Navigation.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

struct Position {
    std::int32_t x{};
    std::int32_t y{};
};

using namespace rts;

std::vector<std::int32_t> runDuel() {
    ecs::World world;
    ecs::EntityCommandBuffer commands;
    gameplay::CombatRuntime combat(16, 16);

    const auto attackerA = world.create();
    world.emplace<Position>(attackerA, Position{1, 1});
    world.emplace<gameplay::Team>(attackerA, gameplay::Team{1});
    world.emplace<gameplay::Health>(attackerA, gameplay::Health{20, 20});
    world.emplace<gameplay::Weapon>(attackerA, gameplay::Weapon{4, 5, 2, 0});
    world.emplace<gameplay::CombatTarget>(attackerA, gameplay::CombatTarget{});

    const auto attackerB = world.create();
    world.emplace<Position>(attackerB, Position{1, 2});
    world.emplace<gameplay::Team>(attackerB, gameplay::Team{1});
    world.emplace<gameplay::Health>(attackerB, gameplay::Health{20, 20});
    world.emplace<gameplay::Weapon>(attackerB, gameplay::Weapon{4, 5, 2, 0});
    world.emplace<gameplay::CombatTarget>(attackerB, gameplay::CombatTarget{});

    const auto defender = world.create();
    world.emplace<Position>(defender, Position{4, 1});
    world.emplace<gameplay::Team>(defender, gameplay::Team{2});
    world.emplace<gameplay::Health>(defender, gameplay::Health{10, 10});
    world.emplace<gameplay::Armor>(defender, gameplay::Armor{1});

    std::vector<std::int32_t> healthHistory;
    for (std::uint64_t tick = 0; tick < 5; ++tick) {
        const ecs::SystemContext context{tick, 0, ecs::Stage::Simulation};
        combat.advance<Position>(context, commands, world);
        if (const auto* health = world.try_get<gameplay::Health>(defender)) {
            healthHistory.push_back(health->current);
        } else {
            healthHistory.push_back(-1);
        }
        commands.commit_through(world, ecs::Stage::Simulation);
    }

    assert(!world.alive(defender));
    return healthHistory;
}

void testStableTargetTieBreak() {
    ecs::World world;
    ecs::EntityCommandBuffer commands;
    gameplay::CombatRuntime combat(16, 16);

    const auto attacker = world.create();
    world.emplace<Position>(attacker, Position{5, 5});
    world.emplace<gameplay::Team>(attacker, gameplay::Team{1});
    world.emplace<gameplay::Health>(attacker, gameplay::Health{10, 10});
    world.emplace<gameplay::Weapon>(attacker, gameplay::Weapon{1, 4, 1, 0});
    world.emplace<gameplay::CombatTarget>(attacker, gameplay::CombatTarget{});

    const auto first = world.create();
    world.emplace<Position>(first, Position{4, 5});
    world.emplace<gameplay::Team>(first, gameplay::Team{2});
    world.emplace<gameplay::Health>(first, gameplay::Health{10, 10});

    const auto second = world.create();
    world.emplace<Position>(second, Position{6, 5});
    world.emplace<gameplay::Team>(second, gameplay::Team{2});
    world.emplace<gameplay::Health>(second, gameplay::Health{10, 10});

    combat.advance<Position>({0, 0, ecs::Stage::Simulation}, commands, world);
    const auto* target = world.try_get<gameplay::CombatTarget>(attacker);
    assert(target && target->entity == first);
}

void testDeathCallbackReleasesBlocker() {
    ecs::World world;
    ecs::EntityCommandBuffer commands;
    gameplay::CombatRuntime combat(8, 8);
    gameplay::NavigationGrid navigation(8, 8);

    const auto tower = world.create();
    world.emplace<Position>(tower, Position{3, 3});
    world.emplace<gameplay::Team>(tower, gameplay::Team{2});
    world.emplace<gameplay::Health>(tower, gameplay::Health{1, 1});
    navigation.setBlocked({3, 3}, true);

    const auto attacker = world.create();
    world.emplace<Position>(attacker, Position{2, 3});
    world.emplace<gameplay::Team>(attacker, gameplay::Team{1});
    world.emplace<gameplay::Health>(attacker, gameplay::Health{10, 10});
    world.emplace<gameplay::Weapon>(attacker, gameplay::Weapon{5, 2, 1, 0});
    world.emplace<gameplay::CombatTarget>(attacker, gameplay::CombatTarget{});

    std::uint32_t deaths = 0;
    combat.advance<Position>({0, 0, ecs::Stage::Simulation}, commands, world,
        [&](ecs::Entity entity) {
            if (entity == tower) {
                navigation.setBlocked({3, 3}, false);
                ++deaths;
            }
        });
    commands.commit_through(world, ecs::Stage::Simulation);

    assert(deaths == 1);
    assert(!world.alive(tower));
    assert(!navigation.blocked({3, 3}));
}

} // namespace

int main() {
    const auto first = runDuel();
    const auto second = runDuel();
    assert(first == second);
    assert(first.size() == 5);
    assert(first[0] == 4); // two stable hits: (4 damage - 1 armor) x 2
    assert(first[1] == 4); // cooldown tick produces no damage
    assert(first[2] == 0); // the second stable volley kills the defender

    testStableTargetTieBreak();
    testDeathCallbackReleasesBlocker();
    std::cout << "combat tests passed\n";
    return 0;
}
