#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Assets/CookedAsset.h>
#include <RTSEngine/Assets/Vfs.h>
#include <RTSEngine/Scripting/RealScriptHost.h>
#include <RTSEngine/Scripting/ScriptBundle.h>

#include <realscript/bytecode/Bytecode.h>
#include <realscript/game/GameScripting.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace rts;

void check(bool condition) {
    assert(condition);
    if (!condition) std::abort();
}

void writeCooked(
    assets::MemoryVfs& vfs,
    const std::string& path,
    assets::CookedAsset asset) {
    const auto bytes = assets::EncodeCookedAsset(std::move(asset));
    check(!bytes.empty());
    check(vfs.write(path, bytes));
}

realscript::game::GameApi makeApi() {
    realscript::game::GameApi api;
    check(api.function(
        "Engine.Test",
        "Add",
        [](int first, int second) { return first + second; }));
    check(api.function(
        "Engine.Test",
        "ReadClock",
        [] { return 7; },
        realscript::runtime::BindingDeterminism::NonDeterministic));
    check(api.valid());
    return api;
}

const char* source = R"(
module Game.Main;
import Engine.Test;

int main()
{
    return Add(20, 22);
}

int readClock()
{
    return ReadClock();
}
)";

struct Fixture final {
    assets::MemoryVfs vfs;
    assets::AssetManager assets{vfs, 32u * 1024u * 1024u};
    realscript::game::GameApi api{makeApi()};
    scripting::RealScriptHost host{assets, api};
    scripting::ScriptBundle bundle;
    std::vector<assets::CookedAsset> modules;
    assets::AssetKey bundleKey{assets::AssetType::ScriptBundle, 900};

    Fixture() {
        realscript::game::GameScriptCompiler compiler(api);
        const auto compiled = compiler.compile({{"game/main.rs", source}});
        check(compiled.succeeded());
        check(!compiled.modules.empty());

        std::uint64_t nextModuleId = 100;
        for (const auto& module : compiled.modules) {
            auto asset = scripting::ScriptAssetCodec::moduleAsset(
                nextModuleId++,
                realscript::bytecode::encodeModule(module));
            check(asset.key.valid());
            modules.push_back(std::move(asset));
        }

        scripting::ScriptDiagnostics diagnostics;
        const auto described = host.describeBundle(modules, diagnostics);
        check(described.has_value());
        check(!diagnostics.hasErrors());
        bundle = *described;

        for (const auto& module : modules) {
            const auto path =
                "scripts/modules/" + std::to_string(module.key.id) + ".rta";
            writeCooked(vfs, path, module);
            check(assets.registerAsset(
                {module.key, path, scripting::kScriptModuleSchemaVersion}));
        }
        auto bundleAsset = scripting::ScriptAssetCodec::bundleAsset(
            bundleKey.id,
            bundle);
        check(bundleAsset.key == bundleKey);
        writeCooked(vfs, "scripts/game.rta", bundleAsset);
        check(assets.registerAsset(
            {bundleKey,
             "scripts/game.rta",
             scripting::kScriptBundleSchemaVersion}));

        const auto request = assets.request(bundleKey);
        check(request.valid());
        check(assets.process() == 1);
        assets::AssetRequestStatus status;
        check(assets.requestStatus(request, status));
        check(status.completed && status.state == assets::AssetState::Ready);
        check(assets.releaseRequest(request));
    }
};

void testBundleAssetRoundTripAndExecution() {
    Fixture fixture;
    const auto loaded = fixture.host.load(fixture.bundleKey);
    check(loaded.succeeded());
    check(loaded.program->identity().script.hostApiHash ==
          fixture.bundle.identity.hostApiHash);
    check(loaded.program->identity().script.programContentHash ==
          fixture.bundle.identity.programContentHash);

    scripting::ScriptExecutionPolicy policy;
    policy.instructionBudget = 1000;
    const auto execution = loaded.program->invoke(
        "Game.Main::main",
        {},
        policy);
    check(execution.succeeded);
    check(std::get<std::int64_t>(execution.value) == 42);

    scripting::ScriptExecutionPolicy exhausted;
    exhausted.instructionBudget = 1;
    const auto limited = loaded.program->invoke(
        "Game.Main::main",
        {},
        exhausted);
    check(!limited.succeeded);
    check(limited.error.code ==
          realscript::runtime::ErrorCode::InstructionBudgetExceeded);
}

void testStrictDeterminismRejectsNonDeterministicBindings() {
    Fixture fixture;
    const auto loaded = fixture.host.load(fixture.bundleKey);
    check(loaded.succeeded());

    const auto strict = loaded.program->invoke("Game.Main::readClock");
    check(!strict.succeeded);
    check(strict.error.code ==
          realscript::runtime::ErrorCode::DeterminismViolation);

    scripting::ScriptExecutionPolicy relaxed;
    relaxed.strictDeterminism = false;
    const auto allowed = loaded.program->invoke(
        "Game.Main::readClock",
        {},
        relaxed);
    check(allowed.succeeded);
    check(std::get<std::int64_t>(allowed.value) == 7);
}

void testHostApiMismatchIsRejectedBeforeExecution() {
    Fixture fixture;
    auto incompatible = fixture.bundle;
    ++incompatible.identity.hostApiHash;
    const assets::AssetKey incompatibleKey{
        assets::AssetType::ScriptBundle,
        901};
    auto bundleAsset = scripting::ScriptAssetCodec::bundleAsset(
        incompatibleKey.id,
        incompatible);
    check(bundleAsset.key == incompatibleKey);
    writeCooked(fixture.vfs, "scripts/incompatible.rta", bundleAsset);
    check(fixture.assets.registerAsset(
        {incompatibleKey,
         "scripts/incompatible.rta",
         scripting::kScriptBundleSchemaVersion}));
    const auto request = fixture.assets.request(incompatibleKey);
    check(request.valid());
    check(fixture.assets.process() == 1);

    const auto loaded = fixture.host.load(incompatibleKey);
    check(!loaded.succeeded());
    check(loaded.failure == scripting::ScriptLoadFailure::HostApiMismatch);
    check(fixture.assets.releaseRequest(request));
}

void testCookedAssetCodecAcceptsScriptAssetTypes() {
    realscript::game::GameApi api = makeApi();
    realscript::game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({{"game/main.rs", source}});
    check(compiled.succeeded() && !compiled.modules.empty());

    const auto original = scripting::ScriptAssetCodec::moduleAsset(
        777,
        realscript::bytecode::encodeModule(compiled.modules.front()));
    const auto encoded = assets::EncodeCookedAsset(original);
    check(!encoded.empty());
    assets::CookedAsset decoded;
    check(assets::DecodeCookedAsset(encoded, decoded));
    check(decoded.key.type == assets::AssetType::ScriptModule);
    check(decoded.key.id == 777);
    check(decoded.payloadHash == original.payloadHash);
}

} // namespace

int main() {
    testBundleAssetRoundTripAndExecution();
    testStrictDeterminismRejectsNonDeterministicBindings();
    testHostApiMismatchIsRejectedBeforeExecution();
    testCookedAssetCodecAcceptsScriptAssetTypes();
    std::cout << "RTSEngine RealScript Stage RS1 tests passed\n";
    return 0;
}
