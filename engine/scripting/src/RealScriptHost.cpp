#include <RTSEngine/Scripting/RealScriptHost.h>

#include <realscript/Version.h>
#include <realscript/bytecode/Bytecode.h>

#include <algorithm>
#include <utility>

namespace rts::scripting {
namespace {

realscript::runtime::ExecutionOptions makeExecutionOptions(
    ScriptExecutionPolicy policy) {
    realscript::runtime::ExecutionOptions options;
    options.limits.instructionBudget =
        std::max<std::uint64_t>(1u, policy.instructionBudget);
    options.limits.recursionLimit =
        std::max<std::size_t>(1u, policy.recursionLimit);
    options.limits.gcWorkBudget = policy.gcWorkBudget;
    options.determinism.mode = policy.strictDeterminism
        ? realscript::runtime::DeterminismMode::Strict
        : realscript::runtime::DeterminismMode::Off;
    return options;
}

realscript::runtime::ExecutionResult invalidProgramResult() {
    realscript::runtime::ExecutionResult result;
    result.error.code = realscript::runtime::ErrorCode::InvalidProgram;
    result.error.message = "script program is not loaded";
    return result;
}

void appendLoadDiagnostics(
    ScriptDiagnostics& destination,
    const realscript::diagnostics::DiagnosticBag& source) {
    destination.append(source);
}

} // namespace

realscript::runtime::ExecutionResult ScriptProgram::invoke(
    const std::string& qualifiedName,
    const std::vector<realscript::runtime::Value>& arguments,
    ScriptExecutionPolicy policy) const {
    if (!valid()) return invalidProgramResult();
    return runtime_->invoke(
        qualifiedName,
        arguments,
        makeExecutionOptions(policy));
}

RealScriptHost::RealScriptHost(
    assets::AssetManager& assets,
    realscript::game::GameApi api)
    : assets_(assets), api_(std::move(api)) {}

std::optional<ScriptBundle> RealScriptHost::describeBundle(
    const std::vector<assets::CookedAsset>& modules,
    ScriptDiagnostics& diagnostics) const {
    if (!api_.valid()) {
        for (const auto& message : api_.errors()) {
            diagnostics.add("RTSRS2001", message);
        }
        return std::nullopt;
    }
    if (modules.empty() || modules.size() > kMaximumScriptModules) {
        diagnostics.add(
            "RTSRS2002",
            "script bundle must contain at least one module within the supported limit");
        return std::nullopt;
    }

    std::vector<const assets::CookedAsset*> ordered;
    ordered.reserve(modules.size());
    for (const auto& module : modules) ordered.push_back(&module);
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const assets::CookedAsset* first,
           const assets::CookedAsset* second) {
            return first->key.id < second->key.id;
        });

    std::vector<std::vector<std::uint8_t>> encodedModules;
    std::vector<ScriptModuleReference> references;
    encodedModules.reserve(ordered.size());
    references.reserve(ordered.size());
    std::uint64_t previousId = 0;
    for (const auto* module : ordered) {
        if (!module || module->key.id == previousId) {
            diagnostics.add(
                "RTSRS2003",
                "script bundle contains duplicate module asset identities");
            return std::nullopt;
        }
        std::vector<std::uint8_t> encoded;
        if (!ScriptAssetCodec::decodeModule(*module, encoded, &diagnostics)) {
            return std::nullopt;
        }
        previousId = module->key.id;
        references.push_back({module->key.id, module->payloadHash});
        encodedModules.push_back(std::move(encoded));
    }

    realscript::game::GameProgramLoader loader(api_);
    auto loaded = loader.loadBytecodeModules(encodedModules);
    appendLoadDiagnostics(diagnostics, loaded.diagnostics);
    if (!loaded.succeeded()) return std::nullopt;

    ScriptBundle bundle;
    bundle.identity.sdkCompatibilityVersion =
        realscript::kSdkCompatibilityVersion;
    bundle.identity.gameSdkPackageVersion =
        realscript::kGameSdkPackageVersion;
    bundle.identity.hostApiHash = loaded.package.hostApiHash;
    bundle.identity.programContentHash = loaded.package.programContentHash;
    bundle.modules = std::move(references);
    if (!ScriptAssetCodec::canonicalize(bundle, &diagnostics)) {
        return std::nullopt;
    }
    return bundle;
}

