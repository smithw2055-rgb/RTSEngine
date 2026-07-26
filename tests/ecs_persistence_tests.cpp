#include <RTSEngine/Ecs/WorldArchive.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

struct Position {
    std::int32_t x{};
    std::int32_t y{};
};

struct Health {
    std::int32_t value{};
};

struct TransientMarker {
    std::int32_t value{};
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
    using namespace rts;
    using namespace rts::ecs;

    EntityRegistry registry;
    const Entity first = registry.create();
    const Entity second = registry.create();
    const Entity third = registry.create();
    CHECK(registry.destroy(second));
    CHECK(registry.destroy(first));

    foundation::BinaryWriter registryWriter;
    CHECK(registry.writeState(registryWriter));
    EntityRegistryState registryState;
    foundation::BinaryReader registryReader(registryWriter.bytes());
    CHECK(EntityRegistry::readState(registryReader, registryState));
    CHECK(registryReader.atEnd());

    EntityRegistry restoredRegistry;
    CHECK(restoredRegistry.restore(registryState));
    CHECK(restoredRegistry.alive(third));
    CHECK(registry.create() == restoredRegistry.create());
    CHECK(registry.create() == restoredRegistry.create());

    auto invalidRegistryState = registryState;
    invalidRegistryState.free.push_back(invalidRegistryState.free.back());
    const auto preservedRegistryState = restoredRegistry.snapshot();
    CHECK(!restoredRegistry.restore(invalidRegistryState));
    CHECK(restoredRegistry.snapshot().generations == preservedRegistryState.generations);
    CHECK(restoredRegistry.snapshot().alive == preservedRegistryState.alive);
    CHECK(restoredRegistry.snapshot().free == preservedRegistryState.free);

    ComponentSchemaRegistry schemas;
    CHECK(schemas.registerSchema<Position>(
        0x1001u, 1u, "Position",
        [](foundation::BinaryWriter& writer, const Position& value) {
            writer.writeI32(value.x);
            writer.writeI32(value.y);
        },
        [](foundation::BinaryReader& reader,
           ComponentSchemaVersion version,
           Position& value) {
            return version == 1u &&
                   reader.readI32(value.x) && reader.readI32(value.y);
        },
        [](foundation::CanonicalHash& hash, const Position& value) {
            hash.WriteI32(value.x);
            hash.WriteI32(value.y);
        }));
    CHECK(schemas.registerSchema<Health>(
        0x2000u, 1u, "Health",
        [](foundation::BinaryWriter& writer, const Health& value) {
            writer.writeI32(value.value);
        },
        [](foundation::BinaryReader& reader,
           ComponentSchemaVersion version,
           Health& value) {
            return version == 1u && reader.readI32(value.value);
        },
        [](foundation::CanonicalHash& hash, const Health& value) {
            hash.WriteI32(value.value);
        }));

    World unfrozenWorld;
    unfrozenWorld.create();
    foundation::BinaryWriter unfrozenWriter;
    CHECK(!WorldArchive::write(unfrozenWriter, unfrozenWorld, schemas));
    CHECK(unfrozenWriter.bytes().empty());
    schemas.freeze();

    World world;
    const Entity entityA = world.create();
    const Entity destroyed = world.create();
    const Entity entityB = world.create();
    world.emplace<Position>(entityA, Position{1, 2});
    world.emplace<Health>(entityA, Health{10});
    world.emplace<Position>(destroyed, Position{99, 99});
    CHECK(world.destroy(destroyed));
    const Entity entityC = world.create();
    world.emplace<Position>(entityC, Position{5, 6});
    world.emplace<Health>(entityB, Health{20});

    foundation::BinaryWriter writer;
    CHECK(WorldArchive::write(writer, world, schemas));
    CHECK(!writer.bytes().empty());

    World restored;
    foundation::BinaryReader reader(writer.bytes());
    CHECK(WorldArchive::read(reader, schemas, restored));
    CHECK(reader.atEnd());
    CHECK(restored.capacity() == world.capacity());
    CHECK(restored.alive(entityA));
    CHECK(restored.alive(entityB));
    CHECK(restored.alive(entityC));
    CHECK(!restored.alive(destroyed));
    CHECK(restored.try_get<Position>(entityA)->x == 1);
    CHECK(restored.try_get<Position>(entityA)->y == 2);
    CHECK(restored.try_get<Health>(entityA)->value == 10);
    CHECK(restored.try_get<Health>(entityB)->value == 20);
    CHECK(restored.try_get<Position>(entityC)->x == 5);
    CHECK(restored.try_get<Position>(entityC)->y == 6);
    CHECK(restored.view<Position>().size() == 2);
    CHECK(restored.view<Health>().size() == 2);

    CHECK(world.create() == restored.create());
    foundation::BinaryWriter continuedOriginal;
    foundation::BinaryWriter continuedRestored;
    CHECK(WorldArchive::write(continuedOriginal, world, schemas));
    CHECK(WorldArchive::write(continuedRestored, restored, schemas));
    CHECK(continuedOriginal.bytes() == continuedRestored.bytes());

    World reordered;
    const Entity reorderedA = reordered.create();
    const Entity reorderedDestroyed = reordered.create();
    const Entity reorderedB = reordered.create();
    reordered.emplace<Health>(reorderedA, Health{10});
    reordered.emplace<Position>(reorderedDestroyed, Position{99, 99});
    reordered.emplace<Position>(reorderedA, Position{1, 2});
    CHECK(reordered.destroy(reorderedDestroyed));
    const Entity reorderedC = reordered.create();
    reordered.emplace<Health>(reorderedB, Health{20});
    reordered.emplace<Position>(reorderedC, Position{5, 6});
    reordered.create();
    CHECK(reorderedA == entityA);
    CHECK(reorderedB == entityB);
    CHECK(reorderedC == entityC);

    foundation::BinaryWriter reorderedWriter;
    CHECK(WorldArchive::write(reorderedWriter, reordered, schemas));
    CHECK(reorderedWriter.bytes() == continuedOriginal.bytes());

    World unknownComponentWorld;
    const Entity unknownEntity = unknownComponentWorld.create();
    unknownComponentWorld.emplace<TransientMarker>(unknownEntity, TransientMarker{7});
    foundation::BinaryWriter unknownWriter;
    CHECK(!WorldArchive::write(unknownWriter, unknownComponentWorld, schemas));
    CHECK(unknownWriter.bytes().empty());

    auto truncated = writer.bytes();
    truncated.pop_back();
    World preserved;
    const Entity sentinel = preserved.create();
    preserved.emplace<Health>(sentinel, Health{77});
    foundation::BinaryReader truncatedReader(truncated);
    CHECK(!WorldArchive::read(truncatedReader, schemas, preserved));
    CHECK(preserved.alive(sentinel));
    CHECK(preserved.try_get<Health>(sentinel)->value == 77);

    auto unsupportedArchiveVersion = writer.bytes();
    unsupportedArchiveVersion[4] = 2u;
    foundation::BinaryReader versionReader(unsupportedArchiveVersion);
    CHECK(!WorldArchive::read(versionReader, schemas, preserved));
    CHECK(preserved.alive(sentinel));
    CHECK(preserved.try_get<Health>(sentinel)->value == 77);

    std::cout << "ecs persistence tests passed\n";
    return EXIT_SUCCESS;
}
