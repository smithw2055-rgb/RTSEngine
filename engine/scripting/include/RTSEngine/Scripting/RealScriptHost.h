#pragma once

#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Scripting/ScriptBundle.h>

#include <realscript/game/GameProductization.h>
#include <realscript/runtime/Runtime.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rts::scripting {

struct ScriptExecutionPolicy final {
    std::uint64_t instructionBudget{250000};
    std::size_t recursionLimit{128};
    std::size_t gcWorkBudget{8};
    bool strictDeterminism{true};
};

struct ScriptProgramIdentity final {
    assets::AssetKey bundle{};
    std::uint64_t bundlePayloadHash{};
    ScriptBundleIdentity script;
};

class ScriptProgram final {
public:
    ScriptProgram() = default;

    [[nodiscard]] bool valid() const noexcept {
        return runtime_ != nullptr && identity_.bundle.valid() &&
               identity_.script.valid();
    }

    [[nodiscard]] const ScriptProgramIdentity& identity() const noexcept {
        return identity_;
    }

    [[nodiscard]] realscript::runtime::ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<realscript::runtime::Value>& arguments = {},
        ScriptExecutionPolicy policy = {}) const;

    [[nodiscard]] realscript::game::ScriptRuntime createObjectRuntime() const {
        return package_.createRuntime();
    }

    [[nodiscard]] std::shared_ptr<realscript::runtime::ManagedHeap> heap()
        const noexcept {
        return package_.heap;
    }

    [[nodiscard]] std::shared_ptr<realscript::runtime::NativeHandleRegistry>
    nativeHandles() const noexcept {
        return package_.nativeHandles;
    }

private:
    friend class RealScriptHost;

    ScriptProgram(
        ScriptProgramIdentity identity,
        realscript::game::GameProgramPackage package,
        std::shared_ptr<realscript::runtime::EngineRuntime> runtime)
        : identity_(std::move(identity)),
          package_(std::move(package)),
          runtime_(std::move(runtime)) {}

    ScriptProgramIdentity identity_;
    realscript::game::GameProgramPackage package_;
    std::shared_ptr<realscript::runtime::EngineRuntime> runtime_;
};

enum class ScriptLoadFailure : std::uint8_t {
    None,
    InvalidBundleKey,
    BundleNotLoaded,
    InvalidBundle,
    SdkVersionMismatch,
    HostApiMismatch,
    MissingModule,
    ModuleHashMismatch,
    ProgramDecodeFailed,
    ProgramContentMismatch
};

struct ScriptLoadResult final {
    std::shared_ptr<ScriptProgram> program;
    ScriptLoadFailure failure{ScriptLoadFailure::None};
    ScriptDiagnostics diagnostics;

    [[nodiscard]] bool succeeded() const noexcept {
        return program != nullptr && program->valid() &&
               failure == ScriptLoadFailure::None &&
               !diagnostics.hasErrors();
    }
};

class RealScriptHost final {
public:
    RealScriptHost(
        assets::AssetManager& assets,
        realscript::game::GameApi api);

    [[nodiscard]] const realscript::game::GameApi& api() const noexcept {
        return api_;
    }

    [[nodiscard]] std::optional<ScriptBundle> describeBundle(
        const std::vector<assets::CookedAsset>& modules,
        ScriptDiagnostics& diagnostics) const;

    [[nodiscard]] ScriptLoadResult load(assets::AssetKey bundleKey);

private:
    static realscript::runtime::ExecutionOptions executionOptions(
        ScriptExecutionPolicy policy);

    assets::AssetManager& assets_;
    realscript::game::GameApi api_;
};

[[nodiscard]] const char* scriptLoadFailureName(
    ScriptLoadFailure failure) noexcept;

} // namespace rts::scripting