ScriptLoadResult RealScriptHost::load(assets::AssetKey bundleKey) {
    ScriptLoadResult result;
    if (bundleKey.type != assets::AssetType::ScriptBundle ||
        !bundleKey.valid()) {
        result.failure = ScriptLoadFailure::InvalidBundleKey;
        result.diagnostics.add(
            "RTSRS2101",
            "script load requires a valid ScriptBundle asset key");
        return result;
    }

    const auto* loadedBundle = assets_.loaded(bundleKey);
    if (!loadedBundle) {
        result.failure = ScriptLoadFailure::BundleNotLoaded;
        result.diagnostics.add(
            "RTSRS2102",
            "script bundle is not loaded by the asset manager");
        return result;
    }

    ScriptBundle bundle;
    if (!ScriptAssetCodec::decodeBundle(
            loadedBundle->cooked,
            bundle,
            &result.diagnostics)) {
        result.failure = ScriptLoadFailure::InvalidBundle;
        return result;
    }
    if (bundle.identity.sdkCompatibilityVersion !=
            realscript::kSdkCompatibilityVersion ||
        bundle.identity.gameSdkPackageVersion !=
            realscript::kGameSdkPackageVersion) {
        result.failure = ScriptLoadFailure::SdkVersionMismatch;
        result.diagnostics.add(
            "RTSRS2103",
            "script bundle was produced by an incompatible RealScript SDK");
        return result;
    }

    const auto currentHostApiHash =
        realscript::game::stableGameApiHash(api_);
    if (bundle.identity.hostApiHash != currentHostApiHash) {
        result.failure = ScriptLoadFailure::HostApiMismatch;
        result.diagnostics.add(
            "RTSRS2104",
            "script bundle host API identity does not match the current engine API");
        return result;
    }

    std::vector<std::vector<std::uint8_t>> encodedModules;
    encodedModules.reserve(bundle.modules.size());
    for (const auto& module : bundle.modules) {
        const assets::AssetKey moduleKey{
            assets::AssetType::ScriptModule,
            module.assetId};
        const auto* loadedModule = assets_.loaded(moduleKey);
        if (!loadedModule) {
            result.failure = ScriptLoadFailure::MissingModule;
            result.diagnostics.add(
                "RTSRS2105",
                "a script module required by the bundle is not loaded");
            return result;
        }
        if (loadedModule->cooked.payloadHash != module.payloadHash) {
            result.failure = ScriptLoadFailure::ModuleHashMismatch;
            result.diagnostics.add(
                "RTSRS2106",
                "a loaded script module does not match the bundle identity");
            return result;
        }
        std::vector<std::uint8_t> encoded;
        if (!ScriptAssetCodec::decodeModule(
                loadedModule->cooked,
                encoded,
                &result.diagnostics)) {
            result.failure = ScriptLoadFailure::ProgramDecodeFailed;
            return result;
        }
        encodedModules.push_back(std::move(encoded));
    }

    realscript::game::GameProgramLoader loader(api_);
    auto loadedProgram = loader.loadBytecodeModules(encodedModules);
    appendLoadDiagnostics(result.diagnostics, loadedProgram.diagnostics);
    if (!loadedProgram.succeeded()) {
        result.failure = ScriptLoadFailure::ProgramDecodeFailed;
        return result;
    }
    if (loadedProgram.package.hostApiHash != bundle.identity.hostApiHash ||
        loadedProgram.package.programContentHash !=
            bundle.identity.programContentHash) {
        result.failure = ScriptLoadFailure::ProgramContentMismatch;
        result.diagnostics.add(
            "RTSRS2107",
            "decoded script program identity does not match its bundle");
        return result;
    }

    auto runtime = std::make_shared<realscript::runtime::EngineRuntime>(
        loadedProgram.package.program);
    runtime->setBindings(loadedProgram.package.bindings);
    runtime->setHeap(loadedProgram.package.heap);
    runtime->setNativeHandles(loadedProgram.package.nativeHandles);

    ScriptProgramIdentity identity;
    identity.bundle = bundleKey;
    identity.bundlePayloadHash = loadedBundle->cooked.payloadHash;
    identity.script = bundle.identity;
    result.program = std::shared_ptr<ScriptProgram>(new ScriptProgram(
        identity,
        std::move(loadedProgram.package),
        std::move(runtime)));
    return result;
}

realscript::runtime::ExecutionOptions RealScriptHost::executionOptions(
    ScriptExecutionPolicy policy) {
    return makeExecutionOptions(policy);
}

const char* scriptLoadFailureName(ScriptLoadFailure failure) noexcept {
    switch (failure) {
    case ScriptLoadFailure::None: return "None";
    case ScriptLoadFailure::InvalidBundleKey: return "InvalidBundleKey";
    case ScriptLoadFailure::BundleNotLoaded: return "BundleNotLoaded";
    case ScriptLoadFailure::InvalidBundle: return "InvalidBundle";
    case ScriptLoadFailure::SdkVersionMismatch: return "SdkVersionMismatch";
    case ScriptLoadFailure::HostApiMismatch: return "HostApiMismatch";
    case ScriptLoadFailure::MissingModule: return "MissingModule";
    case ScriptLoadFailure::ModuleHashMismatch: return "ModuleHashMismatch";
    case ScriptLoadFailure::ProgramDecodeFailed: return "ProgramDecodeFailed";
    case ScriptLoadFailure::ProgramContentMismatch:
        return "ProgramContentMismatch";
    }
    return "Unknown";
}

} // namespace rts::scripting
