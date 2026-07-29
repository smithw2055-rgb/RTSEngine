#include <RTSEngine/Scripting/ScriptModPackage.h>

#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <map>
#include <utility>

namespace rts::scripting {
namespace {

constexpr std::uint32_t kKnownCapabilities =
    static_cast<std::uint32_t>(ScriptModCapability::GameplayRead) |
    static_cast<std::uint32_t>(ScriptModCapability::GameplayCommand) |
    static_cast<std::uint32_t>(ScriptModCapability::Presentation) |
    static_cast<std::uint32_t>(ScriptModCapability::FileRead) |
    static_cast<std::uint32_t>(ScriptModCapability::Network) |
    static_cast<std::uint32_t>(ScriptModCapability::WallClock) |
    static_cast<std::uint32_t>(ScriptModCapability::NativeExtension);

bool validString(const std::string& value) noexcept {
    return !value.empty() &&
           value.size() <= ScriptModPackageCodec::kMaximumStringBytes;
}

bool dependencyLess(
    const ScriptModDependency& left,
    const ScriptModDependency& right) noexcept {
    if (left.modId != right.modId) return left.modId < right.modId;
    if (left.version != right.version) return left.version < right.version;
    return left.optional < right.optional;
}

bool validManifest(const ScriptModManifest& manifest) noexcept {
    return validString(manifest.modId) && validString(manifest.version) &&
           validString(manifest.displayName) &&
           manifest.scriptBundle.type == assets::AssetType::ScriptBundle &&
           manifest.scriptBundle.id != 0 && manifest.bundlePayloadHash != 0 &&
           manifest.scriptIdentity.valid() &&
           manifest.instructionBudget != 0 &&
           manifest.maximumHeapBytes != 0 &&
           (static_cast<std::uint32_t>(manifest.capabilities) &
            ~kKnownCapabilities) == 0;
}

void hashManifest(
    foundation::CanonicalHash& hash,
    const ScriptModManifest& manifest) noexcept {
    hash.WriteString(manifest.modId);
    hash.WriteString(manifest.version);
    hash.WriteString(manifest.displayName);
    hash.WriteI32(manifest.priority);
    hash.WriteU32(static_cast<std::uint32_t>(manifest.capabilities));
    hash.WriteBool(manifest.authoritative);
    hash.WriteBool(manifest.strictDeterminism);
    hash.WriteBool(manifest.allowAot);
    hash.WriteU16(static_cast<std::uint16_t>(manifest.scriptBundle.type));
    hash.WriteU64(manifest.scriptBundle.id);
    hash.WriteU64(manifest.bundlePayloadHash);
    hash.WriteU32(manifest.scriptIdentity.sdkCompatibilityVersion);
    hash.WriteU32(manifest.scriptIdentity.gameSdkPackageVersion);
    hash.WriteU64(manifest.scriptIdentity.hostApiHash);
    hash.WriteU64(manifest.scriptIdentity.programContentHash);
    hash.WriteU64(manifest.instructionBudget);
    hash.WriteU64(manifest.maximumHeapBytes);
    hash.WriteU32(static_cast<std::uint32_t>(manifest.dependencies.size()));
    for (const auto& dependency : manifest.dependencies) {
        hash.WriteString(dependency.modId);
        hash.WriteString(dependency.version);
        hash.WriteBool(dependency.optional);
    }
}

void writeManifest(
    foundation::BinaryWriter& writer,
    const ScriptModManifest& manifest) {
    writer.writeString(manifest.modId);
    writer.writeString(manifest.version);
    writer.writeString(manifest.displayName);
    writer.writeI32(manifest.priority);
    writer.writeU32(static_cast<std::uint32_t>(manifest.capabilities));
    writer.writeBool(manifest.authoritative);
    writer.writeBool(manifest.strictDeterminism);
    writer.writeBool(manifest.allowAot);
    writer.writeU16(static_cast<std::uint16_t>(manifest.scriptBundle.type));
    writer.writeU64(manifest.scriptBundle.id);
    writer.writeU64(manifest.bundlePayloadHash);
    writer.writeU32(manifest.scriptIdentity.sdkCompatibilityVersion);
    writer.writeU32(manifest.scriptIdentity.gameSdkPackageVersion);
    writer.writeU64(manifest.scriptIdentity.hostApiHash);
    writer.writeU64(manifest.scriptIdentity.programContentHash);
    writer.writeU64(manifest.instructionBudget);
    writer.writeU64(manifest.maximumHeapBytes);
    writer.writeU32(static_cast<std::uint32_t>(manifest.dependencies.size()));
    for (const auto& dependency : manifest.dependencies) {
        writer.writeString(dependency.modId);
        writer.writeString(dependency.version);
        writer.writeBool(dependency.optional);
    }
}

bool readManifest(
    foundation::BinaryReader& reader,
    ScriptModManifest& manifest) {
    std::uint32_t capabilities = 0;
    std::uint16_t bundleType = 0;
    std::uint32_t dependencyCount = 0;
    if (!reader.readString(
            manifest.modId, ScriptModPackageCodec::kMaximumStringBytes) ||
        !reader.readString(
            manifest.version, ScriptModPackageCodec::kMaximumStringBytes) ||
        !reader.readString(
            manifest.displayName, ScriptModPackageCodec::kMaximumStringBytes) ||
        !reader.readI32(manifest.priority) ||
        !reader.readU32(capabilities) ||
        (capabilities & ~kKnownCapabilities) != 0 ||
        !reader.readBool(manifest.authoritative) ||
        !reader.readBool(manifest.strictDeterminism) ||
        !reader.readBool(manifest.allowAot) ||
        !reader.readU16(bundleType) ||
        bundleType != static_cast<std::uint16_t>(
            assets::AssetType::ScriptBundle) ||
        !reader.readU64(manifest.scriptBundle.id) ||
        manifest.scriptBundle.id == 0 ||
        !reader.readU64(manifest.bundlePayloadHash) ||
        manifest.bundlePayloadHash == 0 ||
        !reader.readU32(
            manifest.scriptIdentity.sdkCompatibilityVersion) ||
        !reader.readU32(
            manifest.scriptIdentity.gameSdkPackageVersion) ||
        !reader.readU64(manifest.scriptIdentity.hostApiHash) ||
        !reader.readU64(manifest.scriptIdentity.programContentHash) ||
        !reader.readU64(manifest.instructionBudget) ||
        !reader.readU64(manifest.maximumHeapBytes) ||
        !reader.readU32(dependencyCount) ||
        dependencyCount > ScriptModPackageCodec::kMaximumDependencies) {
        return false;
    }
    manifest.capabilities = static_cast<ScriptModCapability>(capabilities);
    manifest.scriptBundle.type = static_cast<assets::AssetType>(bundleType);
    manifest.dependencies.resize(dependencyCount);
    for (auto& dependency : manifest.dependencies) {
        if (!reader.readString(
                dependency.modId,
                ScriptModPackageCodec::kMaximumStringBytes) ||
            !reader.readString(
                dependency.version,
                ScriptModPackageCodec::kMaximumStringBytes) ||
            !reader.readBool(dependency.optional)) {
            return false;
        }
    }
    return validManifest(manifest);
}

void diagnostic(
    ScriptModResolveResult& result,
    ScriptModFailure failure,
    std::string modId,
    std::string message) {
    result.diagnostics.push_back(
        {failure, std::move(modId), std::move(message)});
}

bool capabilitiesAllowed(
    ScriptModCapability requested,
    ScriptModCapability allowed) noexcept {
    return (static_cast<std::uint32_t>(requested) &
            ~static_cast<std::uint32_t>(allowed)) == 0;
}

} // namespace

bool ScriptModPackageCodec::canonicalize(ScriptModPackage& package) {
    if (!validManifest(package.manifest) ||
        package.assets.empty() || package.assets.size() > kMaximumAssets ||
        package.manifest.dependencies.size() > kMaximumDependencies) {
        return false;
    }
    std::sort(
        package.manifest.dependencies.begin(),
        package.manifest.dependencies.end(), dependencyLess);
    for (std::size_t index = 0;
         index < package.manifest.dependencies.size(); ++index) {
        const auto& dependency = package.manifest.dependencies[index];
        if (!validString(dependency.modId) ||
            (!dependency.version.empty() &&
             dependency.version.size() > kMaximumStringBytes) ||
            dependency.modId == package.manifest.modId ||
            (index != 0 &&
             package.manifest.dependencies[index - 1].modId ==
                 dependency.modId)) {
            return false;
        }
    }

    for (auto& asset : package.assets) {
        if (!assets::CookedAssetCodec::canonicalize(asset)) return false;
    }
    std::sort(
        package.assets.begin(), package.assets.end(),
        [](const assets::CookedAsset& left,
           const assets::CookedAsset& right) {
            return left.key < right.key;
        });
    bool bundleFound = false;
    for (std::size_t index = 0; index < package.assets.size(); ++index) {
        const auto& asset = package.assets[index];
        if (index != 0 && package.assets[index - 1].key == asset.key) {
            return false;
        }
        if (asset.key == package.manifest.scriptBundle) {
            bundleFound = asset.payloadHash ==
                package.manifest.bundlePayloadHash;
        }
    }
    if (!bundleFound) return false;
    package.packageHash = canonicalHash(package);
    return package.packageHash != 0;
}

std::uint64_t ScriptModPackageCodec::canonicalHash(
    const ScriptModPackage& package) noexcept {
    foundation::CanonicalHash hash;
    hash.WriteU16(kVersion);
    hashManifest(hash, package.manifest);
    hash.WriteU32(static_cast<std::uint32_t>(package.assets.size()));
    for (const auto& asset : package.assets) {
        hash.WriteU16(static_cast<std::uint16_t>(asset.key.type));
        hash.WriteU64(asset.key.id);
        hash.WriteU32(asset.schemaVersion);
        hash.WriteU64(asset.payloadHash);
        hash.WriteU32(static_cast<std::uint32_t>(asset.dependencies.size()));
        for (const auto& dependency : asset.dependencies) {
            hash.WriteU16(static_cast<std::uint16_t>(dependency.key.type));
            hash.WriteU64(dependency.key.id);
            hash.WriteU32(dependency.minimumSchemaVersion);
        }
    }
    return hash.Value();
}

std::vector<std::uint8_t> ScriptModPackageCodec::encode(
    ScriptModPackage package) {
    if (!canonicalize(package)) return {};
    foundation::BinaryWriter writer;
    writer.writeU32(kMagic);
    writer.writeU16(kVersion);
    writer.writeU64(package.packageHash);
    writeManifest(writer, package.manifest);
    writer.writeU32(static_cast<std::uint32_t>(package.assets.size()));
    for (auto asset : package.assets) {
        const auto bytes = assets::CookedAssetCodec::encode(std::move(asset));
        if (bytes.empty() || bytes.size() > kMaximumAssetBytes) return {};
        writer.writeU32(static_cast<std::uint32_t>(bytes.size()));
        writer.writeBytes(bytes);
    }
    return writer.take();
}

bool ScriptModPackageCodec::decode(
    const std::vector<std::uint8_t>& bytes,
    ScriptModPackage& output) {
    foundation::BinaryReader reader(bytes);
    ScriptModPackage candidate;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint64_t storedHash = 0;
    std::uint32_t assetCount = 0;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        magic != kMagic || version != kVersion ||
        !reader.readU64(storedHash) || storedHash == 0 ||
        !readManifest(reader, candidate.manifest) ||
        !reader.readU32(assetCount) || assetCount == 0 ||
        assetCount > kMaximumAssets) {
        return false;
    }
    candidate.assets.reserve(assetCount);
    for (std::uint32_t index = 0; index < assetCount; ++index) {
        std::uint32_t size = 0;
        std::vector<std::uint8_t> assetBytes;
        assets::CookedAsset asset;
        if (!reader.readU32(size) || size == 0 ||
            size > kMaximumAssetBytes ||
            !reader.readBytes(size, assetBytes, kMaximumAssetBytes) ||
            !assets::CookedAssetCodec::decode(assetBytes, asset)) {
            return false;
        }
        candidate.assets.push_back(std::move(asset));
    }
    if (!reader.atEnd() || !canonicalize(candidate) ||
        candidate.packageHash != storedHash) {
        return false;
    }
    output = std::move(candidate);
    return true;
}

