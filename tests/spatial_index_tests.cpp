#include <RTSEngine/Ecs/EntityCommandBuffer.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/Combat.h>
#include <RTSEngine/Rts/SpatialIndex.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace rts;

struct Position final {
    std::int32_t x{};
    std::int32_t y{};
};

void testExactQueriesAndCanonicalResults() {
    gameplay::FixedGridSpatialIndex index(16, 16, 4);
    const ecs::Entity first{1, 1};
    const ecs::Entity second{2, 1};
    const ecs::Entity third{3, 1};
    const ecs::Entity fourth{4, 1};
    const ecs::Entity fifth{5, 1};

    assert(!index.insert({}, 1, 1));
    assert(!index.insert(first, -1, 1));
    assert(!index.insert(first, 16, 1));
    assert(index.insert(fifth, 8, 8));
    assert(index.insert(third, 6, 4));
    assert(index.insert(first, 1, 1));
    assert(index.insert(fourth, 4, 7));
    assert(index.insert(second, 4, 4));
    index.finalize();

    assert(index.entryCount() == 5);
    std::vector<ecs::Entity> result;
    index.queryManhattan(4, 4, 2, result);
    assert(result.size() == 2);
    assert(result[0] == second);
    assert(result[1] == third);

    index.queryManhattan(4, 4, -1, result);
    assert(result.empty());
    index.queryManhattan(-100, -100, 2, result);
    assert(result.empty());
}

std::vector<ecs::Entity> visitOrder(
    const gameplay::FixedGridSpatialIndex& index) {
    std::vector<ecs::Entity> order;
    index.visitManhattan(
        8, 8, 32,
        [&order](const gameplay::SpatialIndexEntry& entry) {
            order.push_back(entry.entity);
        });
    return order;
}

void testVisitOrderIndependentOfInsertionOrder() {
    gameplay::FixedGridSpatialIndex forward(20, 20, 5);
    gameplay::FixedGridSpatialIndex reverse(20, 20, 5);
    struct Value final {
        ecs::Entity entity;
        std::int32_t x;
        std::int32_t y;
    };
    const std::vector<Value> values{
        {{9, 1}, 2, 2},
        {{2, 1}, 7, 2},
        {{7, 1}, 12, 2},
        {{1, 1}, 3, 11},
        {{5, 1}, 9, 9},
        {{4, 1}, 18, 18}};

    for (const auto& value : values) {
        assert(forward.insert(value.entity, value.x, value.y));
    }
    for (auto iterator = values.rbegin(); iterator != values.rend(); ++iterator) {
        assert(reverse.insert(iterator->entity, iterator->x, iterator->y));
    }
    forward.finalize();
    reverse.finalize();

    assert(visitOrder(forward) == visitOrder(reverse));
}

void populateScaleIndex(gameplay::FixedGridSpatialIndex& index) {
    index.clear();
    for (std::uint32_t value = 1; value <= 1024; ++value) {
        const auto x = static_cast<std::int32_t>((value * 17u) % 64u);
        const auto y = static_cast<std::int32_t>((value * 29u) % 64u);
        assert(index.insert({value, 1}, x, y));
    }
    index.finalize();
}

void testCapacityStableAfterWarmup() {
    gameplay::FixedGridSpatialIndex index(64, 64, 4);
    std::vector<ecs::Entity> result;

    populateScaleIndex(index);
    index.queryManhattan(32, 32, 128, result);
    assert(index.entryCount() == 1024);
    assert(result.size() == 1024);
    const auto bucketCapacity = index.totalBucketCapacity();
    const auto queryCapacity = result.capacity();

    for (std::uint32_t iteration = 0; iteration < 256; ++iteration) {
        populateScaleIndex(index);
        index.queryManhattan(
            static_cast<std::int32_t>(iteration % 64u),
            static_cast<std::int32_t>((iteration * 13u) % 64u),
            128,
            result);
        assert(index.entryCount() == 1024);
        assert(result.size() == 1024);
        assert(index.totalBucketCapacity() == bucketCapacity);
        assert(result.capacity() == queryCapacity);
        assert((result.front() == ecs::Entity{1, 1}));
        assert((result.back() == ecs::Entity{1024, 1}));
    }
}

void testCombatRebuildAndTargetTieBreak() {
    ecs::World world;
    ecs::EntityCommandBuffer commands;
    gameplay::CombatRuntime combat(64, 64);

    const auto attacker = world.create();
    world.emplace<Position>(attacker, Position{32, 32});
    world.emplace<gameplay::Team>(attacker, gameplay::Team{1});
    world.emplace<gameplay::Health>(attacker, gameplay::Health{100, 100});
    world.emplace<gameplay::Weapon>(
        attacker, gameplay::Weapon{0, 32, 1, 0});
    world.emplace<gameplay::CombatTarget>(
        attacker, gameplay::CombatTarget{});

    ecs::Entity expected{};
    for (std::uint32_t value = 0; value < 255; ++value) {
        const auto enemy = world.create();
        const auto x = static_cast<std::int32_t>((value * 11u) % 64u);
        const auto y = static_cast<std::int32_t>((value * 23u) % 64u);
        world.emplace<Position>(enemy, Position{x, y});
        world.emplace<gameplay::Team>(enemy, gameplay::Team{2});
        world.emplace<gameplay::Health>(enemy, gameplay::Health{10, 10});
        const auto distance =
            (x > 32 ? x - 32 : 32 - x) +
            (y > 32 ? y - 32 : 32 - y);
        if (distance <= 32 &&
            (!expected.valid() || enemy < expected)) {
            expected = enemy;
        }
    }
    assert(expected.valid());

    combat.advance<Position>(
        {0, 0, ecs::Stage::Combat}, commands, world);
    const auto capacity = combat.spatialIndex().totalBucketCapacity();
    assert(combat.spatialIndex().entryCount() == 256);
    const auto* target = world.try_get<gameplay::CombatTarget>(attacker);
    assert(target && target->entity.valid());

    const auto selected = target->entity;
    for (std::uint64_t tick = 1; tick <= 128; ++tick) {
        combat.advance<Position>(
            {tick, 0, ecs::Stage::Combat}, commands, world);
        assert(combat.spatialIndex().entryCount() == 256);
        assert(combat.spatialIndex().totalBucketCapacity() == capacity);
        target = world.try_get<gameplay::CombatTarget>(attacker);
        assert(target && target->entity == selected);
    }
}

} // namespace

int main() {
    testExactQueriesAndCanonicalResults();
    testVisitOrderIndependentOfInsertionOrder();
    testCapacityStableAfterWarmup();
    testCombatRebuildAndTargetTieBreak();
    std::cout << "spatial index tests passed\n";
    return 0;
}
