#pragma once

#include <RTSEngine/Assets/AssetManager.h>
#include <RTSEngine/Assets/CookedContent.h>
#include <RTSEngine/Audio/AudioDevice.h>
#include <RTSEngine/Presentation/PresentationEvents.h>
#include <RTSEngine/Presentation/RenderPacket.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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
                                audio::AudioDevice& audio) noexcept
        : assets_(assets), audio_(audio) {}

    void consume(const PresentationCueBatch& cues,
                 std::uint64_t nowMilliseconds) {
        for (const auto& cue : cues.animations) {
            if (!startAnimation(cue, nowMilliseconds)) {
                ++stats_.droppedCues;
            } else {
                ++stats_.animationCues;
            }
        }
        for (const auto& cue : cues.effects) {
            if (!spawnEffect(cue, nowMilliseconds)) {
                ++stats_.droppedCues;
            } else {
                ++stats_.effectCues;
            }
        }
        for (const auto& cue : cues.audio) {
            const assets::AssetKey key{
                assets::AssetType::AudioClip, cue.audioAsset};
            if (!assets_.loaded(key)) {
                ++stats_.droppedCues;
                continue;
            }
            audio::AudioPlayCommand command;
            command.eventId = cue.eventId;
            command.clipId = cue.audioAsset;
            command.volume = cue.volume;
            command.x = cue.x;
            command.y = cue.y;
            command.spatial = true;
            if (!audio_.play(command).valid()) {
                ++stats_.droppedCues;
            } else {
                ++stats_.audioCues;
            }
        }
        refreshCounts();
    }

    void apply(RenderPacket& packet, std::uint64_t nowMilliseconds) {
        removeMissingAnimations(packet);
        expireEffects(nowMilliseconds);

        for (auto& sprite : packet.sprites) {
            const auto iterator = lowerAnimation(sprite.viewId);
            const auto clipId = iterator != animations_.end() &&
                                        iterator->viewId == sprite.viewId
                ? iterator->clipId : sprite.animationAsset;
            const auto start = iterator != animations_.end() &&
                                       iterator->viewId == sprite.viewId
                ? iterator->startedMilliseconds : 0u;
            if (clipId == 0) continue;
            std::uint64_t frameSprite = 0;
            bool finished = false;
            if (sampleAnimation(
                    clipId,
                    nowMilliseconds >= start ? nowMilliseconds - start : 0u,
                    frameSprite, finished)) {
                sprite.spriteAsset = frameSprite;
            }
        }

        for (const auto& effect : effects_) {
            std::uint64_t frameSprite = 0;
            bool finished = false;
            const auto elapsed = nowMilliseconds >= effect.startedMilliseconds
                ? nowMilliseconds - effect.startedMilliseconds : 0u;
            if (!sampleAnimation(
                    effect.animationClipId, elapsed,
                    frameSprite, finished)) {
                continue;
            }
            const auto remaining = effect.durationMilliseconds > elapsed
                ? effect.durationMilliseconds - elapsed : 0u;
            const auto fadeWindow = std::max<std::uint64_t>(
                1u, effect.durationMilliseconds / 4u);
            const auto opacity = remaining >= fadeWindow
                ? 1.0f
                : static_cast<float>(remaining) /
                  static_cast<float>(fadeWindow);
            SpriteInstance sprite;
            sprite.viewId = effectViewId(effect.eventId);
            sprite.spriteAsset = frameSprite;
            sprite.animationAsset = effect.animationClipId;
            sprite.layer = RenderLayer::ProjectileAndEffect;
            sprite.x = effect.x;
            sprite.y = effect.y;
            sprite.opacity = std::clamp(opacity, 0.0f, 1.0f);
            sprite.lifecycle = ViewLifecycle::Stable;
            sprite.blend = effect.additive
                ? render::BlendMode::Additive
                : render::BlendMode::Alpha;
            packet.sprites.push_back(sprite);
        }
        RenderPacketBuilder::sort(packet);
        refreshCounts();
    }

    void clear() noexcept {
        for (const auto& animation : animations_) {
            (void)assets_.release(
                {assets::AssetType::AnimationClip, animation.clipId});
        }
        for (const auto& effect : effects_) {
            (void)assets_.release(
                {assets::AssetType::Effect, effect.effectAssetId});
        }
        animations_.clear();
        effects_.clear();
        refreshCounts();
    }

    const PresentationPlaybackStats& stats() const noexcept {
        return stats_;
    }

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
                        std::uint64_t nowMilliseconds) {
        if (cue.viewId == 0 || cue.animationAsset == 0 ||
            !assets_.loaded(
                {assets::AssetType::AnimationClip,
                 cue.animationAsset})) {
            return false;
        }
        auto iterator = lowerAnimation(cue.viewId);
        if (iterator != animations_.end() &&
            iterator->viewId == cue.viewId) {
            if (!cue.restart && iterator->clipId == cue.animationAsset) {
                return true;
            }
            if (iterator->clipId != cue.animationAsset) {
                if (!assets_.retain(
                        {assets::AssetType::AnimationClip,
                         cue.animationAsset})) {
                    return false;
                }
                (void)assets_.release(
                    {assets::AssetType::AnimationClip, iterator->clipId});
                iterator->clipId = cue.animationAsset;
            }
            iterator->eventId = cue.eventId;
            iterator->startedMilliseconds = nowMilliseconds;
            return true;
        }
        if (!assets_.retain(
                {assets::AssetType::AnimationClip,
                 cue.animationAsset})) {
            return false;
        }
        animations_.insert(
            iterator,
            {cue.viewId, cue.animationAsset,
             cue.eventId, nowMilliseconds});
        return true;
    }

    bool spawnEffect(const EffectCue& cue,
                     std::uint64_t nowMilliseconds) {
        if (cue.eventId == 0 || cue.effectAsset == 0 ||
            std::any_of(effects_.begin(), effects_.end(),
                        [&](const EffectState& value) {
                            return value.eventId == cue.eventId;
                        })) {
            return false;
        }
        const assets::AssetKey key{
            assets::AssetType::Effect, cue.effectAsset};
        const auto* loaded = assets_.loaded(key);
        if (!loaded) return false;
        assets::EffectContent content;
        if (!assets::CookedContentCodec::decodeEffect(
                loaded->cooked.payload, content) ||
            !assets_.retain(key)) {
            return false;
        }
        effects_.push_back(
            {cue.eventId, cue.effectAsset,
             content.animationClipId, nowMilliseconds,
             content.durationMilliseconds,
             cue.x, cue.y, content.additive});
        std::sort(effects_.begin(), effects_.end(),
                  [](const EffectState& a, const EffectState& b) {
                      return a.eventId < b.eventId;
                  });
        return true;
    }

    bool sampleAnimation(LogicalAssetId clipId,
                         std::uint64_t elapsedMilliseconds,
                         std::uint64_t& spriteId,
                         bool& finished) {
        const auto* loaded = assets_.loaded(
            {assets::AssetType::AnimationClip, clipId});
        if (!loaded) return false;
        assets::AnimationClipContent clip;
        if (!assets::CookedContentCodec::decodeAnimation(
                loaded->cooked.payload, clip)) {
            return false;
        }

        std::uint64_t duration = 0;
        for (const auto& frame : clip.frames) {
            duration = frame.durationMilliseconds >
                    std::numeric_limits<std::uint64_t>::max() - duration
                ? std::numeric_limits<std::uint64_t>::max()
                : duration + frame.durationMilliseconds;
        }
        if (duration == 0) return false;
        finished = !clip.loop && elapsedMilliseconds >= duration;
        auto sample = clip.loop
            ? elapsedMilliseconds % duration
            : std::min(elapsedMilliseconds, duration - 1u);
        for (const auto& frame : clip.frames) {
            if (sample < frame.durationMilliseconds) {
                spriteId = frame.spriteId;
                return true;
            }
            sample -= frame.durationMilliseconds;
        }
        spriteId = clip.frames.back().spriteId;
        return true;
    }

    void expireEffects(std::uint64_t nowMilliseconds) noexcept {
        auto iterator = effects_.begin();
        while (iterator != effects_.end()) {
            const auto elapsed = nowMilliseconds >= iterator->startedMilliseconds
                ? nowMilliseconds - iterator->startedMilliseconds : 0u;
            if (elapsed < iterator->durationMilliseconds) {
                ++iterator;
                continue;
            }
            (void)assets_.release(
                {assets::AssetType::Effect,
                 iterator->effectAssetId});
            iterator = effects_.erase(iterator);
        }
    }

    void removeMissingAnimations(const RenderPacket& packet) noexcept {
        auto iterator = animations_.begin();
        while (iterator != animations_.end()) {
            const bool present = std::any_of(
                packet.sprites.begin(), packet.sprites.end(),
                [&](const SpriteInstance& sprite) {
                    return sprite.viewId == iterator->viewId;
                });
            if (present) {
                ++iterator;
                continue;
            }
            (void)assets_.release(
                {assets::AssetType::AnimationClip,
                 iterator->clipId});
            iterator = animations_.erase(iterator);
        }
    }

    std::vector<AnimationState>::iterator lowerAnimation(ViewId viewId) {
        return std::lower_bound(
            animations_.begin(), animations_.end(), viewId,
            [](const AnimationState& value, ViewId lookup) {
                return value.viewId < lookup;
            });
    }

    static ViewId effectViewId(PresentationEventId eventId) noexcept {
        const auto value = eventId | (std::uint64_t{1} << 63u);
        return value == 0 ? (std::uint64_t{1} << 63u) : value;
    }

    void refreshCounts() noexcept {
        stats_.activeAnimations =
            static_cast<std::uint32_t>(animations_.size());
        stats_.activeEffects =
            static_cast<std::uint32_t>(effects_.size());
    }

    assets::AssetManager& assets_;
    audio::AudioDevice& audio_;
    std::vector<AnimationState> animations_;
    std::vector<EffectState> effects_;
    PresentationPlaybackStats stats_{};
};

} // namespace rts::presentation
