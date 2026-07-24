#include <RTSEngine/Assets/CookedContent.h>

#include <rts/foundation/BinaryArchive.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace rts::assets {

bool CookedContentCodec::encodeTexture(
    const Texture2DContent& content,
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

bool CookedContentCodec::decodeTexture(
    const std::vector<std::uint8_t>& bytes,
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

bool CookedContentCodec::encodeSprite(
    const SpriteContent& content,
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

bool CookedContentCodec::decodeSprite(
    const std::vector<std::uint8_t>& bytes,
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

bool CookedContentCodec::encodeAnimation(
    const AnimationClipContent& content,
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

bool CookedContentCodec::decodeAnimation(
    const std::vector<std::uint8_t>& bytes,
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

bool CookedContentCodec::encodeEffect(
    const EffectContent& content,
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

bool CookedContentCodec::decodeEffect(
    const std::vector<std::uint8_t>& bytes,
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

bool CookedContentCodec::encodeAudio(
    const AudioClipContent& content,
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

bool CookedContentCodec::decodeAudio(
    const std::vector<std::uint8_t>& bytes,
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

CookedAsset CookedContentCodec::textureAsset(
    std::uint64_t id,
    const Texture2DContent& content) {
    CookedAsset asset;
    asset.key = {AssetType::Texture2D, id};
    asset.schemaVersion = kTextureSchema;
    if (!encodeTexture(content, asset.payload)) return {};
    return asset;
}

CookedAsset CookedContentCodec::spriteAsset(
    std::uint64_t id,
    const SpriteContent& content) {
    CookedAsset asset;
    asset.key = {AssetType::Sprite, id};
    asset.schemaVersion = kSpriteSchema;
    if (!encodeSprite(content, asset.payload)) return {};
    asset.dependencies.push_back({content.texture, kTextureSchema});
    return asset;
}

CookedAsset CookedContentCodec::animationAsset(
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

CookedAsset CookedContentCodec::effectAsset(
    std::uint64_t id,
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

CookedAsset CookedContentCodec::audioAsset(
    std::uint64_t id,
    const AudioClipContent& content) {
    CookedAsset asset;
    asset.key = {AssetType::AudioClip, id};
    asset.schemaVersion = kAudioSchema;
    if (!encodeAudio(content, asset.payload)) return {};
    return asset;
}

bool CookedContentCodec::validTexture(
    const Texture2DContent& content) noexcept {
    if (content.width == 0 || content.height == 0) return false;
    const auto pixels = static_cast<std::uint64_t>(content.width) *
                        static_cast<std::uint64_t>(content.height);
    const auto expected = pixels * PixelBytes(content.format);
    return expected <= kMaximumPayloadBytes &&
           content.pixels.size() == expected;
}

bool CookedContentCodec::validSprite(
    const SpriteContent& content) noexcept {
    return content.texture.type == AssetType::Texture2D &&
           content.texture.id != 0 && content.width != 0 &&
           content.height != 0 && content.pivotXMilli >= 0 &&
           content.pivotXMilli <= 1000 && content.pivotYMilli >= 0 &&
           content.pivotYMilli <= 1000 &&
           content.worldWidthMilli > 0 && content.worldHeightMilli > 0;
}

bool CookedContentCodec::validAnimation(
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

bool CookedContentCodec::validAudio(
    const AudioClipContent& content) noexcept {
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

} // namespace rts::assets
