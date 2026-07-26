#pragma once

#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Presentation/SpriteBatch.h>

#include <cstdint>
#include <vector>

namespace rts::assets {
struct SpriteContent;
}

namespace rts::presentation {

struct PresentationAssetCacheStats final {
    std::uint32_t residentTextures{};
    std::uint32_t residentSprites{};
    std::uint64_t textureUploads{};
    std::uint64_t textureRebuilds{};
    std::uint64_t resolveFailures{};
};

class PresentationAssetCache final : public SpriteResolver {
private:
    struct TextureResolution final {
        render::TextureHandle handle{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t assetGeneration{};
    };

    struct TextureEntry final {
        assets::AssetKey key{};
        std::uint32_t assetGeneration{};
        render::TextureHandle handle{};
        std::uint32_t width{};
        std::uint32_t height{};
    };

    struct SpriteEntry final {
        LogicalAssetId id{};
        std::uint32_t assetGeneration{};
        std::uint32_t textureGeneration{};
        ResolvedSprite resolved{};
    };

    using TextureIterator = std::vector<TextureEntry>::iterator;
    using SpriteIterator = std::vector<SpriteEntry>::iterator;

public:
    PresentationAssetCache(assets::AssetManager& assets,
                           render::RenderDevice& device) noexcept;

    bool resolve(LogicalAssetId spriteAsset,
                 ResolvedSprite& output) override;
    void clear() noexcept;
    const PresentationAssetCacheStats& stats() const noexcept;

private:
    bool resolveTexture(assets::AssetKey key,
                        TextureResolution& output);
    static bool spriteWithinTexture(const assets::SpriteContent& sprite,
                                    std::uint32_t width,
                                    std::uint32_t height) noexcept;
    static ResolvedSprite makeResolved(
        const assets::SpriteContent& sprite,
        const TextureResolution& texture) noexcept;
    void synchronizeDeviceGeneration() noexcept;
    TextureIterator lowerTexture(assets::AssetKey key);
    SpriteIterator lowerSprite(LogicalAssetId id);
    bool fail() noexcept;
    void refreshCounts() noexcept;

    assets::AssetManager& assets_;
    render::RenderDevice& device_;
    std::vector<TextureEntry> textures_;
    std::vector<SpriteEntry> sprites_;
    std::uint32_t deviceGeneration_{};
    PresentationAssetCacheStats stats_{};
};

} // namespace rts::presentation
