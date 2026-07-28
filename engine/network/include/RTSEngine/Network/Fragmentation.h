#pragma once

#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace rts::network {

struct MessageFragment final {
    std::uint64_t messageId{};
    std::uint32_t fragmentIndex{};
    std::uint32_t fragmentCount{};
    std::uint32_t totalBytes{};
    std::vector<std::uint8_t> payload;
};

inline std::vector<std::uint8_t> EncodeMessageFragment(
    const MessageFragment& fragment) {
    if (fragment.messageId == 0 || fragment.fragmentCount == 0 ||
        fragment.fragmentIndex >= fragment.fragmentCount ||
        fragment.totalBytes == 0 || fragment.payload.empty()) {
        return {};
    }
    foundation::BinaryWriter writer;
    writer.writeU32(0x31475246u);
    writer.writeU16(1u);
    writer.writeU64(fragment.messageId);
    writer.writeU32(fragment.fragmentIndex);
    writer.writeU32(fragment.fragmentCount);
    writer.writeU32(fragment.totalBytes);
    writer.writeU32(static_cast<std::uint32_t>(fragment.payload.size()));
    writer.writeBytes(fragment.payload);
    return writer.take();
}

inline bool DecodeMessageFragment(
    const std::vector<std::uint8_t>& bytes,
    std::size_t maximumFragmentBytes,
    std::size_t maximumMessageBytes,
    MessageFragment& fragment) {
    foundation::BinaryReader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint32_t payloadBytes = 0;
    return reader.readU32(magic) && reader.readU16(version) &&
           reader.readU64(fragment.messageId) &&
           reader.readU32(fragment.fragmentIndex) &&
           reader.readU32(fragment.fragmentCount) &&
           reader.readU32(fragment.totalBytes) &&
           reader.readU32(payloadBytes) &&
           magic == 0x31475246u && version == 1u &&
           fragment.messageId != 0 && fragment.fragmentCount != 0 &&
           fragment.fragmentIndex < fragment.fragmentCount &&
           fragment.totalBytes != 0 &&
           fragment.totalBytes <= maximumMessageBytes &&
           payloadBytes != 0 && payloadBytes <= maximumFragmentBytes &&
           reader.readBytes(
               payloadBytes, fragment.payload, maximumFragmentBytes) &&
           reader.atEnd();
}

class MessageFragmenter final {
public:
    explicit MessageFragmenter(
        std::size_t maximumFragmentPayload = 900u) noexcept
        : maximumFragmentPayload_(
              std::max<std::size_t>(1u, maximumFragmentPayload)) {}

    std::vector<std::vector<std::uint8_t>> split(
        std::uint64_t messageId,
        const std::vector<std::uint8_t>& message) const {
        if (messageId == 0 || message.empty() ||
            message.size() > 0xFFFFFFFFu) {
            return {};
        }
        const auto fragmentCount = static_cast<std::uint32_t>(
            (message.size() + maximumFragmentPayload_ - 1u) /
            maximumFragmentPayload_);
        std::vector<std::vector<std::uint8_t>> output;
        output.reserve(fragmentCount);
        for (std::uint32_t index = 0; index < fragmentCount; ++index) {
            const auto offset = static_cast<std::size_t>(index) *
                                maximumFragmentPayload_;
            const auto length = std::min(
                maximumFragmentPayload_, message.size() - offset);
            MessageFragment fragment;
            fragment.messageId = messageId;
            fragment.fragmentIndex = index;
            fragment.fragmentCount = fragmentCount;
            fragment.totalBytes = static_cast<std::uint32_t>(message.size());
            fragment.payload.assign(
                message.begin() + static_cast<std::ptrdiff_t>(offset),
                message.begin() + static_cast<std::ptrdiff_t>(offset + length));
            auto encoded = EncodeMessageFragment(fragment);
            if (encoded.empty()) return {};
            output.push_back(std::move(encoded));
        }
        return output;
    }

private:
    std::size_t maximumFragmentPayload_{};
};

