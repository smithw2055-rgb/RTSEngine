#include <RTSEngine/Presentation/PresentationAssetCache.h>

#include <RTSEngine/Assets/CookedContent.h>

#include <algorithm>

namespace rts::presentation {

PresentationAssetCache::PresentationAssetCache(
    assets::AssetManager& assets,
    render::RenderDevice& device) noexcept
    : assets_(assets), device_(device),
      deviceGeneration_(device.deviceGeneration()) {}

bool PresentationAssetCache::resolve(
    LogicalAssetId spriteAsset,
    ResolvedSprite& output) {
    synchronizeDeviceGeneration();
    if (spriteAsset == 0) return fail();
    const assets::AssetKey spriteKey{
        assets::AssetType::Sprite, spriteAsset};
    const auto* loadedSprite = assets_.loaded(spriteKey);
    if (!loadedSprite) return fail();

    assets::SpriteContent sprite;
    if (!assets::CookedContentCodec::decodeSprite(
            loadedSprite->cooked.payload, sprite)) {
        return fail();
    }

    TextureResolution texture;
    if (!resolveTexture(sprite.texture, texture) ||
        !spriteWithinTexture(sprite, texture.width, texture.height)) {
        return fail();
    }

    const auto iterator = lowerSprite(spriteAsset);
    if (iterator == sprites_.end() || iterator->id != spriteAsset) {
        if (!assets_.retain(spriteKey)) return fail();
        SpriteEntry entry;
        entry.id = spriteAsset;
        entry.assetGeneration = loadedSprite->generation;
        entry.textureGeneration = texture.assetGeneration;
        entry.resolved = makeResolved(sprite, texture);
        sprites_.insert(iterator, entry);
        output = entry.resolved;
    } else {
        if (iterator->assetGeneration != loadedSprite->generation ||
            iterator->textureGeneration != texture.assetGeneration ||
            iterator->resolved.texture != texture.handle) {
            iterator->assetGeneration = loadedSprite->generation;
            iterator->textureGeneration = texture.assetGeneration;
            iterator->resolved = makeResolved(sprite, texture);
        }
        output = iterator->resolved;
    }
    refreshCounts();
    return output.valid();
}

void PresentationAssetCache::clear() noexcept {
    const bool sameDevice =
        deviceGeneration_ == device_.deviceGeneration();
    for (const auto& texture : textures_) {
        if (sameDevice && texture.handle.valid()) {
            (void)device_.destroyTexture(texture.handle);
        }
        (void)assets_.release(texture.key);
    }
    for (const auto& sprite : sprites_) {
        (void)assets_.release(
            {assets::AssetType::Sprite, sprite.id});
    }
    textures_.clear();
    sprites_.clear();
    deviceGeneration_ = device_.deviceGeneration();
    refreshCounts();
}

const PresentationAssetCacheStats&
PresentationAssetCache::stats() const noexcept {
    return stats_;
}

bool PresentationAssetCache::resolveTexture(
    assets::AssetKey key,
    TextureResolution& output) {
    if (key.type != assets::AssetType::Texture2D || !key.valid()) {
        return fail();
    }
    const auto* loadedTexture = assets_.loaded(key);
    if (!loadedTexture) return fail();

    auto iterator = lowerTexture(key);
    if (iterator != textures_.end() && iterator->key == key &&
        iterator->assetGeneration == loadedTexture->generation &&
        iterator->handle.valid()) {
        output = {iterator->handle, iterator->width, iterator->height,
                  iterator->assetGeneration};
        return true;
    }

    assets::Texture2DContent texture;
    if (!assets::CookedContentCodec::decodeTexture(
            loadedTexture->cooked.payload, texture)) {
        return fail();
    }
    const auto format = texture.format == assets::PixelFormat::R8
        ? render::TextureFormat::R8 : render::TextureFormat::Rgba8;
    const auto handle = device_.createTexture(
        {texture.width, texture.height, format,
         false, render::FilterMode::Nearest,
         render::AddressMode::Clamp,
         render::AddressMode::Clamp});
    if (!handle.valid() ||
        !device_.updateTexture(
            handle, texture.pixels.data(), texture.pixels.size())) {
        if (handle.valid()) (void)device_.destroyTexture(handle);
        return fail();
    }

    if (iterator == textures_.end() || iterator->key != key) {
        if (!assets_.retain(key)) {
            (void)device_.destroyTexture(handle);
            return fail();
        }
        TextureEntry entry;
        entry.key = key;
        entry.assetGeneration = loadedTexture->generation;
        entry.handle = handle;
        entry.width = texture.width;
        entry.height = texture.height;
        iterator = textures_.insert(iterator, entry);
    } else {
        if (iterator->handle.valid()) {
            (void)device_.destroyTexture(iterator->handle);
            ++stats_.textureRebuilds;
        }
        iterator->assetGeneration = loadedTexture->generation;
        iterator->handle = handle;
        iterator->width = texture.width;
        iterator->height = texture.height;
    }
    ++stats_.textureUploads;
    output = {iterator->handle, iterator->width, iterator->height,
              iterator->assetGeneration};
    refreshCounts();
    return true;
}

bool PresentationAssetCache::spriteWithinTexture(
    const assets::SpriteContent& sprite,
    std::uint32_t width,
    std::uint32_t height) noexcept {
    return static_cast<std::uint64_t>(sprite.x) + sprite.width <= width &&
           static_cast<std::uint64_t>(sprite.y) + sprite.height <= height;
}

ResolvedSprite PresentationAssetCache::makeResolved(
    const assets::SpriteContent& sprite,
    const TextureResolution& texture) noexcept {
    ResolvedSprite output;
    output.texture = texture.handle;
    output.u0 = static_cast<float>(sprite.x) /
                static_cast<float>(texture.width);
    output.v0 = static_cast<float>(sprite.y) /
                static_cast<float>(texture.height);
    output.u1 = static_cast<float>(sprite.x + sprite.width) /
                static_cast<float>(texture.width);
    output.v1 = static_cast<float>(sprite.y + sprite.height) /
                static_cast<float>(texture.height);
    output.width = static_cast<float>(sprite.worldWidthMilli) / 1000.0f;
    output.height = static_cast<float>(sprite.worldHeightMilli) / 1000.0f;
    output.pivotX = static_cast<float>(sprite.pivotXMilli) / 1000.0f;
    output.pivotY = static_cast<float>(sprite.pivotYMilli) / 1000.0f;
    output.blend = render::BlendMode::Alpha;
    return output;
}

void PresentationAssetCache::synchronizeDeviceGeneration() noexcept {
    const auto generation = device_.deviceGeneration();
    if (generation == deviceGeneration_) return;
    for (auto& texture : textures_) texture.handle = {};
    for (auto& sprite : sprites_) sprite.resolved.texture = {};
    deviceGeneration_ = generation;
}

PresentationAssetCache::TextureIterator
PresentationAssetCache::lowerTexture(assets::AssetKey key) {
    return std::lower_bound(
        textures_.begin(), textures_.end(), key,
        [](const TextureEntry& value, assets::AssetKey lookup) {
            return value.key < lookup;
        });
}

PresentationAssetCache::SpriteIterator
PresentationAssetCache::lowerSprite(LogicalAssetId id) {
    return std::lower_bound(
        sprites_.begin(), sprites_.end(), id,
        [](const SpriteEntry& value, LogicalAssetId lookup) {
            return value.id < lookup;
        });
}

bool PresentationAssetCache::fail() noexcept {
    ++stats_.resolveFailures;
    return false;
}

void PresentationAssetCache::refreshCounts() noexcept {
    stats_.residentTextures = static_cast<std::uint32_t>(textures_.size());
    stats_.residentSprites = static_cast<std::uint32_t>(sprites_.size());
}

} // namespace rts::presentation
