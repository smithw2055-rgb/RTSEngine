#pragma once

#include <RTSEngine/Roguelite/RunSaveEnvelope.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rts::roguelite {

enum class RunSaveCloudTransportError : std::uint8_t {
    None,
    NotFound,
    InvalidKey,
    InvalidEnvelope,
    MissingRevision,
    PayloadTooLarge,
    RevisionMismatch,
    IncompatibleManifest,
    IncompatibleLineage,
    TransportFailure
};

struct RunSaveCloudObject final {
    bool present{};
    std::vector<std::uint8_t> bytes;
    RunSaveEnvelopeDecodeResult decoded;

    std::uint64_t revisionId() const noexcept {
        return present && decoded.error == RunSaveEnvelopeError::None
            ? decoded.envelope.manifest.identity.cloud.revisionId
            : 0u;
    }
};

struct RunSaveCloudFetchResult final {
    RunSaveCloudTransportError error{RunSaveCloudTransportError::NotFound};
    RunSaveCloudObject object;
};

struct RunSaveCloudCompareExchangeResult final {
    RunSaveCloudTransportError error{
        RunSaveCloudTransportError::TransportFailure};
    bool applied{};
    std::uint64_t expectedRevisionId{};
    std::uint64_t actualRevisionId{};
    RunSaveCloudObject current;
};

class IRunSaveCloudTransport {
public:
    virtual ~IRunSaveCloudTransport() = default;

    virtual RunSaveCloudFetchResult fetch(
        std::string_view key) = 0;

    // expectedRevisionId == 0 means create only when the key is absent.
    virtual RunSaveCloudCompareExchangeResult compareExchange(
        std::string_view key,
        std::uint64_t expectedRevisionId,
        const std::vector<std::uint8_t>& envelopeBytes) = 0;
};

inline bool ValidateRunSaveCloudKey(std::string_view key) noexcept {
    if (key.empty() || key.size() > 128u || key.front() == '/' ||
        key.back() == '/') {
        return false;
    }
    bool previousSlash = false;
    for (const unsigned char character : key) {
        const bool slash = character == '/';
        if (!std::isalnum(character) && character != '-' &&
            character != '_' && character != '.' && !slash) {
            return false;
        }
        if (slash && previousSlash) return false;
        previousSlash = slash;
    }
    return key.find("..") == std::string_view::npos;
}

class MemoryRunSaveCloudTransport final : public IRunSaveCloudTransport {
public:
    RunSaveCloudFetchResult fetch(std::string_view key) override {
        RunSaveCloudFetchResult result;
        if (!ValidateRunSaveCloudKey(key)) {
            result.error = RunSaveCloudTransportError::InvalidKey;
            return result;
        }
        const auto iterator = find(key);
        if (iterator == records_.end() || iterator->key != key) {
            result.error = RunSaveCloudTransportError::NotFound;
            return result;
        }
        result.error = RunSaveCloudTransportError::None;
        result.object = makeObject(*iterator);
        return result;
    }

    RunSaveCloudCompareExchangeResult compareExchange(
        std::string_view key,
        std::uint64_t expectedRevisionId,
        const std::vector<std::uint8_t>& envelopeBytes) override {
        RunSaveCloudCompareExchangeResult result;
        result.expectedRevisionId = expectedRevisionId;
        if (!ValidateRunSaveCloudKey(key)) {
            result.error = RunSaveCloudTransportError::InvalidKey;
            return result;
        }
        if (envelopeBytes.size() > kMaximumEnvelopeBytes) {
            result.error = RunSaveCloudTransportError::PayloadTooLarge;
            return result;
        }

        const auto decoded = RunSaveEnvelopeCodec::decode(envelopeBytes);
        if (decoded.error != RunSaveEnvelopeError::None) {
            result.error = RunSaveCloudTransportError::InvalidEnvelope;
            return result;
        }
        const auto& candidateRevision =
            decoded.envelope.manifest.identity.cloud;
        if (!candidateRevision.tracked()) {
            result.error = RunSaveCloudTransportError::MissingRevision;
            return result;
        }

        auto iterator = find(key);
        const bool present =
            iterator != records_.end() && iterator->key == key;
        const auto actualRevisionId = present
            ? iterator->decoded.envelope.manifest.identity.cloud.revisionId
            : 0u;
        result.actualRevisionId = actualRevisionId;
        if (present) result.current = makeObject(*iterator);

        if (actualRevisionId != expectedRevisionId) {
            result.error = RunSaveCloudTransportError::RevisionMismatch;
            return result;
        }

        if (present) {
            const auto& currentManifest = iterator->decoded.envelope.manifest;
            const auto& candidateManifest = decoded.envelope.manifest;
            if (currentManifest.identity.productId !=
                    candidateManifest.identity.productId ||
                currentManifest.identity.contentManifestId !=
                    candidateManifest.identity.contentManifestId) {
                result.error =
                    RunSaveCloudTransportError::IncompatibleManifest;
                return result;
            }
            if (currentManifest.identity.cloud.lineageId !=
                candidateManifest.identity.cloud.lineageId) {
                result.error =
                    RunSaveCloudTransportError::IncompatibleLineage;
                return result;
            }
            iterator->bytes = envelopeBytes;
            iterator->decoded = decoded;
            result.current = makeObject(*iterator);
        } else {
            Record record;
            record.key.assign(key.data(), key.size());
            record.bytes = envelopeBytes;
            record.decoded = decoded;
            iterator = records_.insert(iterator, std::move(record));
            result.current = makeObject(*iterator);
        }

        result.error = RunSaveCloudTransportError::None;
        result.applied = true;
        result.actualRevisionId = result.current.revisionId();
        return result;
    }

    std::size_t size() const noexcept { return records_.size(); }

private:
    static constexpr std::size_t kMaximumEnvelopeBytes =
        static_cast<std::size_t>(
            RunSaveEnvelopeCodec::kMaximumPayloadBytes) + 4096u;

    struct Record final {
        std::string key;
        std::vector<std::uint8_t> bytes;
        RunSaveEnvelopeDecodeResult decoded;
    };

    std::vector<Record>::iterator find(std::string_view key) {
        return std::lower_bound(
            records_.begin(), records_.end(), key,
            [](const Record& record, std::string_view value) {
                return record.key < value;
            });
    }

    static RunSaveCloudObject makeObject(const Record& record) {
        RunSaveCloudObject result;
        result.present = true;
        result.bytes = record.bytes;
        result.decoded = record.decoded;
        return result;
    }

    std::vector<Record> records_;
};

} // namespace rts::roguelite
