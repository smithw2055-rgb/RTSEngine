#pragma once

#include <RTSEngine/Assets/AssetTypes.h>
#include <RTSEngine/Navigation/NavigationWorld.h>
#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace rts::gameplay {

using WorldMapId = std::uint64_t;
using WorldObjectId = std::uint64_t;

struct WorldMapCell final {
    std::uint8_t terrainType{};
    navigation::MovementDomainMask staticBlockedMask{};
};

enum class WorldSpawnKind : std::uint8_t {
    Unit,
    Building,
    Prop
};

struct WorldSpawn final {
    WorldObjectId id{};
    WorldSpawnKind kind{WorldSpawnKind::Unit};
    std::uint32_t definitionId{};
    std::uint32_t teamId{};
    navigation::GridPoint position{};
    std::uint8_t facing{};
};

struct WorldResourceNode final {
    WorldObjectId id{};
    std::uint32_t resourceType{};
    std::int32_t amount{};
    navigation::GridPoint position{};
};

struct WorldRoute final {
    WorldObjectId id{};
    std::uint32_t navProfileId{1};
    navigation::GridPoint start{};
    navigation::GridPoint goal{};
    bool required{};
};

struct WorldLane final {
    WorldObjectId id{};
    std::vector<navigation::GridPoint> points;
};

struct WorldTriggerZone final {
    WorldObjectId id{};
    navigation::GridPoint minimum{};
    navigation::GridPoint maximum{};
    std::uint64_t scriptEventId{};
};

struct WorldMapDefinition final {
    static constexpr std::uint32_t kSchemaVersion = 1;

    std::uint32_t schemaVersion{kSchemaVersion};
    WorldMapId id{};
    std::string name;
    std::int32_t width{};
    std::int32_t height{};
    std::vector<WorldMapCell> cells;
    std::vector<WorldSpawn> spawns;
    std::vector<WorldResourceNode> resources;
    std::vector<WorldRoute> routes;
    std::vector<WorldLane> lanes;
    std::vector<WorldTriggerZone> triggerZones;
};

enum class WorldMapIssueCode : std::uint8_t {
    None,
    InvalidSchema,
    InvalidIdentity,
    InvalidDimensions,
    InvalidCellCount,
    DuplicateObjectId,
    InvalidDefinition,
    InvalidTeam,
    OutOfBounds,
    InvalidResource,
    InvalidRoute,
    InvalidLane,
    InvalidTriggerZone,
    CapacityExceeded,
    TrailingData
};

struct WorldMapIssue final {
    WorldMapIssueCode code{WorldMapIssueCode::None};
    WorldObjectId objectId{};
};

struct WorldMapValidation final {
    std::vector<WorldMapIssue> issues;

    explicit operator bool() const noexcept { return issues.empty(); }
};

struct WorldMapLimits final {
    std::uint32_t maximumCells{navigation::NavigationWorld::kMaximumCells};
    std::uint32_t maximumSpawns{65536};
    std::uint32_t maximumResources{16384};
    std::uint32_t maximumRoutes{4096};
    std::uint32_t maximumLanes{4096};
    std::uint32_t maximumLanePoints{262144};
    std::uint32_t maximumTriggerZones{16384};
    std::uint32_t maximumNameBytes{1024};
};

class WorldMap final {
public:
    static void canonicalize(WorldMapDefinition& map) {
        const auto byId = [](const auto& a, const auto& b) {
            return a.id < b.id;
        };
        std::sort(map.spawns.begin(), map.spawns.end(), byId);
        std::sort(map.resources.begin(), map.resources.end(), byId);
        std::sort(map.routes.begin(), map.routes.end(), byId);
        std::sort(map.lanes.begin(), map.lanes.end(), byId);
        std::sort(map.triggerZones.begin(), map.triggerZones.end(), byId);
    }

