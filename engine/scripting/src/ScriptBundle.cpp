#include <RTSEngine/Scripting/ScriptBundle.h>

#include <RTSEngine/Assets/CookedAsset.h>
#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <utility>

namespace rts::scripting {
namespace {

constexpr std::uint32_t kScriptBundleMagic = 0x31425352u; // RSB1

void report(
    ScriptDiagnostics* diagnostics,
    std::string code,
    std::string message) {
    if (diagnostics) {
        diagnostics->add(std::move(code), std::move(message));
    }
}

} // namespace

assets::CookedAsset ScriptAssetCodec::moduleAsset(
    std::uint64_t assetId,
    std::vector<std::uint8_t> encodedModule) {
    assets::CookedAsset asset;
    if (assetId == 0 || encodedModule.empty()) return asset;
    asset.key = {assets::AssetType::ScriptModule, assetId};
    asset.schemaVersion = kScriptModuleSchemaVersion;
    asset.payload = std::move(encodedModule);
    if (!assets::CookedAssetCodec::canonicalize(asset)) return {};
    return asset;
}

bool ScriptAssetCodec::decodeModule(
    const assets::CookedAsset& asset,
    std::vector<std::uint8_t>& encodedModule,
    ScriptDiagnostics* diagnostics) {
    if (asset.key.type != assets::AssetType::ScriptModule ||
        asset.key.id == 0 ||
        asset.schemaVersion != kScriptModuleSchemaVersion ||
        asset.payload.empty() ||
        asset.payloadHash != assets::CookedAssetCodec::payloadHash(asset.payload)) {
        report(diagnostics,
               "RTSRS1001",
               "script module asset is invalid or uses an unsupported schema");
        return false;
    }
    encodedModule = asset.payload;
    return true;
}

bool ScriptAssetCodec::canonicalize(
    ScriptBundle& bundle,
    ScriptDiagnostics* diagnostics) {
    if (!bundle.identity.valid()) {
        report(diagnostics,
               "RTSRS1002",
               "script bundle identity is incomplete");
        return false;
    }
    if (bundle.modules.empty() ||
        bundle.modules.size() > kMaximumScriptModules) {
        report(diagnostics,
               "RTSRS1003",
               "script bundle module count is outside the supported range");
        return false;
    }

    std::sort(
        bundle.modules.begin(),
        bundle.modules.end(),
        [](const ScriptModuleReference& first,
           const ScriptModuleReference& second) {
            return first.assetId < second.assetId;
        });
    for (std::size_t index = 0; index < bundle.modules.size(); ++index) {
        const auto& module = bundle.modules[index];
        if (module.assetId == 0 || module.payloadHash == 0 ||
            (index != 0 &&
             bundle.modules[index - 1].assetId == module.assetId)) {
            report(diagnostics,
                   "RTSRS1004",
                   "script bundle contains an invalid or duplicate module reference");
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> ScriptAssetCodec::encodeBundlePayload(
    const ScriptBundle& bundle) {
    foundation::BinaryWriter writer;
    writer.writeU32(kScriptBundleMagic);
    writer.writeU16(kScriptBundleFormatVersion);
    writer.writeU16(0);
    writer.writeU32(bundle.identity.sdkCompatibilityVersion);
    writer.writeU32(bundle.identity.gameSdkPackageVersion);
    writer.writeU64(bundle.identity.hostApiHash);
    writer.writeU64(bundle.identity.programContentHash);
    writer.writeU32(static_cast<std::uint32_t>(bundle.modules.size()));
    for (const auto& module : bundle.modules) {
        writer.writeU64(module.assetId);
        writer.writeU64(module.payloadHash);
    }
    return writer.take();
}

bool ScriptAssetCodec::decodeBundlePayload(
    const std::vector<std::uint8_t>& payload,
    ScriptBundle& bundle,
    ScriptDiagnostics* diagnostics) {
    if (payload.empty() || payload.size() > kMaximumScriptBundlePayloadBytes) {
        report(diagnostics,
               "RTSRS1005",
               "script bundle payload size is invalid");
        return false;
    }

    foundation::BinaryReader reader(payload);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint32_t moduleCount = 0;
    ScriptBundle candidate;
    if (!reader.readU32(magic) || !reader.readU16(version) ||
        !reader.readU16(reserved) ||
        magic != kScriptBundleMagic ||
        version != kScriptBundleFormatVersion || reserved != 0 ||
        !reader.readU32(candidate.identity.sdkCompatibilityVersion) ||
        !reader.readU32(candidate.identity.gameSdkPackageVersion) ||
        !reader.readU64(candidate.identity.hostApiHash) ||
        !reader.readU64(candidate.identity.programContentHash) ||
        !reader.readU32(moduleCount) || moduleCount == 0 ||
        moduleCount > kMaximumScriptModules) {
        report(diagnostics,
               "RTSRS1006",
               "script bundle payload header is invalid");
        return false;
    }

    candidate.modules.resize(moduleCount);
    for (auto& module : candidate.modules) {
        if (!reader.readU64(module.assetId) ||
            !reader.readU64(module.payloadHash)) {
            report(diagnostics,
                   "RTSRS1007",
                   "script bundle module table is truncated");
            return false;
        }
    }
    if (!reader.atEnd() || !canonicalize(candidate, diagnostics)) {
        if (!reader.atEnd()) {
            report(diagnostics,
                   "RTSRS1008",
                   "script bundle payload contains trailing data");
        }
        return false;
    }
    bundle = std::move(candidate);
    return true;
}

assets::CookedAsset ScriptAssetCodec::bundleAsset(
    std::uint64_t assetId,
    ScriptBundle bundle) {
    assets::CookedAsset asset;
    if (assetId == 0 || !canonicalize(bundle)) return asset;

    asset.key = {assets::AssetType::ScriptBundle, assetId};
    asset.schemaVersion = kScriptBundleSchemaVersion;
    asset.payload = encodeBundlePayload(bundle);
    asset.dependencies.reserve(bundle.modules.size());
    for (const auto& module : bundle.modules) {
        asset.dependencies.push_back(
            {{assets::AssetType::ScriptModule, module.assetId},
             kScriptModuleSchemaVersion});
    }
    if (!assets::CookedAssetCodec::canonicalize(asset)) return {};
    return asset;
}

bool ScriptAssetCodec::decodeBundle(
    const assets::CookedAsset& asset,
    ScriptBundle& bundle,
    ScriptDiagnostics* diagnostics) {
    if (asset.key.type != assets::AssetType::ScriptBundle ||
        asset.key.id == 0 ||
        asset.schemaVersion != kScriptBundleSchemaVersion ||
        asset.payloadHash != assets::CookedAssetCodec::payloadHash(asset.payload)) {
        report(diagnostics,
               "RTSRS1009",
               "script bundle asset is invalid or uses an unsupported schema");
        return false;
    }

    ScriptBundle candidate;
    if (!decodeBundlePayload(asset.payload, candidate, diagnostics) ||
        asset.dependencies.size() != candidate.modules.size()) {
        if (asset.dependencies.size() != candidate.modules.size()) {
            report(diagnostics,
                   "RTSRS1010",
                   "script bundle dependency table does not match its module table");
        }
        return false;
    }
    for (std::size_t index = 0; index < candidate.modules.size(); ++index) {
        const auto& dependency = asset.dependencies[index];
        if (dependency.key.type != assets::AssetType::ScriptModule ||
            dependency.key.id != candidate.modules[index].assetId ||
            dependency.minimumSchemaVersion != kScriptModuleSchemaVersion) {
            report(diagnostics,
                   "RTSRS1011",
                   "script bundle contains a mismatched module dependency");
            return false;
        }
    }
    bundle = std::move(candidate);
    return true;
}

} // namespace rts::scripting
