#pragma once

#include <RTSEngine/Ecs/Entity.h>
#include <RTSEngine/Rts/Navigation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

struct MovementIntent final {
    ecs::Entity entity{};
    GridPoint source{};
    GridPoint destination{};
    std::uint32_t blockedTicks{};
};

class MovementReservationRuntime final {
public:
    MovementReservationRuntime(
        std::int32_t width = 32,
        std::int32_t height = 32)
        : width_(std::max<std::int32_t>(1, width)),
          height_(std::max<std::int32_t>(1, height)),
          firstOccupant_(cellCount()),
          occupantCount_(cellCount()),
          destinationWinner_(cellCount(), -1) {}

    std::int32_t width() const noexcept { return width_; }
    std::int32_t height() const noexcept { return height_; }
    std::size_t intentCapacity() const noexcept { return intents_.capacity(); }
    std::size_t rejectedCapacity() const noexcept { return rejected_.capacity(); }
    std::size_t cellCapacity() const noexcept {
        return firstOccupant_.capacity() + occupantCount_.capacity() +
               destinationWinner_.capacity();
    }

    void clearOccupancy() noexcept {
        std::fill(firstOccupant_.begin(), firstOccupant_.end(), ecs::Entity{});
        std::fill(occupantCount_.begin(), occupantCount_.end(), 0u);
    }

    bool addOccupant(ecs::Entity entity, GridPoint point) noexcept {
        if (!entity.valid() || !contains(point)) return false;
        const auto cell = index(point);
        auto& count = occupantCount_[cell];
        if (count == 0 || entity < firstOccupant_[cell]) {
            firstOccupant_[cell] = entity;
        }
        if (count != std::numeric_limits<std::uint16_t>::max()) ++count;
        return true;
    }

    bool occupied(GridPoint point) const noexcept {
        return contains(point) && occupantCount_[index(point)] != 0;
    }

    bool occupiedByOther(GridPoint point, ecs::Entity entity) const noexcept {
        if (!contains(point)) return true;
        const auto cell = index(point);
        const auto count = occupantCount_[cell];
        return count > 1 || (count == 1 && firstOccupant_[cell] != entity);
    }

    bool moveOccupant(
        ecs::Entity entity,
        GridPoint source,
        GridPoint destination) noexcept {
        if (!entity.valid() || !contains(source) || !contains(destination)) {
            return false;
        }
        const auto sourceCell = index(source);
        const auto destinationCell = index(destination);
        if (occupantCount_[sourceCell] != 1 ||
            firstOccupant_[sourceCell] != entity ||
            occupantCount_[destinationCell] != 0) {
            return false;
        }
        occupantCount_[sourceCell] = 0;
        firstOccupant_[sourceCell] = {};
        occupantCount_[destinationCell] = 1;
        firstOccupant_[destinationCell] = entity;
        return true;
    }

    void beginIntents() {
        intents_.clear();
        rejected_.clear();
        resolution_.clear();
        std::fill(destinationWinner_.begin(), destinationWinner_.end(), -1);
    }

    bool addIntent(MovementIntent intent) {
        if (!intent.entity.valid() || !contains(intent.source) ||
            !contains(intent.destination)) {
            return false;
        }
        intents_.push_back(intent);
        return true;
    }

    void arbitrate() {
        std::sort(
            intents_.begin(), intents_.end(),
            [](const MovementIntent& a, const MovementIntent& b) {
                return a.entity < b.entity;
            });
        intents_.erase(
            std::unique(
                intents_.begin(), intents_.end(),
                [](const MovementIntent& a, const MovementIntent& b) {
                    return a.entity == b.entity;
                }),
            intents_.end());

        resolution_.assign(intents_.size(), kUnknown);
        for (std::size_t indexValue = 0; indexValue < intents_.size(); ++indexValue) {
            const auto cell = index(intents_[indexValue].destination);
            const auto current = destinationWinner_[cell];
            if (current < 0 || better(
                    intents_[indexValue],
                    intents_[static_cast<std::size_t>(current)])) {
                destinationWinner_[cell] = static_cast<std::int32_t>(indexValue);
            }
        }

        for (std::size_t indexValue = 0; indexValue < intents_.size(); ++indexValue) {
            resolve(indexValue);
        }

        for (std::size_t indexValue = 0; indexValue < intents_.size(); ++indexValue) {
            if (!accepted(indexValue)) rejected_.push_back(indexValue);
        }
        std::sort(
            rejected_.begin(), rejected_.end(),
            [this](std::size_t a, std::size_t b) {
                return better(intents_[a], intents_[b]);
            });
    }

