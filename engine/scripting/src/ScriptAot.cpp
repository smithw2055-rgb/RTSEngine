#include <RTSEngine/Scripting/ScriptAot.h>

#include <algorithm>
#include <tuple>

namespace rts::scripting {
namespace {

using IdentityKey = std::tuple<
    std::uint64_t,
    std::uint64_t,
    std::uint64_t,
    std::string>;

IdentityKey key(const ScriptAotIdentity& value) {
    return {
        value.programContentHash,
        value.hostApiHash,
        value.nativeBuildHash,
        value.programName};
}

realscript::runtime::ExecutionResult missingAotResult() {
    realscript::runtime::ExecutionResult result;
    result.error.code = realscript::runtime::ErrorCode::InvalidProgram;
    result.error.message =
        "a compatible RealScript AOT module is required but was not registered";
    return result;
}

} // namespace

bool SameScriptProgram(
    const ScriptAotIdentity& aot,
    const ScriptProgramIdentity& program) noexcept {
    return aot.sdkCompatibilityVersion ==
               program.script.sdkCompatibilityVersion &&
           aot.gameSdkPackageVersion ==
               program.script.gameSdkPackageVersion &&
           aot.hostApiHash == program.script.hostApiHash &&
           aot.programContentHash == program.script.programContentHash;
}

bool ScriptAotRegistry::registerModule(
    std::shared_ptr<const IScriptAotModule> module) {
    if (!module || !module->identity().valid() ||
        !HasAotCapability(
            module->capabilities(), ScriptAotCapability::StatelessInvoke)) {
        return false;
    }
    const auto position = std::lower_bound(
        modules_.begin(), modules_.end(), key(module->identity()),
        [](const std::shared_ptr<const IScriptAotModule>& value,
           const IdentityKey& candidate) {
            return key(value->identity()) < candidate;
        });
    if (position != modules_.end() &&
        key((*position)->identity()) == key(module->identity())) {
        return false;
    }
    modules_.insert(position, std::move(module));
    return true;
}

bool ScriptAotRegistry::removeModule(const ScriptAotIdentity& identity) {
    const auto position = std::lower_bound(
        modules_.begin(), modules_.end(), key(identity),
        [](const std::shared_ptr<const IScriptAotModule>& value,
           const IdentityKey& candidate) {
            return key(value->identity()) < candidate;
        });
    if (position == modules_.end() ||
        key((*position)->identity()) != key(identity)) {
        return false;
    }
    modules_.erase(position);
    return true;
}

std::shared_ptr<const IScriptAotModule> ScriptAotRegistry::find(
    const ScriptProgramIdentity& program,
    ScriptAotCapability required) const {
    std::shared_ptr<const IScriptAotModule> selected;
    for (const auto& module : modules_) {
        if (!SameScriptProgram(module->identity(), program) ||
            !HasAotCapability(module->capabilities(), required)) {
            continue;
        }
        if (!selected || module->identity().nativeBuildHash >
                             selected->identity().nativeBuildHash) {
            selected = module;
        }
    }
    return selected;
}

std::shared_ptr<const IScriptAotModule> ScriptExecutionFacade::aot() const {
    return preference_ == ScriptBackendPreference::InterpreterOnly ||
                   !aotRegistry_ || !program_.valid()
        ? nullptr
        : aotRegistry_->find(program_.identity());
}

realscript::runtime::ExecutionResult ScriptExecutionFacade::invoke(
    const std::string& qualifiedName,
    const std::vector<realscript::runtime::Value>& arguments,
    ScriptExecutionPolicy policy) const {
    const auto module = aot();
    if (module) {
        return module->invoke(qualifiedName, arguments, policy);
    }
    if (preference_ == ScriptBackendPreference::RequireAot) {
        return missingAotResult();
    }
    return program_.invoke(qualifiedName, arguments, policy);
}

ScriptExecutionSelection ScriptExecutionFacade::selection() const noexcept {
    const auto module = aot();
    return module
        ? ScriptExecutionSelection{
              ScriptBackendKind::Aot,
              module->identity().nativeBuildHash}
        : ScriptExecutionSelection{};
}

} // namespace rts::scripting
