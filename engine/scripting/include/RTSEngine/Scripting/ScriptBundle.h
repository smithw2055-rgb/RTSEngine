#pragma once

#include <RTSEngine/Assets/AssetTypes.h>
#include <RTSEngine/Scripting/ScriptDiagnostics.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts::scripting {

inline constexpr std::uint32_t kScriptModuleSchemaVersion = 1u;
inline constexpr std::uint32_t kScriptBundleSchemaVersion = 1u;
inline constexpr std::uint16_t kScriptBundleFormatVersion = 1u;
inline constexpr std::uint32_t kMaximumScriptModules = 4096u;
inline constexpr std::size_t kMaximumScriptBundlePayloadBytes =
    16u * 1024u * 1024u;

struct ScriptModuleReference final {
    std::uint64_t assetId{};
    std::uint64_t payloadHash{};

    friend bool operator==(const ScriptModuleReference& first,
                           const ScriptModuleReference& second) noexcept {
        return first.assetId == second.assetId &&
               first.payloadHash == second.payloadHash;
    }
};

struct ScriptBundleIdentity final {
    std::uint32_t sdkCompatibilityVersion{};
    std::uint32_t gameSdkPackageVersion{};
    std::uint64_t hostApiHash{};
    std::uint64_t programContentHash{};

    [[nodiscard]] bool valid() const noexcept {
        return sdkCompatibilityVersion != 0 &&
               gameSdkPackageVersion != 0 &&
               hostApiHash != 0 && programContentHash != 0;
    }
};

struct ScriptBundle final {
    ScriptBundleIdentity identity;
    std::vector<ScriptModuleReference> modules;
};

class ScriptAssetCodec final {
public:
    static assets::CookedAsset moduleAsset(
        std::uint64_t assetId,
        std::vector<std::uint8_t> encodedModule);

    static bool decodeModule(
        const assets::CookedAsset& asset,
        std::vector<std::uint8_t>& encodedModule,
        ScriptDiagnostics* diagnostics = nullptr);

    static assets::CookedAsset bundleAsset(
        std::uint64_t assetId,
        ScriptBundle bundle);

    static bool decodeBundle(
        const assets::CookedAsset& asset,
        ScriptBundle& bundle,
        ScriptDiagnostics* diagnostics = nullptr);

    static bool canonicalize(
        ScriptBundle& bundle,
        ScriptDiagnostics* diagnostics = nullptr);

private:
    static std::vector<std::uint8_t> encodeBundlePayload(
        const ScriptBundle& bundle);
    static bool decodeBundlePayload(
        const std::vector<std::uint8_t>& payload,
        ScriptBundle& bundle,
        ScriptDiagnostics* diagnostics);
};

} // namespace rts::scripting