    static WorldMapValidation validate(
        const WorldMapDefinition& map,
        const WorldMapLimits& limits = {}) {
        WorldMapValidation result;
        if (map.schemaVersion != WorldMapDefinition::kSchemaVersion) {
            result.issues.push_back({WorldMapIssueCode::InvalidSchema, 0});
        }
        if (map.id == 0 || map.name.empty() ||
            map.name.size() > limits.maximumNameBytes) {
            result.issues.push_back({WorldMapIssueCode::InvalidIdentity, 0});
        }
        if (map.width <= 0 || map.height <= 0) {
            result.issues.push_back({WorldMapIssueCode::InvalidDimensions, 0});
            return result;
        }
        const auto expectedCells = static_cast<std::uint64_t>(map.width) *
                                   static_cast<std::uint64_t>(map.height);
        if (expectedCells == 0 || expectedCells > limits.maximumCells ||
            expectedCells != map.cells.size()) {
            result.issues.push_back({WorldMapIssueCode::InvalidCellCount, 0});
        }
        if (map.spawns.size() > limits.maximumSpawns ||
            map.resources.size() > limits.maximumResources ||
            map.routes.size() > limits.maximumRoutes ||
            map.lanes.size() > limits.maximumLanes ||
            map.triggerZones.size() > limits.maximumTriggerZones) {
            result.issues.push_back({WorldMapIssueCode::CapacityExceeded, 0});
        }

        std::vector<WorldObjectId> ids;
        ids.reserve(
            map.spawns.size() + map.resources.size() + map.routes.size() +
            map.lanes.size() + map.triggerZones.size());
        const auto acceptId = [&](WorldObjectId id) {
            if (id == 0) {
                result.issues.push_back({WorldMapIssueCode::InvalidIdentity, id});
                return;
            }
            ids.push_back(id);
        };

        for (const auto& spawn : map.spawns) {
            acceptId(spawn.id);
            if (spawn.definitionId == 0) {
                result.issues.push_back(
                    {WorldMapIssueCode::InvalidDefinition, spawn.id});
            }
            if (spawn.kind != WorldSpawnKind::Prop && spawn.teamId == 0) {
                result.issues.push_back(
                    {WorldMapIssueCode::InvalidTeam, spawn.id});
            }
            if (!contains(map, spawn.position)) {
                result.issues.push_back(
                    {WorldMapIssueCode::OutOfBounds, spawn.id});
            }
        }
        for (const auto& resource : map.resources) {
            acceptId(resource.id);
            if (resource.resourceType == 0 || resource.amount <= 0) {
                result.issues.push_back(
                    {WorldMapIssueCode::InvalidResource, resource.id});
            }
            if (!contains(map, resource.position)) {
                result.issues.push_back(
                    {WorldMapIssueCode::OutOfBounds, resource.id});
            }
        }
        for (const auto& route : map.routes) {
            acceptId(route.id);
            if (route.navProfileId == 0 || !contains(map, route.start) ||
                !contains(map, route.goal) || route.start == route.goal) {
                result.issues.push_back(
                    {WorldMapIssueCode::InvalidRoute, route.id});
            }
        }
        std::uint64_t lanePointCount = 0;
        for (const auto& lane : map.lanes) {
            acceptId(lane.id);
            lanePointCount += lane.points.size();
            if (lane.points.size() < 2 ||
                std::any_of(
                    lane.points.begin(), lane.points.end(),
                    [&](navigation::GridPoint point) {
                        return !contains(map, point);
                    })) {
                result.issues.push_back(
                    {WorldMapIssueCode::InvalidLane, lane.id});
            }
        }
        if (lanePointCount > limits.maximumLanePoints) {
            result.issues.push_back({WorldMapIssueCode::CapacityExceeded, 0});
        }
        for (const auto& zone : map.triggerZones) {
            acceptId(zone.id);
            if (zone.scriptEventId == 0 || !contains(map, zone.minimum) ||
                !contains(map, zone.maximum) ||
                zone.minimum.x > zone.maximum.x ||
                zone.minimum.y > zone.maximum.y) {
                result.issues.push_back(
                    {WorldMapIssueCode::InvalidTriggerZone, zone.id});
            }
        }

        std::sort(ids.begin(), ids.end());
        for (std::size_t index = 1; index < ids.size(); ++index) {
            if (ids[index] == ids[index - 1]) {
                result.issues.push_back(
                    {WorldMapIssueCode::DuplicateObjectId, ids[index]});
            }
        }
        return result;
    }

