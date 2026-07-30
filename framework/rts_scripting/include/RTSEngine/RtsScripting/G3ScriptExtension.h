#pragma once

#include <RTSEngine/Rts/G3GameSession.h>
#include <RTSEngine/RtsScripting/RtsScriptSession.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rts::gameplay::scripting {

enum class G3ScriptIntentKind : std::uint8_t {
    Self,
    Entity,
    Point
};

struct G3ScriptCommandOutcome final {
    std::uint64_t tick{};
    std::uint32_t teamId{};
    std::uint32_t sequence{};
    std::uint32_t ordinal{};
    std::uint32_t abilityId{};
    AbilitySubmitResult result{
        AbilitySubmitResult::Accepted};
};

class RtsG3ScriptExtension final {
public:
    RtsG3ScriptExtension(
        RtsG3GameSession& session,
        RtsScriptApi& api);
    ~RtsG3ScriptExtension();

    RtsG3ScriptExtension(
        const RtsG3ScriptExtension&) = delete;
    RtsG3ScriptExtension& operator=(
        const RtsG3ScriptExtension&) = delete;

    // The extension must be installed before compiling/loading a ScriptBundle,
    // because the Engine.G3 functions participate in the Host API hash.
    bool beginScope(
        std::uint32_t teamId,
        std::uint64_t targetTick,
        std::size_t maximumIntents = 256u);
    bool commit();
    void discard() noexcept;
    bool active() const noexcept;

    bool queueSelf(
        ScriptEntityId caster,
        std::uint32_t abilityId);
    bool queueEntity(
        ScriptEntityId caster,
        std::uint32_t abilityId,
        ScriptEntityId target);
    bool queuePoint(
        ScriptEntityId caster,
        std::uint32_t abilityId,
        std::int32_t x,
        std::int32_t y);

    bool hasStatus(
        ScriptEntityId entity,
        std::uint32_t statusId) const noexcept;
    std::uint32_t statusStacks(
        ScriptEntityId entity,
        std::uint32_t statusId) const noexcept;

    std::vector<std::uint8_t> encodeState() const;
    bool restoreState(
        const std::vector<std::uint8_t>& bytes);
    std::uint64_t authoritativeHash() const noexcept;

    const std::vector<G3ScriptCommandOutcome>&
    outcomes() const noexcept;
    void clearOutcomes();

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace rts::gameplay::scripting
