#pragma once

#include <RTSEngine/Assets/AssetTypes.h>
#include <rts/foundation/BinaryArchive.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::assets {

class CookedAssetCodec final {
public:
    static constexpr std::uint32_t kMagic = 0x31415452u; // "RTA1"
    static constexpr std::uint16_t kVersion = 1u;
    static constexpr std::uint32_t kMaximumDependencies = 4096u;
    static constexpr std::uint32_t kMaximumPayloadBytes =
        256u * 1024u * 1024u;

    static std::uint64_t payloadHash(
        const std::vector<std::uint8_t>& payload) noexcept {
        foundation::CanonicalHash hash;
        hash.WriteU32(static_cast<std::uint32_t>(payload.size()));
        for (const auto byte : payload) hash.WriteU8(byte);
        return hash.Value();
    }

    static bool canonicalize(CookedAsset& asset) {
        if (!asset.key.valid() || asset.schemaVersion == 0 ||
            asset.payload.size() > kMaximumPayloadBytes ||
            asset.dependencies.size() > kMaximumDependencies) {
            return false;
        }
        std::sort(asset.dependencies.begin(), asset.dependencies.end());
        for (std::size_t index = 0; index < asset.dependencies.size(); ++index) {
            const auto& dependency = asset.dependencies[index];
            if (!dependency.key.valid() ||
                dependency.minimumSchemaVersion == 0 ||
                dependency.key == asset.key ||
                (index != 0 &&
                 asset.dependencies[index - 1].key == dependency.key)) {
                return false;
            }
        }
        asset.payloadHash = payloadHash(asset.payload);
        return true;
    }

    static std::vector<std::uint8_t> encode(CookedAsset asset) {
        if (!canonicalize(asset)) return {};

        foundation::BinaryWriter writer;
        writer.writeU32(kMagic);
        writer.writeU16(kVersion);
        writer.writeU16(static_cast<std::uint16_t>(asset.key.type));
        writer.writeU64(asset.key.id);
        writer.writeU32(asset.schemaVersion);
        writer.writeU32(static_cast<std::uint32_t>(asset.dependencies.size()));
        for (const auto& dependency : asset.dependencies) {
            writer.writeU16(static_cast<std::uint16_t>(dependency.key.type));
            writer.writeU64(dependency.key.id);
            writer.writeU32(dependency.minimumSchemaVersion);
        }
        writer.writeU64(asset.payloadHash);
        writer.writeU32(static_cast<std::uint32_t>(asset.payload.size()));
        writer.writeBytes(asset.payload);
        return writer.take();
    }

    static bool decode(const std::vector<std::uint8_t>& bytes,
                       CookedAsset& output) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint16_t type = 0;
        std::uint32_t dependencyCount = 0;
        std::uint32_t payloadBytes = 0;
        CookedAsset candidate;
        if (!reader.readU32(magic) || !reader.readU16(version) ||
            !reader.readU16(type) || magic != kMagic || version != kVersion ||
            type > static_cast<std::uint16_t>(AssetType::Binary) ||
            !reader.readU64(candidate.key.id) ||
            !reader.readU32(candidate.schemaVersion) ||
            !reader.readU32(dependencyCount) ||
            dependencyCount > kMaximumDependencies ||
            candidate.key.id == 0 || candidate.schemaVersion == 0) {
            return false;
        }
        candidate.key.type = static_cast<AssetType>(type);
        if (!candidate.key.valid()) return false;

        candidate.dependencies.resize(dependencyCount);
        AssetKey previous{};
        for (auto& dependency : candidate.dependencies) {
            if (!reader.readU16(type) ||
                type == static_cast<std::uint16_t>(AssetType::Unknown) ||
                type > static_cast<std::uint16_t>(AssetType::Binary) ||
                !reader.readU64(dependency.key.id) ||
                !reader.readU32(dependency.minimumSchemaVersion) ||
                dependency.key.id == 0 ||
                dependency.minimumSchemaVersion == 0) {
                return false;
            }
            dependency.key.type = static_cast<AssetType>(type);
            if (dependency.key == candidate.key ||
                (previous.valid() && !(previous < dependency.key))) {
                return false;
            }
            previous = dependency.key;
        }

        if (!reader.readU64(candidate.payloadHash) ||
            !reader.readU32(payloadBytes) ||
            payloadBytes > kMaximumPayloadBytes ||
            !reader.readBytes(payloadBytes, candidate.payload,
                              kMaximumPayloadBytes) ||
            !reader.atEnd() ||
            candidate.payloadHash != payloadHash(candidate.payload)) {
            return false;
        }
        output = std::move(candidate);
        return true;
    }
};

inline std::vector<std::uint8_t> EncodeCookedAsset(CookedAsset asset) {
    return CookedAssetCodec::encode(std::move(asset));
}

inline bool DecodeCookedAsset(const std::vector<std::uint8_t>& bytes,
                              CookedAsset& output) {
    return CookedAssetCodec::decode(bytes, output);
}

} // namespace rts::assets
