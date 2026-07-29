#pragma once

#include <RTSEngine/RtsScripting/RtsScriptSession.h>

#include <realscript/runtime/Runtime.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rts::gameplay::scripting {

namespace detail {
struct RtsEntityBehaviorBindingState;
}

class RtsEntityBehaviorApi final {
public:
    explicit RtsEntityBehaviorApi(RtsScriptApi& baseApi);
    ~RtsEntityBehaviorApi();
    RtsEntityBehaviorApi(const RtsEntityBehaviorApi&) = delete;
    RtsEntityBehaviorApi& operator=(const RtsEntityBehaviorApi&) = delete;

    [[nodiscard]] const realscript::game::GameApi& gameApi() const noexcept {
        return baseApi_.gameApi();
    }

private:
    friend class RtsEntityBehaviorRuntime;
    RtsScriptApi& baseApi_;
    std::shared_ptr<detail::RtsEntityBehaviorBindingState> state_;
};

enum class RtsScriptTriggerKind : std::uint8_t {
    Tick,
    HealthAtMost,
    VisibleEnemy,
    EventType
};

struct RtsScriptTriggerDefinition final {
    std::uint64_t id{};
    RtsScriptTriggerKind kind{RtsScriptTriggerKind::Tick};
    std::uint64_t firstTick{};
    std::uint32_t intervalTicks{};
    std::int32_t threshold{};
    DomainEventType eventType{DomainEventType::CommandRejected};
    bool once{true};
    bool enabled{true};
};

struct RtsEntityBehaviorDefinition final {
    ecs::Entity entity{};
    std::string scriptType;
    std::uint32_t thinkIntervalTicks{};
    std::size_t maximumIntentsPerTick{64};
    rts::scripting::ScriptExecutionPolicy executionPolicy{};
};

struct RtsEntityBehaviorCommandOutcome final {
    std::uint64_t tick{};
    ecs::Entity entity{};
    std::uint32_t teamId{};
    std::uint32_t sequence{};
    std::uint32_t ordinal{};
    CommandType type{CommandType::Move};
    SessionCommandResult result{SessionCommandResult::Accepted};
};

struct RtsEntityBehaviorError final {
    std::uint64_t tick{};
    ecs::Entity entity{};
    std::string callback;
    realscript::runtime::RuntimeError error;
};

struct RtsEntityBehaviorReport final {
    std::uint64_t completedTick{};
    std::uint64_t targetTick{};
    std::uint32_t callbacks{};
    std::uint32_t triggersFired{};
    std::uint32_t intents{};
    std::uint32_t accepted{};
    std::uint32_t rejected{};
    std::uint32_t detached{};
};

enum class RtsEntityBehaviorTickResult : std::uint8_t {
    Processed,
    NoCallbacks,
    InvalidRuntime,
    TimelineMismatch
};

class RtsEntityBehaviorRuntime final {
public:
    static constexpr std::uint32_t kFirstBehaviorSequence = 0x80000000u;

    RtsEntityBehaviorRuntime(
        RtsGameSession& session,
        std::shared_ptr<rts::scripting::ScriptProgram> program,
        RtsEntityBehaviorApi& api);
    ~RtsEntityBehaviorRuntime();
    RtsEntityBehaviorRuntime(RtsEntityBehaviorRuntime&&) noexcept;
    RtsEntityBehaviorRuntime& operator=(RtsEntityBehaviorRuntime&&) noexcept;
    RtsEntityBehaviorRuntime(const RtsEntityBehaviorRuntime&) = delete;
    RtsEntityBehaviorRuntime& operator=(const RtsEntityBehaviorRuntime&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    bool attach(RtsEntityBehaviorDefinition definition);
    bool detach(ecs::Entity entity, bool invokeDestroy = true);
    bool setEnabled(ecs::Entity entity, bool enabled);
    bool addTrigger(ecs::Entity entity, RtsScriptTriggerDefinition trigger);
    bool removeTrigger(ecs::Entity entity, std::uint64_t triggerId);

    [[nodiscard]] RtsEntityBehaviorTickResult processCompletedTick(
        std::uint64_t completedTick);

    [[nodiscard]] const rts::scripting::ScriptProgramIdentity*
        programIdentity() const noexcept;
    [[nodiscard]] bool authoritativeHash(
        std::uint64_t& output,
        realscript::runtime::RuntimeError& error) const;
    [[nodiscard]] std::vector<std::uint8_t> encodeState(
        realscript::runtime::RuntimeError& error) const;
    [[nodiscard]] bool restoreEncodedState(
        const std::vector<std::uint8_t>& bytes,
        realscript::runtime::RuntimeError& error);

    [[nodiscard]] std::optional<realscript::runtime::Value> member(
        ecs::Entity entity,
        const std::string& name,
        realscript::runtime::RuntimeError& error) const;

    [[nodiscard]] const RtsEntityBehaviorReport& lastReport() const noexcept;
    [[nodiscard]] const std::vector<RtsEntityBehaviorCommandOutcome>& outcomes()
        const noexcept;
    [[nodiscard]] const std::vector<RtsEntityBehaviorError>& errors()
        const noexcept;
    void clearErrors();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rts::gameplay::scripting
