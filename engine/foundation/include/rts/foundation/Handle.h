#pragma once

#include <cstdint>
#include <functional>

namespace rts::foundation {

template <typename Tag>
struct Handle final {
    std::uint32_t index{0};
    std::uint32_t generation{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return index != 0 && generation != 0;
    }

    friend constexpr bool operator==(Handle lhs, Handle rhs) noexcept {
        return lhs.index == rhs.index && lhs.generation == rhs.generation;
    }

    friend constexpr bool operator!=(Handle lhs, Handle rhs) noexcept {
        return !(lhs == rhs);
    }
};

} // namespace rts::foundation

namespace std {
template <typename Tag>
struct hash<rts::foundation::Handle<Tag>> {
    size_t operator()(const rts::foundation::Handle<Tag>& value) const noexcept {
        const auto packed = (static_cast<std::uint64_t>(value.generation) << 32u) | value.index;
        return std::hash<std::uint64_t>{}(packed);
    }
};
} // namespace std
