#pragma once

#include <RTSEngine/Rts/RtsGameSession.h>
#include <RTSEngine/Scripting/RealScriptHost.h>

#include <realscript/game/GameScripting.h>
#include <realscript/runtime/Runtime.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rts::gameplay::scripting {

using ScriptEntityId = std::int64_t;

struct RtsScriptEntityView final {
    ScriptEntityId id{};
    std::uint32_t teamId{};
    SnapshotKind kind{SnapshotKind::Unit};
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t health{};
    std::uint32_t queuedOrders{};
    std::uint32_t productionQueueSize{};
    bool moving{};
    bool visible{};
    bool hostile{};

    [[nodiscard]] bool idle() const noexcept {
        return !moving && queuedOrders == 0;
    }
};

class RtsScriptReadView final {
public:
    RtsScriptReadView() = default;

    [[nodiscard]] static RtsScriptReadView capture(
        const RtsGameSession& session,
        std::uint32_t teamId);

    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }
    [[nodiscard]] std::uint32_t teamId() const noexcept { return teamId_; }
    [[nodiscard]] std::uint32_t usedSupply() const noexcept {
        return usedSupply_;
    }
    [[nodiscard]] std::uint32_t supplyCapacity() const noexcept {
        return supplyCapacity_;
    }
    [[nodiscard]] ResourceAmount resourceAvailable(
        ResourceTypeId resourceType) const noexcept;
    [[nodiscard]] const RtsScriptEntityView* entity(
        ScriptEntityId id) const noexcept;
    [[nodiscard]] bool containsCell(std::int32_t x, std::int32_t y) const noexcept;
    [[nodiscard]] ScriptEntityId findIdleUnit() const noexcept;
    [[nodiscard]] ScriptEntityId findIdleProducer() const noexcept;
    [[nodiscard]] ScriptEntityId findNearestVisibleEnemy(
        ScriptEntityId from) const noexcept;
    [[nodiscard]] const std::vector<RtsScriptEntityView>& entities()
        const noexcept {
        return entities_;
    }

private:
    std::uint64_t tick_{};
    std::uint32_t teamId_{};
    std::uint32_t usedSupply_{};
    std::uint32_t supplyCapacity_{};
    std::int32_t width_{};
    std::int32_t height_{};
    std::vector<TeamResourceAccount> resources_;
    std::vector<RtsScriptEntityView> entities_;
};

namespace detail {
struct RtsScriptBindingState;
}

class RtsScriptApi final {
public:
    RtsScriptApi();
    ~RtsScriptApi();
    RtsScriptApi(const RtsScriptApi&) = delete;
    RtsScriptApi& operator=(const RtsScriptApi&) = delete;

    [[nodiscard]] const realscript::game::GameApi& gameApi() const noexcept {
        return api_;
    }

    // Extension modules must be registered before compiling or loading a
    // ScriptBundle so that the Host API hash covers the complete contract.
    [[nodiscard]] realscript::game::GameApi& mutableGameApi() noexcept {
        return api_;
    }

private:
    friend class RtsScriptSession;
    std::shared_ptr<detail::RtsScriptBindingState> state_;
    realscript::game::GameApi api_;
};

struct RtsTeamScriptDefinition final {
    std::uint32_t teamId{};
    std::string scriptType;
    std::uint32_t thinkIntervalTicks{8};
    std::size_t maximumIntentsPerTick{256};
    rts::scripting::ScriptExecutionPolicy executionPolicy{};
};

struct RtsScriptCommandOutcome final {
    std::uint64_t tick{};
    std::uint32_t teamId{};
    std::uint32_t sequence{};
    std::uint32_t ordinal{};
    CommandType type{CommandType::Move};
    SessionCommandResult result{SessionCommandResult::Accepted};
};

struct RtsScriptError final {
    std::uint64_t tick{};
    std::uint32_t teamId{};
    std::string callback;
    realscript::runtime::RuntimeError error;
};

struct RtsScriptTickReport final {
    std::uint64_t completedTick{};
    std::uint64_t targetTick{};
    std::uint32_t callbacks{};
    std::uint32_t intents{};
    std::uint32_t accepted{};
    std::uint32_t rejected{};
};

enum class RtsScriptTickResult : std::uint8_t {
    Processed,
    NoCallbacks,
    InvalidRuntime,
    TimelineMismatch
};

class RtsScriptSession final {
public:
    RtsScriptSession(
        RtsGameSession& session,
        std::shared_ptr<rts::scripting::ScriptProgram> program,
        RtsScriptApi& api);
    ~RtsScriptSession();
    RtsScriptSession(RtsScriptSession&&) noexcept;
    RtsScriptSession& operator=(RtsScriptSession&&) noexcept;
    RtsScriptSession(const RtsScriptSession&) = delete;
    RtsScriptSession& operator=(const RtsScriptSession&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    bool registerTeam(RtsTeamScriptDefinition definition);
    bool setTeamEnabled(std::uint32_t teamId, bool enabled);
    [[nodiscard]] RtsScriptTickResult processCompletedTick(
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

    [[nodiscard]] const RtsScriptTickReport& lastReport() const noexcept;
    [[nodiscard]] const std::vector<RtsScriptCommandOutcome>& outcomes()
        const noexcept;
    [[nodiscard]] const std::vector<RtsScriptError>& errors() const noexcept;
    void clearErrors();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] ScriptEntityId packScriptEntity(ecs::Entity entity) noexcept;
[[nodiscard]] ecs::Entity unpackScriptEntity(ScriptEntityId value) noexcept;

} // namespace rts::gameplay::scripting