struct ReassemblyConfig final {
    std::size_t maximumFragmentBytes{1024u};
    std::size_t maximumMessageBytes{160u * 1024u * 1024u};
    std::size_t maximumAssemblies{32u};
    std::uint64_t timeoutMs{30000u};
};

class MessageReassembler final {
public:
    explicit MessageReassembler(ReassemblyConfig config = {}) noexcept
        : config_(sanitize(config)) {}

    bool receive(
        std::uint32_t source,
        const std::vector<std::uint8_t>& bytes,
        std::uint64_t nowMs,
        std::vector<std::uint8_t>& completed) {
        completed.clear();
        MessageFragment fragment;
        if (source == 0 || !DecodeMessageFragment(
                bytes,
                config_.maximumFragmentBytes,
                config_.maximumMessageBytes,
                fragment)) {
            return false;
        }
        expire(nowMs);
        auto found = lowerAssembly(source, fragment.messageId);
        if (found == assemblies_.end() || found->source != source ||
            found->messageId != fragment.messageId) {
            if (assemblies_.size() >= config_.maximumAssemblies) return false;
            Assembly assembly;
            assembly.source = source;
            assembly.messageId = fragment.messageId;
            assembly.fragmentCount = fragment.fragmentCount;
            assembly.totalBytes = fragment.totalBytes;
            assembly.expiresAtMs = nowMs + config_.timeoutMs;
            assembly.fragments.resize(fragment.fragmentCount);
            assembly.received.resize(fragment.fragmentCount, false);
            found = assemblies_.insert(found, std::move(assembly));
        }
        if (found->fragmentCount != fragment.fragmentCount ||
            found->totalBytes != fragment.totalBytes) {
            return false;
        }
        found->expiresAtMs = nowMs + config_.timeoutMs;
        if (!found->received[fragment.fragmentIndex]) {
            found->received[fragment.fragmentIndex] = true;
            found->fragments[fragment.fragmentIndex] = std::move(fragment.payload);
            ++found->receivedCount;
        }
        if (found->receivedCount != found->fragmentCount) return true;

        std::size_t total = 0;
        for (const auto& part : found->fragments) total += part.size();
        if (total != found->totalBytes) {
            assemblies_.erase(found);
            return false;
        }
        completed.reserve(total);
        for (const auto& part : found->fragments) {
            completed.insert(completed.end(), part.begin(), part.end());
        }
        assemblies_.erase(found);
        return true;
    }

    void expire(std::uint64_t nowMs) {
        assemblies_.erase(
            std::remove_if(
                assemblies_.begin(), assemblies_.end(),
                [nowMs](const Assembly& value) {
                    return value.expiresAtMs <= nowMs;
                }),
            assemblies_.end());
    }

    std::size_t pendingAssemblies() const noexcept {
        return assemblies_.size();
    }

private:
    struct Assembly final {
        std::uint32_t source{};
        std::uint64_t messageId{};
        std::uint32_t fragmentCount{};
        std::uint32_t totalBytes{};
        std::uint32_t receivedCount{};
        std::uint64_t expiresAtMs{};
        std::vector<std::vector<std::uint8_t>> fragments;
        std::vector<bool> received;
    };

    using Iterator = std::vector<Assembly>::iterator;

    static ReassemblyConfig sanitize(ReassemblyConfig value) noexcept {
        value.maximumFragmentBytes = std::max<std::size_t>(
            1u, value.maximumFragmentBytes);
        value.maximumMessageBytes = std::max(
            value.maximumFragmentBytes, value.maximumMessageBytes);
        value.maximumAssemblies = std::max<std::size_t>(
            1u, value.maximumAssemblies);
        value.timeoutMs = std::max<std::uint64_t>(1u, value.timeoutMs);
        return value;
    }

    Iterator lowerAssembly(
        std::uint32_t source,
        std::uint64_t messageId) noexcept {
        const auto identity = std::make_tuple(source, messageId);
        return std::lower_bound(
            assemblies_.begin(), assemblies_.end(), identity,
            [](const Assembly& value, const auto& target) {
                return std::make_tuple(value.source, value.messageId) < target;
            });
    }

    ReassemblyConfig config_;
    std::vector<Assembly> assemblies_;
};

} // namespace rts::network