    static std::uint64_t contentHash(const WorldMapDefinition& source) {
        auto map = source;
        canonicalize(map);
        std::vector<std::uint8_t> bytes;
        if (!encodeCanonical(map, bytes)) return 0;
        std::uint64_t hash = 1469598103934665603ull;
        for (const auto byte : bytes) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        return hash == 0 ? 1 : hash;
    }

    static bool buildNavigationWorld(
        const WorldMapDefinition& map,
        navigation::NavigationWorld& output) {
        if (!validate(map)) return false;
        navigation::NavigationWorld candidate(map.width, map.height);
        for (std::int32_t y = 0; y < map.height; ++y) {
            for (std::int32_t x = 0; x < map.width; ++x) {
                const navigation::GridPoint point{x, y};
                const auto& cell = map.cells[static_cast<std::size_t>(y) *
                                             static_cast<std::size_t>(map.width) +
                                             static_cast<std::size_t>(x)];
                if (!candidate.setTerrain(point, cell.terrainType) ||
                    !candidate.setStaticBlocked(
                        point, cell.staticBlockedMask, true)) {
                    return false;
                }
            }
        }
        output = std::move(candidate);
        return true;
    }

    static bool encode(
        const WorldMapDefinition& source,
        std::vector<std::uint8_t>& output) {
        auto map = source;
        canonicalize(map);
        if (!validate(map)) return false;
        return encodeCanonical(map, output);
    }

    static bool decode(
        const std::vector<std::uint8_t>& bytes,
        WorldMapDefinition& output,
        const WorldMapLimits& limits = {}) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        WorldMapDefinition candidate;
        if (!reader.readU32(magic) || magic != kMagic ||
            !reader.readU32(candidate.schemaVersion) ||
            !reader.readU64(candidate.id) ||
            !reader.readString(candidate.name, limits.maximumNameBytes) ||
            !reader.readI32(candidate.width) ||
            !reader.readI32(candidate.height)) {
            return false;
        }
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > limits.maximumCells) return false;
        candidate.cells.resize(count);
        for (auto& cell : candidate.cells) {
            if (!reader.readU8(cell.terrainType) ||
                !reader.readU8(cell.staticBlockedMask) ||
                (cell.staticBlockedMask & ~navigation::kAllMovementDomains) != 0) {
                return false;
            }
        }
        if (!readVector(reader, candidate.spawns, limits.maximumSpawns,
                        readSpawn) ||
            !readVector(reader, candidate.resources, limits.maximumResources,
                        readResource) ||
            !readVector(reader, candidate.routes, limits.maximumRoutes,
                        readRoute) ||
            !readLanes(reader, candidate.lanes, limits) ||
            !readVector(reader, candidate.triggerZones,
                        limits.maximumTriggerZones, readTriggerZone) ||
            !reader.atEnd()) {
            return false;
        }
        canonicalize(candidate);
        if (!validate(candidate, limits)) return false;
        output = std::move(candidate);
        return true;
    }

