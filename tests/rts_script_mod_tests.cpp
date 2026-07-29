#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Assets/CookedAsset.h>
#include <RTSEngine/Assets/Vfs.h>
#include <RTSEngine/RtsScripting/RtsScriptSession.h>
#include <RTSEngine/Scripting/RealScriptHost.h>
#include <RTSEngine/Scripting/ScriptAot.h>
#include <RTSEngine/Scripting/ScriptModPackage.h>

#include <realscript/bytecode/Bytecode.h>
#include <realscript/game/GameScripting.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace rts;
namespace rs = rts::scripting;
namespace rsg = rts::gameplay::scripting;

#define require(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "RS5 assertion failed at line " << __LINE__          \
                      << ": " #condition "\n";                               \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

const char* source = R"(
module Game.ModFixture;
import Engine.Rts;

int Value()
{
    return 5;
}
)";

void writeCooked(
    assets::MemoryVfs& vfs,
    const std::string& path,
    assets::CookedAsset asset) {
    const auto bytes = assets::EncodeCookedAsset(std::move(asset));
    require(!bytes.empty());
    require(vfs.write(path, bytes));
}

struct PackageFixture final {
    assets::MemoryVfs vfs;
    assets::AssetManager assets{vfs, 32u * 1024u * 1024u};
    rsg::RtsScriptApi api;
    rs::RealScriptHost host{assets, api.gameApi()};
    std::uint64_t baseId{};
    assets::AssetKey bundleKey{};
    rs::ScriptBundle bundle;
    std::vector<assets::CookedAsset> cooked;
    std::shared_ptr<rs::ScriptProgram> program;

    explicit PackageFixture(std::uint64_t value)
        : baseId(value),
          bundleKey{assets::AssetType::ScriptBundle, value} {
        realscript::game::GameScriptCompiler compiler(api.gameApi());
        const auto compiled = compiler.compile({{"mod_fixture.rs", source}});
        require(compiled.succeeded());
        require(!compiled.modules.empty());

        std::uint64_t moduleId = value + 1;
        std::vector<assets::CookedAsset> modules;
        for (const auto& module : compiled.modules) {
            modules.push_back(rs::ScriptAssetCodec::moduleAsset(
                moduleId++, realscript::bytecode::encodeModule(module)));
        }
        rs::ScriptDiagnostics diagnostics;
        const auto described = host.describeBundle(modules, diagnostics);
        require(described.has_value());
        require(!diagnostics.hasErrors());
        bundle = *described;
        cooked = modules;
        cooked.push_back(rs::ScriptAssetCodec::bundleAsset(value, bundle));

        for (const auto& asset : cooked) {
            const auto path = "mods/assets/" +
                std::to_string(static_cast<std::uint16_t>(asset.key.type)) +
                "-" + std::to_string(asset.key.id) + ".rta";
            writeCooked(vfs, path, asset);
            require(assets.registerAsset({
                asset.key, path, asset.schemaVersion}));
        }
        const auto request = assets.request(bundleKey);
        require(request.valid());
        require(assets.process() == 1);
        const auto loaded = host.load(bundleKey);
        require(loaded.succeeded());
        program = loaded.program;
        require(program && program->valid());
        require(assets.releaseRequest(request));
    }

    rs::ScriptModPackage package(
        std::string modId,
        std::string version,
        std::int32_t priority = 0) const {
        rs::ScriptModPackage result;
        result.manifest.modId = std::move(modId);
        result.manifest.version = std::move(version);
        result.manifest.displayName = result.manifest.modId;
        result.manifest.priority = priority;
        result.manifest.capabilities =
            rs::ScriptModCapability::GameplayRead |
            rs::ScriptModCapability::GameplayCommand;
        result.manifest.authoritative = true;
        result.manifest.strictDeterminism = true;
        result.manifest.allowAot = true;
        result.manifest.scriptBundle = bundleKey;
        result.manifest.bundlePayloadHash = cooked.back().payloadHash;
        result.manifest.scriptIdentity = bundle.identity;
        result.manifest.instructionBudget = 10000;
        result.manifest.maximumHeapBytes = 8u * 1024u * 1024u;
        result.assets = cooked;
        require(rs::ScriptModPackageCodec::canonicalize(result));
        return result;
    }
};

class FakeAotModule final : public rs::IScriptAotModule {
public:
    explicit FakeAotModule(
        const rs::ScriptProgramIdentity& program,
        std::uint64_t buildHash) {
        identity_.sdkCompatibilityVersion =
            program.script.sdkCompatibilityVersion;
        identity_.gameSdkPackageVersion =
            program.script.gameSdkPackageVersion;
        identity_.hostApiHash = program.script.hostApiHash;
        identity_.programContentHash = program.script.programContentHash;
        identity_.nativeBuildHash = buildHash;
        identity_.programName = "FakeAot";
    }

    const rs::ScriptAotIdentity& identity() const noexcept override {
        return identity_;
    }

    rs::ScriptAotCapability capabilities() const noexcept override {
        return rs::ScriptAotCapability::StatelessInvoke;
    }

