#pragma once

#include <RTSEngine/Assets/AssetTypes.h>

#include <cstdint>
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
        const std::vector<std::uint8_t>& payload) noexcept;
    static bool canonicalize(CookedAsset& asset);
    static std::vector<std::uint8_t> encode(CookedAsset asset);
    static bool decode(const std::vector<std::uint8_t>& bytes,
                       CookedAsset& output);
};

std::vector<std::uint8_t> EncodeCookedAsset(CookedAsset asset);
bool DecodeCookedAsset(const std::vector<std::uint8_t>& bytes,
                       CookedAsset& output);

} // namespace rts::assets
