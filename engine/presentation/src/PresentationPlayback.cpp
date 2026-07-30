#include <RTSEngine/Presentation/PresentationPlayback.h>

#include <RTSEngine/Assets/CookedContent.h>

#include <algorithm>
#include <limits>

namespace rts::presentation {

PresentationPlaybackRuntime::PresentationPlaybackRuntime(
    assets::AssetManager& assets,
    audio::AudioDevice& audio) noexcept
    : assets_(assets), audio_(audio) {}

void PresentationPlaybackRuntime::consume(
    const PresentationCueBatch& cues,
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

void PresentationPlaybackRuntime::apply(
    RenderPacket& packet,
    std::uint64_t nowMilliseconds) {
    removeMissingAnimations(packet);
    expireEffects(nowMilliseconds);

    for (auto& sprite : packet.sprites) {
        auto iterator = lowerAnimation(sprite.viewId);
        const auto hasOverride = iterator != animations_.end() &&
                                 iterator->viewId == sprite.viewId;
        const auto clipId = hasOverride
            ? iterator->clipId
            : sprite.animationAsset;
        const auto started = hasOverride
            ? iterator->startedMilliseconds
            : 0u;
        if (clipId == 0) continue;
        std::uint64_t frameSprite = 0;
        bool finished = false;
        const auto elapsed = nowMilliseconds >= started
            ? nowMilliseconds - started
            : 0u;
        if (sampleAnimation(
                clipId,
                elapsed,
                frameSprite,
                finished,
                hasOverride ? 0u : sprite.viewId)) {
            sprite.spriteAsset = frameSprite;
            if (hasOverride && finished) {
                completeAnimation(iterator);
            }
        }
    }

    for (const auto& effect : effects_) {
        std::uint64_t frameSprite = 0;
        bool finished = false;
        const auto elapsed = nowMilliseconds >= effect.startedMilliseconds
            ? nowMilliseconds - effect.startedMilliseconds
            : 0u;
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

void PresentationPlaybackRuntime::clear() noexcept {
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
    clipCache_.clear();
    refreshCounts();
}

const PresentationPlaybackStats&
PresentationPlaybackRuntime::stats() const noexcept {
    return stats_;
}

bool PresentationPlaybackRuntime::startAnimation(
    const AnimationCue& cue,
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

bool PresentationPlaybackRuntime::spawnEffect(
    const EffectCue& cue,
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

bool PresentationPlaybackRuntime::sampleAnimation(
    LogicalAssetId clipId,
    std::uint64_t elapsedMilliseconds,
    std::uint64_t& spriteId,
    bool& finished,
    std::uint64_t phaseSeed) {
    const CachedAnimationClip* clip = nullptr;
    if (!resolveAnimationClip(clipId, clip) || !clip ||
        clip->durationMilliseconds == 0 || clip->frames.empty()) {
        return false;
    }
    if (clip->loop && phaseSeed != 0) {
        const auto offset = stablePhase(phaseSeed, clipId) %
                            clip->durationMilliseconds;
        elapsedMilliseconds = elapsedMilliseconds >
                std::numeric_limits<std::uint64_t>::max() - offset
            ? std::numeric_limits<std::uint64_t>::max()
            : elapsedMilliseconds + offset;
    }
    finished = !clip->loop &&
               elapsedMilliseconds >= clip->durationMilliseconds;
    const auto sample = clip->loop
        ? elapsedMilliseconds % clip->durationMilliseconds
        : std::min(
              elapsedMilliseconds, clip->durationMilliseconds - 1u);
    const auto iterator = std::upper_bound(
        clip->frames.begin(), clip->frames.end(), sample,
        [](std::uint64_t value, const CachedAnimationFrame& frame) {
            return value < frame.cumulativeEndMilliseconds;
        });
    spriteId = iterator != clip->frames.end()
        ? iterator->spriteId
        : clip->frames.back().spriteId;
    return true;
}

bool PresentationPlaybackRuntime::resolveAnimationClip(
    LogicalAssetId clipId,
    const CachedAnimationClip*& clip) {
    const auto* loaded = assets_.loaded(
        {assets::AssetType::AnimationClip, clipId});
    if (!loaded) return false;
    auto iterator = lowerClip(clipId);
    if (iterator != clipCache_.end() && iterator->clipId == clipId &&
        iterator->assetGeneration == loaded->generation) {
        ++stats_.animationCacheHits;
        clip = &*iterator;
        return true;
    }

    assets::AnimationClipContent decoded;
    if (!assets::CookedContentCodec::decodeAnimation(
            loaded->cooked.payload, decoded)) {
        return false;
    }
    CachedAnimationClip candidate;
    candidate.clipId = clipId;
    candidate.assetGeneration = loaded->generation;
    candidate.loop = decoded.loop;
    candidate.frames.reserve(decoded.frames.size());
    std::uint64_t duration = 0;
    for (const auto& frame : decoded.frames) {
        duration = frame.durationMilliseconds >
                std::numeric_limits<std::uint64_t>::max() - duration
            ? std::numeric_limits<std::uint64_t>::max()
            : duration + frame.durationMilliseconds;
        candidate.frames.push_back({frame.spriteId, duration});
    }
    if (duration == 0) return false;
    candidate.durationMilliseconds = duration;
    if (iterator != clipCache_.end() && iterator->clipId == clipId) {
        *iterator = std::move(candidate);
    } else {
        iterator = clipCache_.insert(iterator, std::move(candidate));
    }
    ++stats_.animationCacheMisses;
    clip = &*iterator;
    return true;
}

void PresentationPlaybackRuntime::completeAnimation(
    AnimationIterator iterator) noexcept {
    if (iterator == animations_.end()) return;
    (void)assets_.release(
        {assets::AssetType::AnimationClip, iterator->clipId});
    animations_.erase(iterator);
    ++stats_.completedAnimations;
}

void PresentationPlaybackRuntime::expireEffects(
    std::uint64_t nowMilliseconds) noexcept {
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

void PresentationPlaybackRuntime::removeMissingAnimations(
    const RenderPacket& packet) noexcept {
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

PresentationPlaybackRuntime::AnimationIterator
PresentationPlaybackRuntime::lowerAnimation(ViewId viewId) {
    return std::lower_bound(
        animations_.begin(), animations_.end(), viewId,
        [](const AnimationState& value, ViewId lookup) {
            return value.viewId < lookup;
        });
}

PresentationPlaybackRuntime::ClipIterator
PresentationPlaybackRuntime::lowerClip(LogicalAssetId clipId) {
    return std::lower_bound(
        clipCache_.begin(), clipCache_.end(), clipId,
        [](const CachedAnimationClip& value, LogicalAssetId lookup) {
            return value.clipId < lookup;
        });
}

ViewId PresentationPlaybackRuntime::effectViewId(
    PresentationEventId eventId) noexcept {
    const auto value = eventId | (std::uint64_t{1} << 63u);
    return value == 0 ? (std::uint64_t{1} << 63u) : value;
}

std::uint64_t PresentationPlaybackRuntime::stablePhase(
    std::uint64_t seed,
    std::uint64_t clipId) noexcept {
    auto value = seed ^ (clipId + 0x9e3779b97f4a7c15ull +
                         (seed << 6u) + (seed >> 2u));
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebull;
    value ^= value >> 31u;
    return value;
}

void PresentationPlaybackRuntime::refreshCounts() noexcept {
    stats_.activeAnimations =
        static_cast<std::uint32_t>(animations_.size());
    stats_.activeEffects =
        static_cast<std::uint32_t>(effects_.size());
    stats_.residentAnimationClips =
        static_cast<std::uint32_t>(clipCache_.size());
}

} // namespace rts::presentation
