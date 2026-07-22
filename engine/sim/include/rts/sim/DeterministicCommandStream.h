#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rts::sim {

// Command must expose targetTick, issuer, and sequence fields. The stream owns
// ordering, duplicate suppression, and the late-command boundary shared by the
// RTS, tower-defense, and roguelite orchestration layers.
template<class Command>
class DeterministicCommandStream final {
public:
    bool submit(Command command) {
        if (command.targetTick < committedThrough_) return false;
        commands_.push_back(std::move(command));
        return true;
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

        std::stable_sort(result.begin(), result.end(),
                         [](const Command& a, const Command& b) {
                             return a.issuer != b.issuer
                                 ? a.issuer < b.issuer
                                 : a.sequence < b.sequence;
                         });
        result.erase(
            std::unique(result.begin(), result.end(),
                        [](const Command& a, const Command& b) {
                            return a.issuer == b.issuer &&
                                   a.sequence == b.sequence;
                        }),
            result.end());
        return result;
    }

    void clearPending() noexcept { commands_.clear(); }

    std::size_t pending() const noexcept { return commands_.size(); }
    std::uint64_t committedThrough() const noexcept {
        return committedThrough_;
    }

private:
    std::vector<Command> commands_;
    std::uint64_t committedThrough_{};
};

} // namespace rts::sim
