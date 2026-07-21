#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rts::foundation {

class CanonicalHash final {
public:
    static constexpr std::uint64_t kOffset = 14695981039346656037ull;
    static constexpr std::uint64_t kPrime = 1099511628211ull;

    constexpr CanonicalHash() noexcept = default;

    void WriteU8(std::uint8_t value) noexcept { Mix(value); }
    void WriteBool(bool value) noexcept { WriteU8(value ? 1u : 0u); }

    void WriteU16(std::uint16_t value) noexcept {
        for (unsigned shift = 0; shift < 16; shift += 8) Mix(static_cast<std::uint8_t>(value >> shift));
    }

    void WriteU32(std::uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) Mix(static_cast<std::uint8_t>(value >> shift));
    }

    void WriteU64(std::uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) Mix(static_cast<std::uint8_t>(value >> shift));
    }

    void WriteI32(std::int32_t value) noexcept { WriteU32(static_cast<std::uint32_t>(value)); }
    void WriteString(std::string_view value) noexcept {
        WriteU32(static_cast<std::uint32_t>(value.size()));
        for (const char character : value) Mix(static_cast<std::uint8_t>(character));
    }

    [[nodiscard]] constexpr std::uint64_t Value() const noexcept { return value_; }

private:
    void Mix(std::uint8_t byte) noexcept {
        value_ ^= byte;
        value_ *= kPrime;
    }

    std::uint64_t value_{kOffset};
};

} // namespace rts::foundation
