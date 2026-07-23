#pragma once

#include <RTSEngine/Assets/CookedAsset.h>
#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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
                              std::vector<std::uint8_t>& output) {
        if (!validTexture(content)) return false;
        foundation::BinaryWriter writer;
        writer.writeU32(content.width);
        writer.writeU32(content.height);
        writer.writeU8(static_cast<std::uint8_t>(content.format));
        writer.writeU32(static_cast<std::uint32_t>(content.pixels.size()));
        writer.writeBytes(content.pixels);
        output = writer.take();
        return true;
    }

    static bool decodeTexture(const std::vector<std::uint8_t>& bytes,
                              Texture2DContent& output) {
        foundation::BinaryReader reader(bytes);
        std::uint8_t format = 0;
        std::uint32_t count = 0;
        Texture2DContent candidate;
        if (!reader.readU32(candidate.width) ||
            !reader.readU32(candidate.height) ||
            !reader.readU8(format) || format > 1u ||
            !reader.readU32(count) || count > kMaximumPayloadBytes ||
            !reader.readBytes(count, candidate.pixels,
                              kMaximumPayloadBytes) ||
            !reader.atEnd()) {
            return false;
        }
        candidate.format = static_cast<PixelFormat>(format);
        if (!validTexture(candidate)) return false;
        output = std::move(candidate);
        return true;
    }

    static bool encodeSprite(const SpriteContent& content,
                             std::vector<std::uint8_t>& output) {
        if (!validSprite(content)) return false;
        foundation::BinaryWriter writer;
        writer.writeU64(content.texture.id);
        writer.writeU32(content.x);
        writer.writeU32(content.y);
        writer.writeU32(content.width);
        writer.writeU32(content.height);
        writer.writeI32(content.pivotXMilli);
        writer.writeI32(content.pivotYMilli);
        writer.writeI32(content.worldWidthMilli);
        writer.writeI32(content.worldHeightMilli);
        output = writer.take();
        return true;
    }

    static bool decodeSprite(const std::vector<std::uint8_t>& bytes,
                             SpriteContent& output) {
        foundation::BinaryReader reader(bytes);
        SpriteContent candidate;
        candidate.texture.type = AssetType::Texture2D;
        if (!reader.readU64(candidate.texture.id) ||
            !reader.readU32(candidate.x) ||
            !reader.readU32(candidate.y) ||
            !reader.readU32(candidate.width) ||
            !reader.readU32(candidate.height) ||
            !reader.readI32(candidate.pivotXMilli) ||
            !reader.readI32(candidate.pivotYMilli) ||
            !reader.readI32(candidate.worldWidthMilli) ||
            !reader.readI32(candidate.worldHeightMilli) ||
            !reader.atEnd() || !validSprite(candidate)) {
            return false;
        }
        output = candidate;
        return true;
    }

    static bool encodeAnimation(const AnimationClipContent& content,
                                std::vector<std::uint8_t>& output) {
        if (!validAnimation(content)) return false;
        foundation::BinaryWriter writer;
        writer.writeBool(content.loop);
        writer.writeU32(static_cast<std::uint32_t>(content.frames.size()));
        for (const auto& frame : content.frames) {
            writer.writeU64(frame.spriteId);
            writer.writeU32(frame.durationMilliseconds);
        }
        output = writer.take();
        return true;
    }

    static bool decodeAnimation(const std::vector<std::uint8_t>& bytes,
                                AnimationClipContent& output) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t count = 0;
        AnimationClipContent candidate;
        if (!reader.readBool(candidate.loop) || !reader.readU32(count) ||
            count == 0 || count > kMaximumFrames) {
            return false;
        }
        candidate.frames.resize(count);
        for (auto& frame : candidate.frames) {
            if (!reader.readU64(frame.spriteId) ||
                !reader.readU32(frame.durationMilliseconds) ||
                frame.spriteId == 0 || frame.durationMilliseconds == 0) {
                return false;
            }
        }
        if (!reader.atEnd()) return false;
        output = std::move(candidate);
        return true;
    }

    static bool encodeEffect(const EffectContent& content,
                             std::vector<std::uint8_t>& output) {
        if (content.animationClipId == 0 ||
            content.durationMilliseconds == 0) {
            return false;
        }
        foundation::BinaryWriter writer;
        writer.writeU64(content.animationClipId);
        writer.writeU32(content.durationMilliseconds);
        writer.writeBool(content.additive);
        output = writer.take();
        return true;
    }

    static bool decodeEffect(const std::vector<std::uint8_t>& bytes,
                             EffectContent& output) {
        foundation::BinaryReader reader(bytes);
        EffectContent candidate;
        if (!reader.readU64(candidate.animationClipId) ||
            !reader.readU32(candidate.durationMilliseconds) ||
            !reader.readBool(candidate.additive) || !reader.atEnd() ||
            candidate.animationClipId == 0 ||
            candidate.durationMilliseconds == 0) {
            return false;
        }
        output = candidate;
        return true;
    }

    static bool encodeAudio(const AudioClipContent& content,
                            std::vector<std::uint8_t>& output) {
        if (!validAudio(content)) return false;
        foundation::BinaryWriter writer;
        writer.writeU32(content.sampleRate);
        writer.writeU16(content.channels);
        writer.writeU16(content.bitsPerSample);
        writer.writeU32(static_cast<std::uint32_t>(content.pcm.size()));
        writer.writeBytes(content.pcm);
        output = writer.take();
        return true;
    }

    static bool decodeAudio(const std::vector<std::uint8_t>& bytes,
                            AudioClipContent& output) {
        foundation::BinaryReader reader(bytes);
        std::uint32_t count = 0;
        AudioClipContent candidate;
        if (!reader.readU32(candidate.sampleRate) ||
            !reader.readU16(candidate.channels) ||
            !reader.readU16(candidate.bitsPerSample) ||
            !reader.readU32(count) || count > kMaximumPayloadBytes ||
            !reader.readBytes(count, candidate.pcm,
                              kMaximumPayloadBytes) ||
            !reader.atEnd() || !validAudio(candidate)) {
            return false;
        }
        output = std::move(candidate);
        return true;
    }

    static CookedAsset textureAsset(std::uint64_t id,
                                    const Texture2DContent& content) {
        CookedAsset asset;
        asset.key = {AssetType::Texture2D, id};
        asset.schemaVersion = kTextureSchema;
        if (!encodeTexture(content, asset.payload)) return {};
        return asset;
    }

    static CookedAsset spriteAsset(std::uint64_t id,
                                   const SpriteContent& content) {
        CookedAsset asset;
        asset.key = {AssetType::Sprite, id};
        asset.schemaVersion = kSpriteSchema;
        if (!encodeSprite(content, asset.payload)) return {};
        asset.dependencies.push_back({content.texture, kTextureSchema});
        return asset;
    }

    static CookedAsset animationAsset(
        std::uint64_t id,
        const AnimationClipContent& content) {
        CookedAsset asset;
        asset.key = {AssetType::AnimationClip, id};
        asset.schemaVersion = kAnimationSchema;
        if (!encodeAnimation(content, asset.payload)) return {};
        for (const auto& frame : content.frames) {
            asset.dependencies.push_back(
                {{AssetType::Sprite, frame.spriteId}, kSpriteSchema});
        }
        std::sort(asset.dependencies.begin(), asset.dependencies.end());
        asset.dependencies.erase(
            std::unique(asset.dependencies.begin(), asset.dependencies.end(),
                        [](const AssetDependency& a,
                           const AssetDependency& b) {
                            return a.key == b.key;
                        }),
            asset.dependencies.end());
        return asset;
    }

    static CookedAsset effectAsset(std::uint64_t id,
                                   const EffectContent& content) {
        CookedAsset asset;
        asset.key = {AssetType::Effect, id};
        asset.schemaVersion = kEffectSchema;
        if (!encodeEffect(content, asset.payload)) return {};
        asset.dependencies.push_back(
            {{AssetType::AnimationClip, content.animationClipId},
             kAnimationSchema});
        return asset;
    }

    static CookedAsset audioAsset(std::uint64_t id,
                                  const AudioClipContent& content) {
        CookedAsset asset;
        asset.key = {AssetType::AudioClip, id};
        asset.schemaVersion = kAudioSchema;
        if (!encodeAudio(content, asset.payload)) return {};
        return asset;
    }

