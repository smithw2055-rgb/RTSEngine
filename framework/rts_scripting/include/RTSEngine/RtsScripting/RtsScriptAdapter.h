#pragma once

#include <RTSEngine/Rts/RtsGameSession.h>
#include <RTSEngine/Scripting/RealScriptHost.h>

#include <realscript/game/GameScripting.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rts::gameplay::scripting {

enum class RtsScriptIntentType : std::uint8_t {
    Attack,
    AttackMove
};

struct RtsScriptIntent final {
    RtsScriptIntentType type{RtsScriptIntentType::AttackMove};
    ecs::Entity subject{};
    ecs::Entity target{};
    std::int32_t x{};
    std::int32_t y{};
};

struct RtsScriptContext final {
    RtsGameSession* session{};
    std::uint32_t activeTeam{};
    std::uint64_t nextTick{};
    std::vector<RtsScriptIntent> intents;

    void begin(RtsGameSession& value, std::uint32_t teamId,
               std::uint64_t targetTick);
    void clear() noexcept;
};

[[nodiscard]] realscript::game::GameApi CreateRtsScriptApi(
    const std::shared_ptr<RtsScriptContext>& context);

struct RtsTeamScriptConfig final {
    std::uint32_t teamId{};
    std::uint32_t thinkIntervalTicks{8};
    std::uint32_t nextSequence{1};
    std::string entryPoint{"Game.AI::OnThink"};
};

struct RtsTeamScriptError final {
    std::uint64_t tick{};
    std::uint32_t teamId{};
    realscript::runtime::RuntimeError error;
};

class RtsTeamScriptDriver final {
public:
    RtsTeamScriptDriver(
        RtsGameSession& session,
        std::shared_ptr<::rts::scripting::ScriptProgram> program,
        std::shared_ptr<RtsScriptContext> context);

    bool registerTeam(RtsTeamScriptConfig config);
    bool removeTeam(std::uint32_t teamId);

    // Call after Tick N has completed. Script intents become normal commands for
    // Tick N+1 and still pass through RtsGameSession validation.
    std::size_t afterStep(std::uint64_t completedTick);

    [[nodiscard]] const std::vector<RtsTeamScriptConfig>& teams() const noexcept {
        return teams_;
    }
    [[nodiscard]] const std::vector<RtsTeamScriptError>& errors() const noexcept {
        return errors_;
    }
    void clearErrors() { errors_.clear(); }

private:
    bool submitIntent(RtsTeamScriptConfig& team,
                      std::uint64_t targetTick,
                      const RtsScriptIntent& intent);

    RtsGameSession& session_;
    std::shared_ptr<::rts::scripting::ScriptProgram> program_;
    std::shared_ptr<RtsScriptContext> context_;
    std::vector<RtsTeamScriptConfig> teams_;
    std::vector<RtsTeamScriptError> errors_;
};

[[nodiscard]] std::int64_t PackScriptEntity(ecs::Entity entity) noexcept;
[[nodiscard]] ecs::Entity UnpackScriptEntity(std::int64_t value) noexcept;

} // namespace rts::gameplay::scripting
