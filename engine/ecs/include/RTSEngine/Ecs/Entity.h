#pragma once

#include <cstdint>

namespace rts::ecs {

struct Entity {
    std::uint32_t index{0};
    std::uint32_t generation{0};

    constexpr bool valid() const noexcept {
        return index != 0 && generation != 0;
    }

    friend constexpr bool operator==(Entity a, Entity b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }

    friend constexpr bool operator!=(Entity a, Entity b) noexcept {
        return !(a == b);
    }

    friend constexpr bool operator<(Entity a, Entity b) noexcept {
        return a.index < b.index ||
               (a.index == b.index && a.generation < b.generation);
    }
};

struct DeferredEntity {
    std::uint32_t id{0};

    constexpr bool valid() const noexcept {
        return id != 0;
    }
};

} // namespace rts::ecs
