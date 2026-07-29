#pragma once

#include <RTSEngine/Assets/CookedAsset.h>
#include <RTSEngine/Scripting/ScriptBundle.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rts::scripting {

enum class ScriptModCapability : std::uint32_t {
    None = 0,
    GameplayRead = 1u << 0u,
    GameplayCommand = 1u << 1u,
    Presentation = 1u << 2u,
    FileRead = 1u << 3u,
    Network = 1u << 4u,
    WallClock = 1u << 5u,
    NativeExtension = 1u << 6u
};

constexpr ScriptModCapability operator|(
    ScriptModCapability left,
    ScriptModCapability right) noexcept {
    return static_cast<ScriptModCapability>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right));
}

constexpr ScriptModCapability operator&(
    ScriptModCapability left,
    ScriptModCapability right) noexcept {
    return static_cast<ScriptModCapability>(
        static_cast<std::uint32_t>(left) &
        static_cast<std::uint32_t>(right));
}

constexpr bool HasModCapabilities(
    ScriptModCapability value,
    ScriptModCapability required) noexcept {
    return (value & required) == required;
}

struct ScriptModDependency final {
    std::string modId;
    std::string version;
    bool optional{};
};

struct ScriptModManifest final {
    std::string modId;
    std::string version;
    std::string displayName;
    std::int32_t priority{};
    ScriptModCapability capabilities{ScriptModCapability::GameplayRead};
    bool authoritative{true};
    bool strictDeterminism{true};
    bool allowAot{};
    assets::AssetKey scriptBundle{};
    std::uint64_t bundlePayloadHash{};
    ScriptBundleIdentity scriptIdentity;
    std::uint64_t instructionBudget{250000};
    std::uint64_t maximumHeapBytes{64u * 1024u * 1024u};
    std::vector<ScriptModDependency> dependencies;
};

struct ScriptModPackage final {
    ScriptModManifest manifest;
    std::vector<assets::CookedAsset> assets;
    std::uint64_t packageHash{};
};

enum class ScriptModFailure : std::uint8_t {
    None,
    InvalidPackage,
    DuplicateMod,
    MissingDependency,
    DependencyVersionMismatch,
    DependencyCycle,
    CapabilityDenied,
    BudgetExceeded,
    AotDenied,
    LimitExceeded
};

struct ScriptModDiagnostic final {
    ScriptModFailure failure{ScriptModFailure::None};
    std::string modId;
    std::string message;
};

struct ScriptModPolicy final {
    std::size_t maximumMods{256};
    std::size_t maximumAssetsPerMod{4096};
    std::uint64_t maximumInstructionBudget{1000000};
    std::uint64_t maximumHeapBytes{256u * 1024u * 1024u};
    ScriptModCapability allowedAuthoritativeCapabilities{
        ScriptModCapability::GameplayRead |
        ScriptModCapability::GameplayCommand};
    ScriptModCapability allowedPresentationCapabilities{
        ScriptModCapability::GameplayRead |
        ScriptModCapability::Presentation |
        ScriptModCapability::FileRead};
    bool allowAot{true};
    bool requireStrictAuthoritativeDeterminism{true};
};

struct ScriptModResolveResult final {
    std::vector<std::size_t> loadOrder;
    std::vector<ScriptModDiagnostic> diagnostics;
    std::uint64_t modSetHash{};

    [[nodiscard]] bool succeeded() const noexcept {
        return diagnostics.empty() && modSetHash != 0;
    }
};

class ScriptModPackageCodec final {
public:
    static constexpr std::uint32_t kMagic = 0x354D5452u; // RTM5
    static constexpr std::uint16_t kVersion = 1u;
    static constexpr std::uint32_t kMaximumAssets = 4096u;
    static constexpr std::uint32_t kMaximumDependencies = 1024u;
    static constexpr std::uint32_t kMaximumStringBytes = 4096u;
    static constexpr std::uint32_t kMaximumAssetBytes =
        256u * 1024u * 1024u;

    [[nodiscard]] static bool canonicalize(ScriptModPackage& package);
    [[nodiscard]] static std::uint64_t canonicalHash(
        const ScriptModPackage& package) noexcept;
    [[nodiscard]] static std::vector<std::uint8_t> encode(
        ScriptModPackage package);
    [[nodiscard]] static bool decode(
        const std::vector<std::uint8_t>& bytes,
        ScriptModPackage& output);
};

class ScriptModResolver final {
public:
    [[nodiscard]] static ScriptModResolveResult resolve(
        const std::vector<ScriptModPackage>& packages,
        ScriptModPolicy policy = {});
};

[[nodiscard]] std::uint64_t CombineScriptModContentHash(
    std::uint64_t baseContentHash,
    std::uint64_t modSetHash) noexcept;

[[nodiscard]] const char* ScriptModFailureName(
    ScriptModFailure failure) noexcept;

} // namespace rts::scripting