ScriptModResolveResult ScriptModResolver::resolve(
    const std::vector<ScriptModPackage>& packages,
    ScriptModPolicy policy) {
    ScriptModResolveResult result;
    if (packages.empty() || packages.size() > policy.maximumMods) {
        diagnostic(result, ScriptModFailure::LimitExceeded, {},
                   "Mod count is outside the configured limit");
        return result;
    }

    std::map<std::string, std::size_t> byId;
    for (std::size_t index = 0; index < packages.size(); ++index) {
        const auto& package = packages[index];
        const auto& manifest = package.manifest;
        if (!validManifest(manifest) ||
            package.assets.size() > policy.maximumAssetsPerMod ||
            package.packageHash == 0 ||
            package.packageHash != ScriptModPackageCodec::canonicalHash(package)) {
            diagnostic(result, ScriptModFailure::InvalidPackage,
                       manifest.modId, "Mod package is not canonical");
            continue;
        }
        if (!byId.emplace(manifest.modId, index).second) {
            diagnostic(result, ScriptModFailure::DuplicateMod,
                       manifest.modId, "Duplicate Mod ID");
        }
        const auto allowed = manifest.authoritative
            ? policy.allowedAuthoritativeCapabilities
            : policy.allowedPresentationCapabilities;
        if (!capabilitiesAllowed(manifest.capabilities, allowed)) {
            diagnostic(result, ScriptModFailure::CapabilityDenied,
                       manifest.modId, "Requested Mod capability is denied");
        }
        if (manifest.authoritative &&
            policy.requireStrictAuthoritativeDeterminism &&
            !manifest.strictDeterminism) {
            diagnostic(result, ScriptModFailure::CapabilityDenied,
                       manifest.modId,
                       "Authoritative Mods must use Strict determinism");
        }
        if (manifest.instructionBudget > policy.maximumInstructionBudget ||
            manifest.maximumHeapBytes > policy.maximumHeapBytes) {
            diagnostic(result, ScriptModFailure::BudgetExceeded,
                       manifest.modId, "Mod execution budget exceeds policy");
        }
        if (manifest.allowAot && !policy.allowAot) {
            diagnostic(result, ScriptModFailure::AotDenied,
                       manifest.modId, "AOT is disabled by Mod policy");
        }
    }
    if (!result.diagnostics.empty()) return result;

    std::vector<std::uint32_t> indegree(packages.size(), 0);
    std::vector<std::vector<std::size_t>> outgoing(packages.size());
    for (std::size_t index = 0; index < packages.size(); ++index) {
        for (const auto& dependency : packages[index].manifest.dependencies) {
            const auto found = byId.find(dependency.modId);
            if (found == byId.end()) {
                if (!dependency.optional) {
                    diagnostic(result, ScriptModFailure::MissingDependency,
                               packages[index].manifest.modId,
                               "Required Mod dependency is missing: " +
                                   dependency.modId);
                }
                continue;
            }
            const auto& actual = packages[found->second].manifest.version;
            if (!dependency.version.empty() &&
                dependency.version != actual) {
                diagnostic(result,
                           ScriptModFailure::DependencyVersionMismatch,
                           packages[index].manifest.modId,
                           "Mod dependency version mismatch: " +
                               dependency.modId);
                continue;
            }
            ++indegree[index];
            outgoing[found->second].push_back(index);
        }
    }
    if (!result.diagnostics.empty()) return result;

    const auto readyLess = [&](std::size_t left, std::size_t right) {
        const auto& a = packages[left].manifest;
        const auto& b = packages[right].manifest;
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.modId < b.modId;
    };
    std::vector<std::size_t> ready;
    for (std::size_t index = 0; index < packages.size(); ++index) {
        if (indegree[index] == 0) ready.push_back(index);
    }
    std::sort(ready.begin(), ready.end(), readyLess);
    while (!ready.empty()) {
        const auto current = ready.front();
        ready.erase(ready.begin());
        result.loadOrder.push_back(current);
        for (const auto dependent : outgoing[current]) {
            if (--indegree[dependent] == 0) {
                ready.push_back(dependent);
                std::sort(ready.begin(), ready.end(), readyLess);
            }
        }
    }
    if (result.loadOrder.size() != packages.size()) {
        diagnostic(result, ScriptModFailure::DependencyCycle, {},
                   "Mod dependency graph contains a cycle");
        result.loadOrder.clear();
        return result;
    }

    foundation::CanonicalHash hash;
    hash.WriteU16(ScriptModPackageCodec::kVersion);
    hash.WriteU32(static_cast<std::uint32_t>(result.loadOrder.size()));
    for (const auto index : result.loadOrder) {
        hash.WriteString(packages[index].manifest.modId);
        hash.WriteString(packages[index].manifest.version);
        hash.WriteU64(packages[index].packageHash);
    }
    result.modSetHash = hash.Value();
    return result;
}

std::uint64_t CombineScriptModContentHash(
    std::uint64_t baseContentHash,
    std::uint64_t modSetHash) noexcept {
    foundation::CanonicalHash hash;
    hash.WriteString("RTSEngine.ScriptMods.v1");
    hash.WriteU64(baseContentHash);
    hash.WriteU64(modSetHash);
    return hash.Value();
}

const char* ScriptModFailureName(ScriptModFailure failure) noexcept {
    switch (failure) {
        case ScriptModFailure::None: return "none";
        case ScriptModFailure::InvalidPackage: return "invalid-package";
        case ScriptModFailure::DuplicateMod: return "duplicate-mod";
        case ScriptModFailure::MissingDependency: return "missing-dependency";
        case ScriptModFailure::DependencyVersionMismatch:
            return "dependency-version-mismatch";
        case ScriptModFailure::DependencyCycle: return "dependency-cycle";
        case ScriptModFailure::CapabilityDenied: return "capability-denied";
        case ScriptModFailure::BudgetExceeded: return "budget-exceeded";
        case ScriptModFailure::AotDenied: return "aot-denied";
        case ScriptModFailure::LimitExceeded: return "limit-exceeded";
    }
    return "unknown";
}

} // namespace rts::scripting
