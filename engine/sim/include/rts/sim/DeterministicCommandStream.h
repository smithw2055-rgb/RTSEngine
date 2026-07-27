#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rts::sim {

// Command must expose targetTick, issuer, and sequence fields. The stream owns
// ordering, duplicate suppression, skipped-Tick cleanup, late-command rejection,
// and a canonical persistence state shared by all orchestration layers.
enum class CommandSubmitResult : std::uint8_t {
    Accepted,
    DuplicateIdentity,
    Late
};

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

        // A command identity is globally unique within the pending stream.
        // Silently accepting a second payload for the same identity makes the
        // authoritative result depend on producer or network arrival order.
        const auto duplicate = std::find_if(
            commands_.begin(), commands_.end(),
            [&](const Command& current) {
                return sameIdentity(current, command);
            });
        if (duplicate != commands_.end()) {
            return CommandSubmitResult::DuplicateIdentity;
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

        sortAndDeduplicate(result);
        return result;
    }

    State snapshot() const {
        State result{committedThrough_, commands_};
        sortAndDeduplicate(result.pending);
        return result;
    }

    bool restore(State state) {
        if (std::any_of(
                state.pending.begin(),
                state.pending.end(),
                [&](const Command& command) {
                    return command.targetTick < state.committedThrough;
                })) {
            return false;
        }
        sortAndDeduplicate(state.pending);
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

    static bool less(const Command& a, const Command& b) {
        if (a.targetTick != b.targetTick) {
            return a.targetTick < b.targetTick;
        }
        if (a.issuer != b.issuer) return a.issuer < b.issuer;
        return a.sequence < b.sequence;
    }

    static void sortAndDeduplicate(std::vector<Command>& commands) {
        std::stable_sort(commands.begin(), commands.end(), less);
        commands.erase(
            std::unique(
                commands.begin(), commands.end(),
                [](const Command& a, const Command& b) {
                    return sameIdentity(a, b);
                }),
            commands.end());
    }

    std::vector<Command> commands_;
    std::uint64_t committedThrough_{};
};

} // namespace rts::sim
