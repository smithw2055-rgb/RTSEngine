#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::presentation {

using AnimationClipId = std::uint64_t;

enum class AnimationAction : std::uint8_t {
    Idle,
    Move,
    Attack,
    Cast,
    Hit,
    Death,
    Build,
    Work,
    Gather,
    Deliver
};

enum class Direction8 : std::uint8_t {
    South,
    SouthWest,
    West,
    NorthWest,
    North,
    NorthEast,
    East,
    SouthEast
};

struct AnimationBinding2D final {
    AnimationAction action{AnimationAction::Idle};
    Direction8 direction{Direction8::South};
    AnimationClipId clipId{};
    AnimationAction fallbackAction{AnimationAction::Idle};
    std::uint8_t priority{};
    bool loop{true};
    bool interruptible{true};
};

class AnimationSet2D final {
public:
    bool set(AnimationBinding2D binding) {
        if (binding.clipId == 0) return false;
        const auto iterator = lower(binding.action, binding.direction);
        if (iterator != bindings_.end() &&
            iterator->action == binding.action &&
            iterator->direction == binding.direction) {
            *iterator = binding;
        } else {
            bindings_.insert(iterator, binding);
        }
        return true;
    }

    const AnimationBinding2D* resolve(
        AnimationAction action,
        Direction8 direction) const noexcept {
        if (const auto* exact = find(action, direction)) return exact;
        if (const auto* actionSouth = find(action, Direction8::South)) {
            return actionSouth;
        }
        if (const auto* idleDirection =
                find(AnimationAction::Idle, direction)) {
            return idleDirection;
        }
        return find(AnimationAction::Idle, Direction8::South);
    }

    std::size_t size() const noexcept { return bindings_.size(); }

private:
    using Iterator = std::vector<AnimationBinding2D>::iterator;
    using ConstIterator = std::vector<AnimationBinding2D>::const_iterator;

    static bool keyLess(
        const AnimationBinding2D& value,
        AnimationAction action,
        Direction8 direction) noexcept {
        return value.action < action ||
               (value.action == action && value.direction < direction);
    }

    Iterator lower(AnimationAction action, Direction8 direction) {
        return std::lower_bound(
            bindings_.begin(), bindings_.end(),
            std::pair<AnimationAction, Direction8>{action, direction},
            [](const AnimationBinding2D& value, const auto& key) {
                return keyLess(value, key.first, key.second);
            });
    }

    ConstIterator lower(
        AnimationAction action,
        Direction8 direction) const noexcept {
        return std::lower_bound(
            bindings_.begin(), bindings_.end(),
            std::pair<AnimationAction, Direction8>{action, direction},
            [](const AnimationBinding2D& value, const auto& key) {
                return keyLess(value, key.first, key.second);
            });
    }

    const AnimationBinding2D* find(
        AnimationAction action,
        Direction8 direction) const noexcept {
        const auto iterator = lower(action, direction);
        return iterator != bindings_.end() &&
                       iterator->action == action &&
                       iterator->direction == direction
            ? &*iterator
            : nullptr;
    }

    std::vector<AnimationBinding2D> bindings_;
};

struct AnimatorState2D final {
    AnimationAction action{AnimationAction::Idle};
    Direction8 direction{Direction8::South};
    AnimationClipId clipId{};
    std::uint64_t startedMilliseconds{};
    std::uint64_t phaseOffsetMilliseconds{};
    std::uint8_t priority{};
    bool loop{true};
    bool interruptible{true};
    bool changed{};
};

class Animator2D final {
public:
    explicit Animator2D(std::uint64_t stableViewId = 1) noexcept
        : stableViewId_(stableViewId == 0 ? 1 : stableViewId) {}

    bool setBaseState(
        const AnimationSet2D& set,
        AnimationAction action,
        Direction8 direction,
        std::uint64_t nowMilliseconds) {
        baseAction_ = action;
        baseDirection_ = direction;
        if (state_.clipId == 0 || state_.loop) {
            return transition(set, action, direction, nowMilliseconds, false);
        }
        return true;
    }

    bool play(
        const AnimationSet2D& set,
        AnimationAction action,
        Direction8 direction,
        std::uint64_t nowMilliseconds,
        bool restart = true) {
        return transition(set, action, direction, nowMilliseconds, restart);
    }

    bool completeOneShot(
        const AnimationSet2D& set,
        std::uint64_t nowMilliseconds) {
        if (state_.clipId == 0 || state_.loop) return false;
        const auto fallback = state_.action == AnimationAction::Death
            ? AnimationAction::Death
            : baseAction_;
        return transition(
            set, fallback, baseDirection_, nowMilliseconds, true, true);
    }

    void setDirection(
        const AnimationSet2D& set,
        Direction8 direction,
        std::uint64_t nowMilliseconds) {
        baseDirection_ = direction;
        (void)transition(
            set, state_.action, direction, nowMilliseconds, false, true);
    }

    const AnimatorState2D& state() const noexcept { return state_; }

    bool consumeChanged() noexcept {
        const auto changed = state_.changed;
        state_.changed = false;
        return changed;
    }

private:
    bool transition(
        const AnimationSet2D& set,
        AnimationAction action,
        Direction8 direction,
        std::uint64_t nowMilliseconds,
        bool restart,
        bool force = false) {
        const auto* binding = set.resolve(action, direction);
        if (!binding) return false;
        if (!force && state_.clipId != 0 && !state_.interruptible &&
            !state_.loop && binding->priority < state_.priority) {
            return false;
        }
        if (!restart && state_.clipId == binding->clipId &&
            state_.direction == binding->direction) {
            return true;
        }
        state_.action = binding->action;
        state_.direction = binding->direction;
        state_.clipId = binding->clipId;
        state_.startedMilliseconds = nowMilliseconds;
        state_.phaseOffsetMilliseconds = binding->loop
            ? stablePhase(stableViewId_, binding->clipId)
            : 0;
        state_.priority = binding->priority;
        state_.loop = binding->loop;
        state_.interruptible = binding->interruptible;
        state_.changed = true;
        return true;
    }

    static std::uint64_t stablePhase(
        std::uint64_t viewId,
        std::uint64_t clipId) noexcept {
        auto value = viewId ^ (clipId + 0x9e3779b97f4a7c15ull +
                               (viewId << 6u) + (viewId >> 2u));
        value ^= value >> 30u;
        value *= 0xbf58476d1ce4e5b9ull;
        value ^= value >> 27u;
        value *= 0x94d049bb133111ebull;
        value ^= value >> 31u;
        return value;
    }

    std::uint64_t stableViewId_{1};
    AnimationAction baseAction_{AnimationAction::Idle};
    Direction8 baseDirection_{Direction8::South};
    AnimatorState2D state_{};
};

} // namespace rts::presentation