    realscript::runtime::ExecutionResult invoke(
        const std::string&,
        const std::vector<realscript::runtime::Value>&,
        rs::ScriptExecutionPolicy) const override {
        realscript::runtime::ExecutionResult result;
        result.succeeded = true;
        result.value = std::int64_t{77};
        return result;
    }

private:
    rs::ScriptAotIdentity identity_;
};

void testAotRegistrySelectionAndFallback() {
    PackageFixture fixture(1000);
    rs::ScriptAotRegistry registry;
    auto older = std::make_shared<FakeAotModule>(
        fixture.program->identity(), 10);
    auto newer = std::make_shared<FakeAotModule>(
        fixture.program->identity(), 20);
    require(registry.registerModule(older));
    require(registry.registerModule(newer));
    require(registry.size() == 2);
    const auto selected = registry.find(fixture.program->identity());
    require(selected && selected->identity().nativeBuildHash == 20);

    rs::ScriptExecutionFacade aot(
        *fixture.program, &registry, rs::ScriptBackendPreference::PreferAot);
    require(aot.selection().backend == rs::ScriptBackendKind::Aot);
    require(aot.selection().nativeBuildHash == 20);
    const auto aotResult = aot.invoke("Game.ModFixture::Value");
    require(aotResult.succeeded);
    require(std::get<std::int64_t>(aotResult.value) == 77);

    rs::ScriptExecutionFacade interpreter(
        *fixture.program, nullptr,
        rs::ScriptBackendPreference::InterpreterOnly);
    require(interpreter.selection().backend ==
            rs::ScriptBackendKind::Interpreter);
    const auto interpreted = interpreter.invoke("Game.ModFixture::Value");
    require(interpreted.succeeded);
    require(std::get<std::int64_t>(interpreted.value) == 5);

    rs::ScriptAotRegistry empty;
    rs::ScriptExecutionFacade required(
        *fixture.program, &empty, rs::ScriptBackendPreference::RequireAot);
    const auto missing = required.invoke("Game.ModFixture::Value");
    require(!missing.succeeded);
    require(missing.error.code ==
            realscript::runtime::ErrorCode::InvalidProgram);
}

void testModPackageRoundTripAndResolution() {
    PackageFixture coreFixture(2000);
    PackageFixture addonFixture(3000);
    auto core = coreFixture.package("core.gameplay", "1.0.0", 10);
    auto addon = addonFixture.package("addon.units", "2.0.0", -20);
    addon.manifest.dependencies.push_back(
        {"core.gameplay", "1.0.0", false});
    require(rs::ScriptModPackageCodec::canonicalize(addon));

    const auto encoded = rs::ScriptModPackageCodec::encode(addon);
    require(!encoded.empty());
    rs::ScriptModPackage decoded;
    require(rs::ScriptModPackageCodec::decode(encoded, decoded));
    require(decoded.manifest.modId == addon.manifest.modId);
    require(decoded.packageHash == addon.packageHash);

    const std::vector<rs::ScriptModPackage> packages{addon, core};
    const auto resolved = rs::ScriptModResolver::resolve(packages);
    require(resolved.succeeded());
    require(resolved.loadOrder.size() == 2);
    require(packages[resolved.loadOrder[0]].manifest.modId ==
            "core.gameplay");
    require(packages[resolved.loadOrder[1]].manifest.modId ==
            "addon.units");
    require(resolved.modSetHash != 0);
    require(rs::CombineScriptModContentHash(55, resolved.modSetHash) != 55);
}

void testModSandboxAndDependencyFailures() {
    PackageFixture firstFixture(4000);
    PackageFixture secondFixture(5000);
    auto first = firstFixture.package("first", "1", 0);
    auto second = secondFixture.package("second", "1", 0);

    first.manifest.capabilities =
        first.manifest.capabilities | rs::ScriptModCapability::Network;
    require(rs::ScriptModPackageCodec::canonicalize(first));
    auto denied = rs::ScriptModResolver::resolve({first});
    require(!denied.succeeded());
    require(denied.diagnostics.front().failure ==
            rs::ScriptModFailure::CapabilityDenied);

    first = firstFixture.package("first", "1", 0);
    first.manifest.dependencies.push_back({"second", "1", false});
    second.manifest.dependencies.push_back({"first", "1", false});
    require(rs::ScriptModPackageCodec::canonicalize(first));
    require(rs::ScriptModPackageCodec::canonicalize(second));
    const auto cycle = rs::ScriptModResolver::resolve({first, second});
    require(!cycle.succeeded());
    require(cycle.diagnostics.front().failure ==
            rs::ScriptModFailure::DependencyCycle);

    auto missing = secondFixture.package("missing-user", "1", 0);
    missing.manifest.dependencies.push_back({"not-installed", "1", false});
    require(rs::ScriptModPackageCodec::canonicalize(missing));
    const auto unresolved = rs::ScriptModResolver::resolve({missing});
    require(!unresolved.succeeded());
    require(unresolved.diagnostics.front().failure ==
            rs::ScriptModFailure::MissingDependency);
}

} // namespace

int main() {
    testAotRegistrySelectionAndFallback();
    testModPackageRoundTripAndResolution();
    testModSandboxAndDependencyFailures();
    std::cout << "RTSEngine RealScript Stage RS5 tests passed\n";
    return 0;
}
