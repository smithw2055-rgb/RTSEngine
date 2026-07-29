#pragma once

#include <RTSEngine/RtsScripting/RtsEntityBehaviorRuntime.h>
#include <RTSEngine/RtsScripting/RtsScriptSessionArchive.h>

#include <cstdint>
#include <vector>

namespace rts::gameplay::scripting {

enum class RtsScriptWorldTickResult : std::uint8_t {
    Processed,
    NoCallbacks,
    InvalidRuntime,
    TimelineMismatch,
    TeamScriptFailed,
    EntityBehaviorFailed
};

class RtsScriptWorldRuntime final {
public:
    RtsScriptWorldRuntime(
        RtsGameSession& session,
        RtsScriptSession& teams,
        RtsEntityBehaviorRuntime& entities)
        : session_(session), teams_(teams), entities_(entities) {}

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] RtsScriptWorldTickResult processCompletedTick(
        std::uint64_t completedTick);

    [[nodiscard]] bool authoritativeHash(
        std::uint64_t& output,
        realscript::runtime::RuntimeError& error) const;
    [[nodiscard]] std::vector<std::uint8_t> encode(
        realscript::runtime::RuntimeError& error) const;
    [[nodiscard]] bool restore(
        const std::vector<std::uint8_t>& bytes,
        realscript::runtime::RuntimeError& error);

    [[nodiscard]] RtsGameSession& session() noexcept { return session_; }
    [[nodiscard]] RtsScriptSession& teamScripts() noexcept { return teams_; }
    [[nodiscard]] RtsEntityBehaviorRuntime& entityBehaviors() noexcept {
        return entities_;
    }

private:
    RtsGameSession& session_;
    RtsScriptSession& teams_;
    RtsEntityBehaviorRuntime& entities_;
};

class RtsScriptWorldArchive final {
public:
    static constexpr std::uint32_t kMagic = 0x34575352u; // RSW4
    static constexpr std::uint16_t kVersion = 1u;
    static constexpr std::uint32_t kMaximumTeamArchiveBytes =
        64u * 1024u * 1024u;
    static constexpr std::uint32_t kMaximumEntityStateBytes =
        64u * 1024u * 1024u;

    [[nodiscard]] static bool authoritativeHash(
        const RtsGameSession& session,
        const RtsScriptSession& teams,
        const RtsEntityBehaviorRuntime& entities,
        std::uint64_t& output,
        realscript::runtime::RuntimeError& error);

    [[nodiscard]] static std::vector<std::uint8_t> encode(
        const RtsGameSession& session,
        const RtsScriptSession& teams,
        const RtsEntityBehaviorRuntime& entities,
        realscript::runtime::RuntimeError& error);

    [[nodiscard]] static bool decode(
        const std::vector<std::uint8_t>& bytes,
        RtsGameSession& session,
        RtsScriptSession& teams,
        RtsEntityBehaviorRuntime& entities,
        realscript::runtime::RuntimeError& error);
};

} // namespace rts::gameplay::scripting
