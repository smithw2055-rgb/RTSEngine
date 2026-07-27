#include <RTSEngine/Ecs/ComponentSchema.h>
#include <RTSEngine/Ecs/EntityCommandBuffer.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

struct Position {
    std::int32_t x{};
    std::int32_t y{};
};

struct Health {
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

    std::uint32_t mutableReferenceCount = 0;
    world.eachRef<Position, Health>(
        [&](Entity entity, Position& position, Health& health) {
            CHECK(entity == complete);
            health.value += position.x;
            ++mutableReferenceCount;
        });
    CHECK(mutableReferenceCount == 1);
    CHECK(world.try_get<Health>(complete)->value == 13);

    const World& constWorld = world;
    std::int32_t constReferenceTotal = 0;
    constWorld.eachRef<Position, Health>(
        [&](Entity entity, const Position& position, const Health& health) {
            if (entity == complete) {
                constReferenceTotal += position.x + position.y + health.value;
            }
        });
    CHECK(constReferenceTotal == 20);

    Scheduler scheduler;
    std::vector<int> executionOrder;
    scheduler.add(Stage::Simulation, 1, 1,
                  [&](World&, const SystemContext&) { executionOrder.push_back(2); });
    scheduler.add(Stage::Simulation, 0, 100,
                  [&](World&, const SystemContext&) { executionOrder.push_back(1); });
    scheduler.run(world, 7);
    CHECK((executionOrder == std::vector<int>{1, 2}));

    EntityCommandBuffer commands;
    commands.reserve(16, 4);
    const auto reservedCommandCapacity = commands.commandCapacity();
    CHECK(reservedCommandCapacity >= 16);
    const SystemContext simulation{1, 0, Stage::Simulation};
    const DeferredEntity deferred = commands.create(simulation);
    commands.add<Position>(simulation, deferred, Position{9, 9});
    CHECK(world.view<Position>().size() == 2);

    commands.commit_through(world, Stage::Simulation);
    const auto afterCreate = world.view<Position>();
    CHECK(afterCreate.size() == 3);
    CHECK(world.try_get<Position>(afterCreate.back()) != nullptr);
    CHECK(commands.commandCapacity() == reservedCommandCapacity);

    const Entity gated = world.create();
    const SystemContext cleanup{1, 4, Stage::Cleanup};
    commands.add<Health>(cleanup, gated, Health{7});
    commands.commit_through(world, Stage::Simulation);
    CHECK(world.try_get<Health>(gated) == nullptr);
    commands.commit_through(world, Stage::Cleanup);
    CHECK(world.try_get<Health>(gated)->value == 7);
    CHECK(commands.commandCapacity() == reservedCommandCapacity);

    const Entity removable = world.create();
    world.emplace<Health>(removable, Health{5});
    commands.remove<Health>(simulation, removable);
    commands.destroy(cleanup, removable);
    commands.commit_through(world, Stage::Simulation);
    CHECK(world.alive(removable));
    CHECK(world.try_get<Health>(removable) == nullptr);
    commands.commit_through(world, Stage::Cleanup);
    CHECK(!world.alive(removable));
    CHECK(commands.commandCapacity() == reservedCommandCapacity);

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

    CHECK(!schemas.registerSchema<Health>(
        0x1001u, 1u, "DuplicateId",
        [](foundation::BinaryWriter&, const Health&) {},
        [](foundation::BinaryReader&, ComponentSchemaVersion, Health&) {
            return true;
        },
        [](foundation::CanonicalHash&, const Health&) {}));
    CHECK(!schemas.registerSchema<Position>(
        0x3000u, 1u, "DuplicateType",
        [](foundation::BinaryWriter&, const Position&) {},
        [](foundation::BinaryReader&, ComponentSchemaVersion, Position&) {
            return true;
        },
        [](foundation::CanonicalHash&, const Position&) {}));

    const auto descriptors = schemas.descriptors();
    CHECK(descriptors.size() == 2);
    CHECK(descriptors[0].typeId == 0x1001u);
    CHECK(descriptors[0].name == "Position");
    CHECK(descriptors[1].typeId == 0x2000u);
    CHECK(schemas.find<Position>()->version == 1u);
    CHECK(schemas.find(0x2000u)->name == "Health");

    schemas.freeze();
    CHECK(schemas.frozen());
    CHECK(!schemas.registerSchema<std::int32_t>(
        0x3000u, 1u, "LateRegistration",
        [](foundation::BinaryWriter&, const std::int32_t&) {},
        [](foundation::BinaryReader&, ComponentSchemaVersion, std::int32_t&) {
            return true;
        },
        [](foundation::CanonicalHash&, const std::int32_t&) {}));

    const Position original{-7, 42};
    foundation::BinaryWriter componentWriter;
    CHECK(schemas.write(componentWriter, original));

    Position restored;
    foundation::BinaryReader componentReader(componentWriter.bytes());
    CHECK(schemas.read(componentReader, restored));
    CHECK(componentReader.atEnd());
    CHECK(restored.x == original.x);
    CHECK(restored.y == original.y);

    foundation::CanonicalHash originalHash;
    foundation::CanonicalHash restoredHash;
    CHECK(schemas.hash(originalHash, original));
    CHECK(schemas.hash(restoredHash, restored));
    CHECK(originalHash.Value() == restoredHash.Value());

    auto unsupportedVersion = componentWriter.bytes();
    unsupportedVersion[4] = 2u;
    foundation::BinaryReader versionReader(unsupportedVersion);
    Position rejectedVersion;
    CHECK(!schemas.read(versionReader, rejectedVersion));

    foundation::BinaryWriter truncatedWriter;
    truncatedWriter.writeU32(0x1001u);
    truncatedWriter.writeU16(1u);
    truncatedWriter.writeU32(100u);
    truncatedWriter.writeU8(0u);
    foundation::BinaryReader truncatedReader(truncatedWriter.bytes());
    Position rejectedTruncated;
    CHECK(!schemas.read(truncatedReader, rejectedTruncated));

    std::cout << "ecs tests passed\n";
    return EXIT_SUCCESS;
}