    std::size_t intentCount() const noexcept { return intents_.size(); }

    const MovementIntent& intent(std::size_t indexValue) const noexcept {
        return intents_[indexValue];
    }

    bool accepted(std::size_t indexValue) const noexcept {
        return indexValue < resolution_.size() &&
               resolution_[indexValue] == kAccepted;
    }

    const std::vector<std::size_t>& rejected() const noexcept {
        return rejected_;
    }

private:
    static constexpr std::uint8_t kUnknown = 0;
    static constexpr std::uint8_t kVisiting = 1;
    static constexpr std::uint8_t kAccepted = 2;
    static constexpr std::uint8_t kRejected = 3;

    bool contains(GridPoint point) const noexcept {
        return point.x >= 0 && point.y >= 0 &&
               point.x < width_ && point.y < height_;
    }

    std::size_t cellCount() const noexcept {
        return static_cast<std::size_t>(width_) *
               static_cast<std::size_t>(height_);
    }

    std::size_t index(GridPoint point) const noexcept {
        return static_cast<std::size_t>(point.y) *
                   static_cast<std::size_t>(width_) +
               static_cast<std::size_t>(point.x);
    }

    static bool better(
        const MovementIntent& a,
        const MovementIntent& b) noexcept {
        if (a.blockedTicks != b.blockedTicks) {
            return a.blockedTicks > b.blockedTicks;
        }
        return a.entity < b.entity;
    }

    bool isWinner(std::size_t indexValue) const noexcept {
        const auto cell = index(intents_[indexValue].destination);
        return destinationWinner_[cell] ==
               static_cast<std::int32_t>(indexValue);
    }

    std::size_t findIntent(ecs::Entity entity) const noexcept {
        const auto iterator = std::lower_bound(
            intents_.begin(), intents_.end(), entity,
            [](const MovementIntent& intentValue, ecs::Entity entityValue) {
                return intentValue.entity < entityValue;
            });
        if (iterator == intents_.end() || iterator->entity != entity) {
            return intents_.size();
        }
        return static_cast<std::size_t>(iterator - intents_.begin());
    }

    bool resolve(std::size_t indexValue) {
        auto& state = resolution_[indexValue];
        if (state == kAccepted) return true;
        if (state == kRejected || state == kVisiting) {
            state = kRejected;
            return false;
        }
        if (!isWinner(indexValue)) {
            state = kRejected;
            return false;
        }

        state = kVisiting;
        const auto& intentValue = intents_[indexValue];
        const auto destinationCell = index(intentValue.destination);
        const auto count = occupantCount_[destinationCell];
        if (count == 0 ||
            (count == 1 && firstOccupant_[destinationCell] == intentValue.entity)) {
            state = kAccepted;
            return true;
        }
        if (count > 1) {
            state = kRejected;
            return false;
        }

        const auto occupantIntent = findIntent(firstOccupant_[destinationCell]);
        if (occupantIntent == intents_.size() || !resolve(occupantIntent)) {
            state = kRejected;
            return false;
        }

        state = kAccepted;
        return true;
    }

    std::int32_t width_{};
    std::int32_t height_{};
    std::vector<ecs::Entity> firstOccupant_;
    std::vector<std::uint16_t> occupantCount_;
    std::vector<std::int32_t> destinationWinner_;
    std::vector<MovementIntent> intents_;
    std::vector<std::uint8_t> resolution_;
    std::vector<std::size_t> rejected_;
};

} // namespace rts::gameplay
