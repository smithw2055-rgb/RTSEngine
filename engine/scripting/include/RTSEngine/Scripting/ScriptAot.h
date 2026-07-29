#pragma once

#include <RTSEngine/Scripting/RealScriptHost.h>

#include <realscript/runtime/Runtime.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rts::scripting {

enum class ScriptBackendKind : std::uint8_t {
    Interpreter,
    Aot
};

enum class ScriptBackendPreference : std::uint8_t {
    InterpreterOnly,
    PreferAot,
    RequireAot
};

enum class ScriptAotCapability : std::uint32_t {
    None = 0,
    StatelessInvoke = 1u << 0u,
    ObjectLifecycle = 1u << 1u,
    PersistentObjectState = 1u << 2u
};

constexpr ScriptAotCapability operator|(
    ScriptAotCapability left,
    ScriptAotCapability right) noexcept {
    return static_cast<ScriptAotCapability>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right));
}

constexpr bool HasAotCapability(
    ScriptAotCapability value,
    ScriptAotCapability required) noexcept {
    return (static_cast<std::uint32_t>(value) &
            static_cast<std::uint32_t>(required)) ==
           static_cast<std::uint32_t>(required);
}

struct ScriptAotIdentity final {
    std::uint32_t sdkCompatibilityVersion{};
    std::uint32_t gameSdkPackageVersion{};
    std::uint64_t hostApiHash{};
    std::uint64_t programContentHash{};
    std::uint64_t nativeBuildHash{};
    std::string programName;

    [[nodiscard]] bool valid() const noexcept {
        return sdkCompatibilityVersion != 0 &&
               gameSdkPackageVersion != 0 && hostApiHash != 0 &&
               programContentHash != 0 && nativeBuildHash != 0 &&
               !programName.empty();
    }
};

[[nodiscard]] bool SameScriptProgram(
    const ScriptAotIdentity& aot,
    const ScriptProgramIdentity& program) noexcept;

class IScriptAotModule {
public:
    virtual ~IScriptAotModule() = default;
    [[nodiscard]] virtual const ScriptAotIdentity& identity() const noexcept = 0;
    [[nodiscard]] virtual ScriptAotCapability capabilities() const noexcept = 0;
    [[nodiscard]] virtual realscript::runtime::ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<realscript::runtime::Value>& arguments,
        ScriptExecutionPolicy policy) const = 0;
};

class ScriptAotRegistry final {
public:
    bool registerModule(std::shared_ptr<const IScriptAotModule> module);
    bool removeModule(const ScriptAotIdentity& identity);

    [[nodiscard]] std::shared_ptr<const IScriptAotModule> find(
        const ScriptProgramIdentity& program,
        ScriptAotCapability required =
            ScriptAotCapability::StatelessInvoke) const;
    [[nodiscard]] std::size_t size() const noexcept { return modules_.size(); }

private:
    std::vector<std::shared_ptr<const IScriptAotModule>> modules_;
};

struct ScriptExecutionSelection final {
    ScriptBackendKind backend{ScriptBackendKind::Interpreter};
    std::uint64_t nativeBuildHash{};
};

class ScriptExecutionFacade final {
public:
    ScriptExecutionFacade(
        const ScriptProgram& program,
        const ScriptAotRegistry* aotRegistry = nullptr,
        ScriptBackendPreference preference =
            ScriptBackendPreference::PreferAot)
        : program_(program),
          aotRegistry_(aotRegistry),
          preference_(preference) {}

    [[nodiscard]] realscript::runtime::ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<realscript::runtime::Value>& arguments = {},
        ScriptExecutionPolicy policy = {}) const;

    [[nodiscard]] ScriptExecutionSelection selection() const noexcept;

private:
    [[nodiscard]] std::shared_ptr<const IScriptAotModule> aot() const;

    const ScriptProgram& program_;
    const ScriptAotRegistry* aotRegistry_{};
    ScriptBackendPreference preference_{ScriptBackendPreference::PreferAot};
};

} // namespace rts::scripting
