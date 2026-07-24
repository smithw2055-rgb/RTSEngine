#pragma once

#include <RTSEngine/Assets/CookedAsset.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts::assets {

enum class PixelFormat : std::uint8_t {
    R8,
    Rgba8
};

constexpr std::size_t PixelBytes(PixelFormat format) noexcept {
    return format == PixelFormat::R8 ? 1u : 4u;
}

struct Texture2DContent final {
    std::uint32_t width{};
    std::uint32_t height{};
    PixelFormat format{PixelFormat::Rgba8};
    std::vector<std::uint8_t> pixels;
};

struct SpriteContent final {
    AssetKey texture{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::int32_t pivotXMilli{500};
    std::int32_t pivotYMilli{500};
    std::int32_t worldWidthMilli{1000};
    std::int32_t worldHeightMilli{1000};
};

struct AnimationFrameContent final {
    std::uint64_t spriteId{};
    std::uint32_t durationMilliseconds{1};
};

struct AnimationClipContent final {
    bool loop{true};
    std::vector<AnimationFrameContent> frames;
};

struct EffectContent final {
    std::uint64_t animationClipId{};
    std::uint32_t durationMilliseconds{1};
    bool additive{};
};

struct AudioClipContent final {
    std::uint32_t sampleRate{};
    std::uint16_t channels{};
    std::uint16_t bitsPerSample{};
    std::vector<std::uint8_t> pcm;
};

class CookedContentCodec final {
public:
    static constexpr std::uint32_t kTextureSchema = 1u;
    static constexpr std::uint32_t kSpriteSchema = 1u;
    static constexpr std::uint32_t kAnimationSchema = 1u;
    static constexpr std::uint32_t kEffectSchema = 1u;
    static constexpr std::uint32_t kAudioSchema = 1u;
    static constexpr std::uint32_t kMaximumFrames = 65536u;
    static constexpr std::uint32_t kMaximumPayloadBytes =
        CookedAssetCodec::kMaximumPayloadBytes;

    static bool encodeTexture(const Texture2DContent& content,
                              std::vector<std::uint8_t>& output);
    static bool decodeTexture(const std::vector<std::uint8_t>& bytes,
                              Texture2DContent& output);
    static bool encodeSprite(const SpriteContent& content,
                             std::vector<std::uint8_t>& output);
    static bool decodeSprite(const std::vector<std::uint8_t>& bytes,
                             SpriteContent& output);
    static bool encodeAnimation(const AnimationClipContent& content,
                                std::vector<std::uint8_t>& output);
    static bool decodeAnimation(const std::vector<std::uint8_t>& bytes,
                                AnimationClipContent& output);
    static bool encodeEffect(const EffectContent& content,
                             std::vector<std::uint8_t>& output);
    static bool decodeEffect(const std::vector<std::uint8_t>& bytes,
                             EffectContent& output);
    static bool encodeAudio(const AudioClipContent& content,
                            std::vector<std::uint8_t>& output);
    static bool decodeAudio(const std::vector<std::uint8_t>& bytes,
                            AudioClipContent& output);

    static CookedAsset textureAsset(std::uint64_t id,
                                    const Texture2DContent& content);
    static CookedAsset spriteAsset(std::uint64_t id,
                                   const SpriteContent& content);
    static CookedAsset animationAsset(
        std::uint64_t id,
        const AnimationClipContent& content);
    static CookedAsset effectAsset(std::uint64_t id,
                                   const EffectContent& content);
    static CookedAsset audioAsset(std::uint64_t id,
                                  const AudioClipContent& content);

private:
    static bool validTexture(const Texture2DContent& content) noexcept;
    static bool validSprite(const SpriteContent& content) noexcept;
    static bool validAnimation(
        const AnimationClipContent& content) noexcept;
    static bool validAudio(const AudioClipContent& content) noexcept;
};

} // namespace rts::assets