private:
    static constexpr std::uint32_t kMagic = 0x4d575452u; // RTWM

    static bool contains(
        const WorldMapDefinition& map,
        navigation::GridPoint point) noexcept {
        return point.x >= 0 && point.y >= 0 &&
               point.x < map.width && point.y < map.height;
    }

    static void writePoint(
        foundation::BinaryWriter& writer,
        navigation::GridPoint point) {
        writer.writeI32(point.x);
        writer.writeI32(point.y);
    }

    static bool readPoint(
        foundation::BinaryReader& reader,
        navigation::GridPoint& point) {
        return reader.readI32(point.x) && reader.readI32(point.y);
    }

    static bool encodeCanonical(
        const WorldMapDefinition& map,
        std::vector<std::uint8_t>& output) {
        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU32(map.schemaVersion);
        writer.writeU64(map.id);
        writer.writeString(map.name);
        writer.writeI32(map.width);
        writer.writeI32(map.height);
        writer.writeU32(static_cast<std::uint32_t>(map.cells.size()));
        for (const auto& cell : map.cells) {
            writer.writeU8(cell.terrainType);
            writer.writeU8(cell.staticBlockedMask);
        }
        writer.writeU32(static_cast<std::uint32_t>(map.spawns.size()));
        for (const auto& spawn : map.spawns) {
            writer.writeU64(spawn.id);
            writer.writeU8(static_cast<std::uint8_t>(spawn.kind));
            writer.writeU32(spawn.definitionId);
            writer.writeU32(spawn.teamId);
            writePoint(writer, spawn.position);
            writer.writeU8(spawn.facing);
        }
        writer.writeU32(static_cast<std::uint32_t>(map.resources.size()));
        for (const auto& resource : map.resources) {
            writer.writeU64(resource.id);
            writer.writeU32(resource.resourceType);
            writer.writeI32(resource.amount);
            writePoint(writer, resource.position);
        }
        writer.writeU32(static_cast<std::uint32_t>(map.routes.size()));
        for (const auto& route : map.routes) {
            writer.writeU64(route.id);
            writer.writeU32(route.navProfileId);
            writePoint(writer, route.start);
            writePoint(writer, route.goal);
            writer.writeBool(route.required);
        }
        writer.writeU32(static_cast<std::uint32_t>(map.lanes.size()));
        for (const auto& lane : map.lanes) {
            writer.writeU64(lane.id);
            writer.writeU32(static_cast<std::uint32_t>(lane.points.size()));
            for (const auto point : lane.points) writePoint(writer, point);
        }
        writer.writeU32(static_cast<std::uint32_t>(map.triggerZones.size()));
        for (const auto& zone : map.triggerZones) {
            writer.writeU64(zone.id);
            writePoint(writer, zone.minimum);
            writePoint(writer, zone.maximum);
            writer.writeU64(zone.scriptEventId);
        }
        output = writer.take();
        return true;
    }

    template<class T, class Reader>
    static bool readVector(
        foundation::BinaryReader& reader,
        std::vector<T>& output,
        std::uint32_t maximum,
        Reader itemReader) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > maximum) return false;
        output.resize(count);
        for (auto& item : output) {
            if (!itemReader(reader, item)) return false;
        }
        return true;
    }

    static bool readSpawn(
        foundation::BinaryReader& reader,
        WorldSpawn& spawn) {
        std::uint8_t kind = 0;
        if (!reader.readU64(spawn.id) || !reader.readU8(kind) ||
            kind > static_cast<std::uint8_t>(WorldSpawnKind::Prop) ||
            !reader.readU32(spawn.definitionId) ||
            !reader.readU32(spawn.teamId) ||
            !readPoint(reader, spawn.position) ||
            !reader.readU8(spawn.facing)) {
            return false;
        }
        spawn.kind = static_cast<WorldSpawnKind>(kind);
        return true;
    }

    static bool readResource(
        foundation::BinaryReader& reader,
        WorldResourceNode& resource) {
        return reader.readU64(resource.id) &&
               reader.readU32(resource.resourceType) &&
               reader.readI32(resource.amount) &&
               readPoint(reader, resource.position);
    }

    static bool readRoute(
        foundation::BinaryReader& reader,
        WorldRoute& route) {
        return reader.readU64(route.id) &&
               reader.readU32(route.navProfileId) &&
               readPoint(reader, route.start) &&
               readPoint(reader, route.goal) &&
               reader.readBool(route.required);
    }

    static bool readTriggerZone(
        foundation::BinaryReader& reader,
        WorldTriggerZone& zone) {
        return reader.readU64(zone.id) &&
               readPoint(reader, zone.minimum) &&
               readPoint(reader, zone.maximum) &&
               reader.readU64(zone.scriptEventId);
    }

    static bool readLanes(
        foundation::BinaryReader& reader,
        std::vector<WorldLane>& lanes,
        const WorldMapLimits& limits) {
        std::uint32_t count = 0;
        if (!reader.readU32(count) || count > limits.maximumLanes) return false;
        lanes.resize(count);
        std::uint64_t totalPoints = 0;
        for (auto& lane : lanes) {
            std::uint32_t pointCount = 0;
            if (!reader.readU64(lane.id) || !reader.readU32(pointCount)) {
                return false;
            }
            totalPoints += pointCount;
            if (totalPoints > limits.maximumLanePoints) return false;
            lane.points.resize(pointCount);
            for (auto& point : lane.points) {
                if (!readPoint(reader, point)) return false;
            }
        }
        return true;
    }
};

