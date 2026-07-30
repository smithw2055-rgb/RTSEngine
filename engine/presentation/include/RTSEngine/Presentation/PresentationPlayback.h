#pragma once

#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Audio/AudioDevice.h>
#include <RTSEngine/Presentation/PresentationEvents.h>
#include <RTSEngine/Presentation/RenderPacket.h>

#include <cstdint>
#include <vector>

namespace rts::presentation {

struct PresentationPlaybackStats final {
    std::uint32_t activeAnimations{};
    std::uint32_t activeEffects{};
    std::uint32_t residentAnimationClips{};
    std::uint64_t animationCues{};
    std::uint64_t effectCues{};
    std::uint64_t audioCues{};
    std::uint64_t droppedCues{};
    std::uint64_t completedAnimations{};
    std::uint64_t animationCacheHits{};
    std::uint64_t animationCacheMisses{};
};

class PresentationPlaybackRuntime final {
public:
    PresentationPlaybackRuntime(assets::AssetManager& assets,
                                audio::AudioDevice& audio) noexcept;

    void consume(const PresentationCueBatch& cues,
                 std::uint64_t nowMilliseconds);
    void apply(RenderPacket& packet,
               std::uint64_t nowMilliseconds);
    void clear() noexcept;
    const PresentationPlaybackStats& stats() const noexcept;

private:
    struct AnimationState final {
        ViewId viewId{};
        LogicalAssetId clipId{};
        PresentationEventId eventId{};
        std::uint64_t startedMilliseconds{};
    };

    struct EffectState final {
        PresentationEventId eventId{};
        LogicalAssetId effectAssetId{};
        LogicalAssetId animationClipId{};
        std::uint64_t startedMilliseconds{};
        std::uint64_t durationMilliseconds{};
        float x{};
        float y{};
        bool additive{};
    };

    struct CachedAnimationFrame final {
        std::uint64_t spriteId{};
        std::uint64_t cumulativeEndMilliseconds{};
    };

    struct CachedAnimationClip final {
        LogicalAssetId clipId{};
        std::uint32_t assetGeneration{};
        bool loop{};
        std::uint64_t durationMilliseconds{};
        std::vector<CachedAnimationFrame> frames;
    };

    using AnimationIterator = std::vector<AnimationState>::iterator;
    using ClipIterator = std::vector<CachedAnimationClip>::iterator;

    bool startAnimation(const AnimationCue& cue,
                        std::uint64_t nowMilliseconds);
    bool spawnEffect(const EffectCue& cue,
                     std::uint64_t nowMilliseconds);
    bool sampleAnimation(LogicalAssetId clipId,
                         std::uint64_t elapsedMilliseconds,
                         std::uint64_t& spriteId,
                         bool& finished,
                         std::uint64_t phaseSeed = 0);
    bool resolveAnimationClip(
        LogicalAssetId clipId,
        const CachedAnimationClip*& clip);
    void completeAnimation(AnimationIterator iterator) noexcept;
    void expireEffects(std::uint64_t nowMilliseconds) noexcept;
    void removeMissingAnimations(const RenderPacket& packet) noexcept;
    AnimationIterator lowerAnimation(ViewId viewId);
    ClipIterator lowerClip(LogicalAssetId clipId);
    static ViewId effectViewId(PresentationEventId eventId) noexcept;
    static std::uint64_t stablePhase(
        std::uint64_t seed,
        std::uint64_t clipId) noexcept;
    void refreshCounts() noexcept;

    assets::AssetManager& assets_;
    audio::AudioDevice& audio_;
    std::vector<AnimationState> animations_;
    std::vector<EffectState> effects_;
    std::vector<CachedAnimationClip> clipCache_;
    PresentationPlaybackStats stats_{};
};

} // namespace rts::presentation
