#pragma once

#include <RTSEngine/Presentation/PresentationScene.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <vector>

namespace rts::presentation {

using PresentationEventId = std::uint64_t;

enum class PresentationEventDomain : std::uint32_t {
    Rts = 1u,
    TowerDefense = 2u,
    Roguelite = 3u,
    Application = 100u
};

struct PresentationEvent final {
    PresentationEventId id{};
    std::uint64_t tick{};
    std::uint32_t domain{};
    std::uint32_t type{};
    ViewId sourceView{};
    ViewId targetView{};
    std::uint32_t objectId{};
    std::int32_t value{};
    float x{};
    float y{};
};

struct EventBindingKey final {
    std::uint32_t domain{};
    std::uint32_t type{};
    std::uint32_t objectId{};

    friend constexpr bool operator==(EventBindingKey a,
                                     EventBindingKey b) noexcept {
        return a.domain == b.domain && a.type == b.type &&
               a.objectId == b.objectId;
    }

    friend constexpr bool operator<(EventBindingKey a,
                                    EventBindingKey b) noexcept {
        return a.domain < b.domain ||
               (a.domain == b.domain &&
                (a.type < b.type ||
                 (a.type == b.type && a.objectId < b.objectId)));
    }
};

struct PresentationEventBinding final {
    EventBindingKey key{};
    LogicalAssetId animationAsset{};
    LogicalAssetId effectAsset{};
    LogicalAssetId audioAsset{};
    float audioVolume{1.0f};
    bool restartAnimation{true};
};

class PresentationEventCatalog final {
private:
    using Iterator = std::vector<PresentationEventBinding>::iterator;
    using ConstIterator =
        std::vector<PresentationEventBinding>::const_iterator;

public:
    bool upsert(PresentationEventBinding binding);
    const PresentationEventBinding* resolve(
        const PresentationEvent& event) const noexcept;
    const std::vector<PresentationEventBinding>& bindings() const noexcept;

private:
    Iterator lower(EventBindingKey key);
    ConstIterator lower(EventBindingKey key) const;

    std::vector<PresentationEventBinding> bindings_;
};

struct AnimationCue final {
    PresentationEventId eventId{};
    ViewId viewId{};
    LogicalAssetId animationAsset{};
    bool restart{true};
};

struct EffectCue final {
    PresentationEventId eventId{};
    LogicalAssetId effectAsset{};
    float x{};
    float y{};
};

struct AudioCue final {
    PresentationEventId eventId{};
    LogicalAssetId audioAsset{};
    float volume{1.0f};
    float x{};
    float y{};
};

struct PresentationCueBatch final {
    std::vector<AnimationCue> animations;
    std::vector<EffectCue> effects;
    std::vector<AudioCue> audio;
};

class PresentationEventConsumer final {
public:
    explicit PresentationEventConsumer(
        std::size_t rememberedEvents = 4096u);

    PresentationCueBatch consume(
        std::vector<PresentationEvent> events,
        const PresentationEventCatalog& catalog);
    bool hasSeen(PresentationEventId id) const noexcept;
    std::size_t seenCount() const noexcept;
    void clear();

private:
    void remember(PresentationEventId id);

    std::size_t rememberedEvents_;
    std::deque<PresentationEventId> order_;
    std::set<PresentationEventId> seen_;
};

PresentationEventId MakePresentationEventId(
    std::uint32_t domain,
    std::uint64_t tick,
    std::uint32_t ordinal,
    std::uint32_t type,
    ViewId sourceView,
    ViewId targetView,
    std::uint32_t objectId,
    std::int32_t value) noexcept;

} // namespace rts::presentation
