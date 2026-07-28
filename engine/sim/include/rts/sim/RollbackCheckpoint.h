#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rts::sim {

struct RollbackCheckpoint final {
    std::uint64_t resumeTick{};
    std::uint64_t authoritativeHash{};
    std::vector<std::uint8_t> archive;
};

class RollbackCheckpointRing final {
public:
    RollbackCheckpointRing(
        std::uint32_t intervalTicks = 8,
        std::size_t capacity = 32) noexcept
        : intervalTicks_(std::max<std::uint32_t>(1, intervalTicks)),
          capacity_(std::max<std::size_t>(1, capacity)) {}

    std::uint32_t intervalTicks() const noexcept { return intervalTicks_; }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t size() const noexcept { return checkpoints_.size(); }
    bool empty() const noexcept { return checkpoints_.empty(); }

    bool shouldCapture(std::uint64_t resumeTick) const noexcept {
        return checkpoints_.empty() ||
               resumeTick % intervalTicks_ == 0;
    }

    bool capture(
        RollbackCheckpoint checkpoint,
        bool force = false) {
        if (checkpoint.archive.empty() ||
            (!force && !shouldCapture(checkpoint.resumeTick))) {
            return false;
        }
        const auto found = std::lower_bound(
            checkpoints_.begin(), checkpoints_.end(),
            checkpoint.resumeTick,
            [](const RollbackCheckpoint& value, std::uint64_t tick) {
                return value.resumeTick < tick;
            });
        if (found != checkpoints_.end() &&
            found->resumeTick == checkpoint.resumeTick) {
            *found = std::move(checkpoint);
        } else {
            checkpoints_.insert(found, std::move(checkpoint));
        }
        while (checkpoints_.size() > capacity_) {
            checkpoints_.erase(checkpoints_.begin());
        }
        return true;
    }

    const RollbackCheckpoint* latestAtOrBefore(
        std::uint64_t resumeTick) const noexcept {
        const auto found = std::upper_bound(
            checkpoints_.begin(), checkpoints_.end(),
            resumeTick,
            [](std::uint64_t tick, const RollbackCheckpoint& value) {
                return tick < value.resumeTick;
            });
        return found == checkpoints_.begin() ? nullptr : &*(found - 1);
    }

    void discardAfter(std::uint64_t resumeTick) {
        checkpoints_.erase(
            std::upper_bound(
                checkpoints_.begin(), checkpoints_.end(),
                resumeTick,
                [](std::uint64_t tick, const RollbackCheckpoint& value) {
                    return tick < value.resumeTick;
                }),
            checkpoints_.end());
    }

    void clear() noexcept { checkpoints_.clear(); }

    const std::vector<RollbackCheckpoint>& checkpoints() const noexcept {
        return checkpoints_;
    }

private:
    std::uint32_t intervalTicks_{8};
    std::size_t capacity_{32};
    std::vector<RollbackCheckpoint> checkpoints_;
};

} // namespace rts::sim
