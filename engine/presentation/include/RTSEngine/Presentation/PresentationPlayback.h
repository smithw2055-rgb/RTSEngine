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
    std::uint64_t animationCues{};
    std::uint64_t effectCues{};
    std::uint64_t audioCues{};
    std::uint64_t droppedCues{};
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

    bool startAnimation(const AnimationCue& cue,
                        std::uint64_t nowMilliseconds);
    bool spawnEffect(const EffectCue& cue,
                     std::uint64_t nowMilliseconds);
    bool sampleAnimation(LogicalAssetId clipId,
                         std::uint64_t elapsedMilliseconds,
                         std::uint64_t& spriteId,
                         bool& finished);
    void expireEffects(std::uint64_t nowMilliseconds) noexcept;
    void removeMissingAnimations(const RenderPacket& packet) noexcept;
    std::vector<AnimationState>::iterator lowerAnimation(ViewId viewId);
    static ViewId effectViewId(PresentationEventId eventId) noexcept;
    void refreshCounts() noexcept;

    assets::AssetManager& assets_;
    audio::AudioDevice& audio_;
    std::vector<AnimationState> animations_;
    std::vector<EffectState> effects_;
    PresentationPlaybackStats stats_{};
};

} // namespace rts::presentation
