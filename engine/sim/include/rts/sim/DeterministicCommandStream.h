#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace rts::sim {

// Command must expose targetTick, issuer, and sequence fields. The stream owns
// ordering, idempotent retry handling, conflicting duplicate rejection,
// skipped-Tick cleanup, late-command rejection, and canonical persistence.
enum class CommandSubmitResult : std::uint8_t {
    Accepted,
    DuplicateIdentity,
    Late
};

namespace detail {

template<class T, class = void>
struct HasType : std::false_type {};
template<class T>
struct HasType<T, std::void_t<decltype(std::declval<const T&>().type)>>
    : std::true_type {};

template<class T, class = void>
struct HasSubject : std::false_type {};
template<class T>
struct HasSubject<T, std::void_t<decltype(std::declval<const T&>().subject)>>
    : std::true_type {};

template<class T, class = void>
struct HasTargetX : std::false_type {};
template<class T>
struct HasTargetX<T, std::void_t<decltype(std::declval<const T&>().targetX)>>
    : std::true_type {};

template<class T, class = void>
struct HasTargetY : std::false_type {};
template<class T>
struct HasTargetY<T, std::void_t<decltype(std::declval<const T&>().targetY)>>
    : std::true_type {};

template<class T, class = void>
struct HasAppend : std::false_type {};
template<class T>
struct HasAppend<T, std::void_t<decltype(std::declval<const T&>().append)>>
    : std::true_type {};

template<class T, class = void>
struct HasDefinitionId : std::false_type {};
template<class T>
struct HasDefinitionId<
    T, std::void_t<decltype(std::declval<const T&>().definitionId)>>
    : std::true_type {};

template<class T, class = void>
struct HasObjectId : std::false_type {};
template<class T>
struct HasObjectId<T, std::void_t<decltype(std::declval<const T&>().objectId)>>
    : std::true_type {};

template<class T, class = void>
struct HasTargetEntity : std::false_type {};
template<class T>
struct HasTargetEntity<
    T, std::void_t<decltype(std::declval<const T&>().targetEntity)>>
    : std::true_type {};

template<class T, class = void>
struct HasPayload : std::false_type {};
template<class T>
struct HasPayload<T, std::void_t<decltype(std::declval<const T&>().payload)>>
    : std::true_type {};

} // namespace detail

template<class Command>
class DeterministicCommandStream final {
public:
    struct State {
        std::uint64_t committedThrough{};
        std::vector<Command> pending;
    };

    CommandSubmitResult submitDetailed(Command command) {
        if (command.targetTick < committedThrough_) {
            return CommandSubmitResult::Late;
        }

        const auto duplicate = std::find_if(
            commands_.begin(), commands_.end(),
            [&](const Command& current) {
                return sameIdentity(current, command);
            });
        if (duplicate != commands_.end()) {
            // Re-sending the exact command is an idempotent success. Reusing
            // the identity for another payload is a deterministic conflict.
            return samePayload(*duplicate, command)
                ? CommandSubmitResult::Accepted
                : CommandSubmitResult::DuplicateIdentity;
        }

        commands_.push_back(std::move(command));
        return CommandSubmitResult::Accepted;
    }

    bool submit(Command command) {
        return submitDetailed(std::move(command)) ==
               CommandSubmitResult::Accepted;
    }

    std::vector<Command> consume(std::uint64_t tick) {
        if (tick < committedThrough_) return {};
        committedThrough_ = tick + 1;

        std::vector<Command> result;
        std::vector<Command> remaining;
        result.reserve(commands_.size());
        remaining.reserve(commands_.size());

        for (auto& command : commands_) {
            if (command.targetTick == tick) {
                result.push_back(std::move(command));
            } else if (command.targetTick > tick) {
                remaining.push_back(std::move(command));
            }
            // Commands older than the consumed tick are deterministically
            // discarded. submit() prevents new late commands from entering.
        }
        commands_ = std::move(remaining);

        (void)normalize(result);
        return result;
    }

    State snapshot() const {
        State result{committedThrough_, commands_};
        (void)normalize(result.pending);
        return result;
    }

    bool restore(State state) {
        if (std::any_of(
                state.pending.begin(),
                state.pending.end(),
                [&](const Command& command) {
                    return command.targetTick < state.committedThrough;
                }) ||
            !normalize(state.pending)) {
            return false;
        }
        committedThrough_ = state.committedThrough;
        commands_ = std::move(state.pending);
        return true;
    }

    void clearPending() noexcept { commands_.clear(); }

    std::size_t pending() const noexcept { return commands_.size(); }
    std::uint64_t committedThrough() const noexcept {
        return committedThrough_;
    }

private:
    static bool sameIdentity(const Command& a, const Command& b) noexcept {
        return a.targetTick == b.targetTick &&
               a.issuer == b.issuer &&
               a.sequence == b.sequence;
    }

    static bool samePayload(const Command& a, const Command& b) noexcept {
        bool same = true;
        if constexpr (detail::HasType<Command>::value) {
            same = same && a.type == b.type;
        }
        if constexpr (detail::HasSubject<Command>::value) {
            same = same && a.subject == b.subject;
        }
        if constexpr (detail::HasTargetX<Command>::value) {
            same = same && a.targetX == b.targetX;
        }
        if constexpr (detail::HasTargetY<Command>::value) {
            same = same && a.targetY == b.targetY;
        }
        if constexpr (detail::HasAppend<Command>::value) {
            same = same && a.append == b.append;
        }
        if constexpr (detail::HasDefinitionId<Command>::value) {
            same = same && a.definitionId == b.definitionId;
        }
        if constexpr (detail::HasObjectId<Command>::value) {
            same = same && a.objectId == b.objectId;
        }
        if constexpr (detail::HasTargetEntity<Command>::value) {
            same = same && a.targetEntity == b.targetEntity;
        }
        if constexpr (detail::HasPayload<Command>::value) {
            same = same && a.payload == b.payload;
        }
        return same;
    }

    static bool less(const Command& a, const Command& b) {
        if (a.targetTick != b.targetTick) {
            return a.targetTick < b.targetTick;
        }
        if (a.issuer != b.issuer) return a.issuer < b.issuer;
        return a.sequence < b.sequence;
    }

    static bool normalize(std::vector<Command>& commands) {
        std::stable_sort(commands.begin(), commands.end(), less);
        auto output = commands.begin();
        for (auto current = commands.begin(); current != commands.end();
             ++current) {
            if (output != commands.begin() &&
                sameIdentity(*(output - 1), *current)) {
                if (!samePayload(*(output - 1), *current)) return false;
                continue;
            }
            if (output != current) *output = std::move(*current);
            ++output;
        }
        commands.erase(output, commands.end());
        return true;
    }

    std::vector<Command> commands_;
    std::uint64_t committedThrough_{};
};

} // namespace rts::sim