private:
    static bool validTexture(const Texture2DContent& content) noexcept {
        if (content.width == 0 || content.height == 0) return false;
        const auto pixels = static_cast<std::uint64_t>(content.width) *
                            static_cast<std::uint64_t>(content.height);
        const auto expected = pixels * PixelBytes(content.format);
        return expected <= kMaximumPayloadBytes &&
               content.pixels.size() == expected;
    }

    static bool validSprite(const SpriteContent& content) noexcept {
        return content.texture.type == AssetType::Texture2D &&
               content.texture.id != 0 && content.width != 0 &&
               content.height != 0 && content.pivotXMilli >= 0 &&
               content.pivotXMilli <= 1000 && content.pivotYMilli >= 0 &&
               content.pivotYMilli <= 1000 &&
               content.worldWidthMilli > 0 && content.worldHeightMilli > 0;
    }

    static bool validAnimation(
        const AnimationClipContent& content) noexcept {
        if (content.frames.empty() ||
            content.frames.size() > kMaximumFrames) {
            return false;
        }
        return std::all_of(
            content.frames.begin(), content.frames.end(),
            [](const AnimationFrameContent& frame) {
                return frame.spriteId != 0 &&
                       frame.durationMilliseconds != 0;
            });
    }

    static bool validAudio(const AudioClipContent& content) noexcept {
        if (content.sampleRate == 0 || content.channels == 0 ||
            content.channels > 8 ||
            (content.bitsPerSample != 8 &&
             content.bitsPerSample != 16 &&
             content.bitsPerSample != 32) ||
            content.pcm.empty() ||
            content.pcm.size() > kMaximumPayloadBytes) {
            return false;
        }
        const auto frameBytes = static_cast<std::size_t>(content.channels) *
                                (content.bitsPerSample / 8u);
        return frameBytes != 0 && content.pcm.size() % frameBytes == 0;
    }
};

} // namespace rts::assets
