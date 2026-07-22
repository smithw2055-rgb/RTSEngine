#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rts::sim {

class BinaryWriter final {
public:
    void writeU8(std::uint8_t value) {
        bytes_.push_back(value);
    }

    void writeBool(bool value) {
        writeU8(value ? 1u : 0u);
    }

    void writeU16(std::uint16_t value) {
        for (unsigned shift = 0; shift < 16; shift += 8) {
            writeU8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void writeU32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            writeU8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void writeU64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            writeU8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void writeI32(std::int32_t value) {
        writeU32(static_cast<std::uint32_t>(value));
    }

    void writeString(std::string_view value) {
        writeU32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    const std::vector<std::uint8_t>& bytes() const noexcept {
        return bytes_;
    }

    std::vector<std::uint8_t> take() noexcept {
        return std::move(bytes_);
    }

private:
    std::vector<std::uint8_t> bytes_;
};

class BinaryReader final {
public:
    explicit BinaryReader(const std::vector<std::uint8_t>& bytes) noexcept
        : data_(bytes.data()), size_(bytes.size()) {}

    BinaryReader(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    bool readU8(std::uint8_t& value) noexcept {
        if (!require(1)) return false;
        value = data_[offset_++];
        return true;
    }

    bool readBool(bool& value) noexcept {
        std::uint8_t raw = 0;
        if (!readU8(raw) || raw > 1) return fail();
        value = raw != 0;
        return true;
    }

    bool readU16(std::uint16_t& value) noexcept {
        value = 0;
        for (unsigned shift = 0; shift < 16; shift += 8) {
            std::uint8_t byte = 0;
            if (!readU8(byte)) return false;
            value |= static_cast<std::uint16_t>(byte) << shift;
        }
        return true;
    }

    bool readU32(std::uint32_t& value) noexcept {
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            std::uint8_t byte = 0;
            if (!readU8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }

    bool readU64(std::uint64_t& value) noexcept {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            std::uint8_t byte = 0;
            if (!readU8(byte)) return false;
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }

    bool readI32(std::int32_t& value) noexcept {
        std::uint32_t raw = 0;
        if (!readU32(raw)) return false;
        value = static_cast<std::int32_t>(raw);
        return true;
    }

    bool readString(std::string& value,
                    std::uint32_t maximumBytes = 1024u * 1024u) {
        std::uint32_t size = 0;
        if (!readU32(size) || size > maximumBytes || !require(size)) {
            return false;
        }
        value.assign(
            reinterpret_cast<const char*>(data_ + offset_), size);
        offset_ += size;
        return true;
    }

    bool ok() const noexcept { return ok_; }
    bool atEnd() const noexcept { return ok_ && offset_ == size_; }
    std::size_t remaining() const noexcept {
        return offset_ <= size_ ? size_ - offset_ : 0;
    }

private:
    bool require(std::size_t count) noexcept {
        if (!ok_ || offset_ > size_ || count > size_ - offset_) {
            return fail();
        }
        return true;
    }

    bool fail() noexcept {
        ok_ = false;
        return false;
    }

    const std::uint8_t* data_{};
    std::size_t size_{};
    std::size_t offset_{};
    bool ok_{true};
};

} // namespace rts::sim
