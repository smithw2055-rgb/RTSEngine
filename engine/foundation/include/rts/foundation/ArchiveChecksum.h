#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts::foundation {

// FNV-1a is used as a deterministic corruption checksum, not as a security or
// authenticity primitive. Save authenticity belongs to the product/platform
// layer when signed or encrypted saves are required.
inline std::uint64_t ArchiveChecksum(
    const std::uint8_t* data,
    std::size_t size) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t value = offset;
    for (std::size_t index = 0; index < size; ++index) {
        value ^= data[index];
        value *= prime;
    }
    return value;
}

inline std::uint64_t ArchiveChecksum(
    const std::vector<std::uint8_t>& bytes) noexcept {
    return ArchiveChecksum(bytes.data(), bytes.size());
}

} // namespace rts::foundation