class WorldMapAssetCodec final {
public:
    static bool cook(
        const WorldMapDefinition& map,
        assets::CookedAsset& output) {
        std::vector<std::uint8_t> payload;
        if (!WorldMap::encode(map, payload)) return false;
        assets::CookedAsset candidate;
        candidate.key = {assets::AssetType::WorldMap, map.id};
        candidate.schemaVersion = WorldMapDefinition::kSchemaVersion;
        candidate.payloadHash = WorldMap::contentHash(map);
        candidate.payload = std::move(payload);
        if (!candidate.key.valid() || candidate.payloadHash == 0) return false;
        output = std::move(candidate);
        return true;
    }

    static bool decode(
        const assets::CookedAsset& cooked,
        WorldMapDefinition& output,
        const WorldMapLimits& limits = {}) {
        if (cooked.key.type != assets::AssetType::WorldMap ||
            cooked.key.id == 0 ||
            cooked.schemaVersion != WorldMapDefinition::kSchemaVersion ||
            cooked.payloadHash == 0) {
            return false;
        }
        WorldMapDefinition candidate;
        if (!WorldMap::decode(cooked.payload, candidate, limits) ||
            candidate.id != cooked.key.id ||
            WorldMap::contentHash(candidate) != cooked.payloadHash) {
            return false;
        }
        output = std::move(candidate);
        return true;
    }
};

struct WorldBootstrapPlan final {
    std::uint64_t contentHash{};
    navigation::NavigationWorld navigation;
    std::vector<WorldSpawn> spawns;
    std::vector<WorldResourceNode> resources;
    std::vector<WorldRoute> requiredRoutes;
    std::vector<WorldLane> lanes;
    std::vector<WorldTriggerZone> triggerZones;
};

inline bool BuildWorldBootstrapPlan(
    const WorldMapDefinition& source,
    WorldBootstrapPlan& output) {
    auto map = source;
    WorldMap::canonicalize(map);
    if (!WorldMap::validate(map)) return false;
    WorldBootstrapPlan candidate;
    if (!WorldMap::buildNavigationWorld(map, candidate.navigation)) {
        return false;
    }
    candidate.contentHash = WorldMap::contentHash(map);
    candidate.spawns = map.spawns;
    candidate.resources = map.resources;
    candidate.lanes = map.lanes;
    candidate.triggerZones = map.triggerZones;
    for (const auto& route : map.routes) {
        if (route.required) candidate.requiredRoutes.push_back(route);
    }
    output = std::move(candidate);
    return true;
}

} // namespace rts::gameplay
