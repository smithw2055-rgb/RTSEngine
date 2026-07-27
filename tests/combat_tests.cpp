#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Rts/Combat.h>
#include <RTSEngine/Rts/Navigation.h>
#include <RTSEngine/Rts/SpatialIndex.h>

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
        const ecs::SystemContext context{tick, 0, ecs::Stage::Combat};
        combat.advance<Position>(context, commands, world);
        if (const auto* health = world.try_get<gameplay::Health>(defender)) {
            healthHistory.push_back(health->current);
        } else {
            healthHistory.push_back(-1);
        }
        commands.commit_through(world, ecs::Stage::Combat);
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

    combat.advance<Position>({0, 0, ecs::Stage::Combat}, commands, world);
    const auto* target = world.try_get<gameplay::CombatTarget>(attacker);
    assert(target && target->entity == first);
}

void testFixedGridSpatialIndex() {
    gameplay::FixedGridSpatialIndex index(64, 64, 4);
    std::vector<ecs::Entity> result;

    assert(!index.insert({}, 1, 1));
    assert(!index.insert({1, 1}, -1, 1));
    assert(index.insert({5, 1}, 8, 8));
    assert(index.insert({3, 1}, 6, 4));
    assert(index.insert({1, 1}, 1, 1));
    assert(index.insert({4, 1}, 4, 7));
    assert(index.insert({2, 1}, 4, 4));
    index.finalize();

    index.queryManhattan(4, 4, 2, result);
    assert(result.size() == 2);
    assert((result[0] == ecs::Entity{2, 1}));
    assert((result[1] == ecs::Entity{3, 1}));

    auto populate = [&index]() {
        index.clear();
        for (std::uint32_t value = 1; value <= 1024; ++value) {
            const auto x = static_cast<std::int32_t>((value * 17u) % 64u);
            const auto y = static_cast<std::int32_t>((value * 29u) % 64u);
            assert(index.insert({value, 1}, x, y));
        }
        index.finalize();
    };

    populate();
    index.queryManhattan(32, 32, 128, result);
    assert(index.entryCount() == 1024);
    assert(result.size() == 1024);
    const auto bucketCapacity = index.totalBucketCapacity();
    const auto resultCapacity = result.capacity();

    for (std::uint32_t iteration = 0; iteration < 128; ++iteration) {
        populate();
        index.queryManhattan(
            static_cast<std::int32_t>(iteration % 64u),
            static_cast<std::int32_t>((iteration * 13u) % 64u),
            128,
            result);
        assert(index.entryCount() == 1024);
        assert(result.size() == 1024);
        assert(index.totalBucketCapacity() == bucketCapacity);
        assert(result.capacity() == resultCapacity);
        assert((result.front() == ecs::Entity{1, 1}));
        assert((result.back() == ecs::Entity{1024, 1}));
    }
}

void testCombatSpatialCapacityStable() {
    ecs::World world;
    ecs::EntityCommandBuffer commands;
    gameplay::CombatRuntime combat(64, 64);

    const auto attacker = world.create();
    world.emplace<Position>(attacker, Position{32, 32});
    world.emplace<gameplay::Team>(attacker, gameplay::Team{1});
    world.emplace<gameplay::Health>(attacker, gameplay::Health{100, 100});
    world.emplace<gameplay::Weapon>(attacker, gameplay::Weapon{0, 32, 1, 0});
    world.emplace<gameplay::CombatTarget>(attacker, gameplay::CombatTarget{});

    for (std::uint32_t value = 0; value < 255; ++value) {
        const auto enemy = world.create();
        const auto x = static_cast<std::int32_t>((value * 11u) % 64u);
        const auto y = static_cast<std::int32_t>((value * 23u) % 64u);
        world.emplace<Position>(enemy, Position{x, y});
        world.emplace<gameplay::Team>(enemy, gameplay::Team{2});
        world.emplace<gameplay::Health>(enemy, gameplay::Health{10, 10});
    }

    combat.advance<Position>({0, 0, ecs::Stage::Combat}, commands, world);
    assert(combat.spatialIndex().entryCount() == 256);
    const auto capacity = combat.spatialIndex().totalBucketCapacity();
    const auto* target = world.try_get<gameplay::CombatTarget>(attacker);
    assert(target && target->entity.valid());
    const auto selected = target->entity;

    for (std::uint64_t tick = 1; tick <= 64; ++tick) {
        combat.advance<Position>(
            {tick, 0, ecs::Stage::Combat}, commands, world);
        assert(combat.spatialIndex().entryCount() == 256);
        assert(combat.spatialIndex().totalBucketCapacity() == capacity);
        target = world.try_get<gameplay::CombatTarget>(attacker);
        assert(target && target->entity == selected);
    }
}

struct DeathCallbackContext final {
    ecs::Entity attacker{};
    ecs::Entity tower{};
    gameplay::NavigationGrid* navigation{};
    std::uint32_t* deaths{};
};

void releaseDeadTower(
    void* rawContext,
    ecs::Entity victim,
    ecs::Entity killer) {
    auto& context = *static_cast<DeathCallbackContext*>(rawContext);
    assert(killer == context.attacker);
    if (victim == context.tower) {
        context.navigation->setBlocked({3, 3}, false);
        ++*context.deaths;
    }
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
    DeathCallbackContext deathContext{
        attacker, tower, &navigation, &deaths};
    combat.advance<Position>(
        {0, 0, ecs::Stage::Combat},
        commands,
        world,
        &deathContext,
        &releaseDeadTower);
    commands.commit_through(world, ecs::Stage::Combat);

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
    testFixedGridSpatialIndex();
    testCombatSpatialCapacityStable();
    testDeathCallbackReleasesBlocker();
    std::cout << "combat tests passed\n";
    return 0;
}
