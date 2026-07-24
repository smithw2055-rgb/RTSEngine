#include <RTSEngine/Presentation/PresentationEvents.h>

#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cmath>

namespace rts::presentation {

bool PresentationEventCatalog::upsert(
    PresentationEventBinding binding) {
    if (binding.key.domain == 0 || binding.key.type == 0 ||
        (binding.animationAsset == 0 && binding.effectAsset == 0 &&
         binding.audioAsset == 0) ||
        !std::isfinite(binding.audioVolume) ||
        binding.audioVolume < 0.0f) {
        return false;
    }
    const auto iterator = lower(binding.key);
    if (iterator != bindings_.end() && iterator->key == binding.key) {
        *iterator = binding;
    } else {
        bindings_.insert(iterator, binding);
    }
    return true;
}

const PresentationEventBinding* PresentationEventCatalog::resolve(
    const PresentationEvent& event) const noexcept {
    const EventBindingKey exact{
        event.domain, event.type, event.objectId};
    const auto iterator = lower(exact);
    if (iterator != bindings_.end() && iterator->key == exact) {
        return &*iterator;
    }
    const EventBindingKey wildcard{event.domain, event.type, 0};
    const auto fallback = lower(wildcard);
    return fallback != bindings_.end() && fallback->key == wildcard
        ? &*fallback : nullptr;
}

const std::vector<PresentationEventBinding>&
PresentationEventCatalog::bindings() const noexcept {
    return bindings_;
}

PresentationEventCatalog::Iterator PresentationEventCatalog::lower(
    EventBindingKey key) {
    return std::lower_bound(
        bindings_.begin(), bindings_.end(), key,
        [](const PresentationEventBinding& value,
           EventBindingKey lookup) {
            return value.key < lookup;
        });
}

PresentationEventCatalog::ConstIterator PresentationEventCatalog::lower(
    EventBindingKey key) const {
    return std::lower_bound(
        bindings_.begin(), bindings_.end(), key,
        [](const PresentationEventBinding& value,
           EventBindingKey lookup) {
            return value.key < lookup;
        });
}

PresentationEventConsumer::PresentationEventConsumer(
    std::size_t rememberedEvents)
    : rememberedEvents_(std::max<std::size_t>(1u, rememberedEvents)) {}

PresentationCueBatch PresentationEventConsumer::consume(
    std::vector<PresentationEvent> events,
    const PresentationEventCatalog& catalog) {
    std::stable_sort(
        events.begin(), events.end(),
        [](const PresentationEvent& a, const PresentationEvent& b) {
            return a.tick < b.tick ||
                   (a.tick == b.tick && a.id < b.id);
        });

    PresentationCueBatch output;
    for (const auto& event : events) {
        if (event.id == 0 || event.domain == 0 || event.type == 0 ||
            seen_.find(event.id) != seen_.end()) {
            continue;
        }
        remember(event.id);
        const auto* binding = catalog.resolve(event);
        if (!binding) continue;

        if (binding->animationAsset != 0 && event.sourceView != 0) {
            output.animations.push_back(
                {event.id, event.sourceView,
                 binding->animationAsset,
                 binding->restartAnimation});
        }
        if (binding->effectAsset != 0) {
            output.effects.push_back(
                {event.id, binding->effectAsset, event.x, event.y});
        }
        if (binding->audioAsset != 0) {
            output.audio.push_back(
                {event.id, binding->audioAsset,
                 binding->audioVolume, event.x, event.y});
        }
    }
    return output;
}

bool PresentationEventConsumer::hasSeen(
    PresentationEventId id) const noexcept {
    return seen_.find(id) != seen_.end();
}

std::size_t PresentationEventConsumer::seenCount() const noexcept {
    return seen_.size();
}

void PresentationEventConsumer::clear() {
    order_.clear();
    seen_.clear();
}

void PresentationEventConsumer::remember(PresentationEventId id) {
    seen_.insert(id);
    order_.push_back(id);
    while (order_.size() > rememberedEvents_) {
        seen_.erase(order_.front());
        order_.pop_front();
    }
}

PresentationEventId MakePresentationEventId(
    std::uint32_t domain,
    std::uint64_t tick,
    std::uint32_t ordinal,
    std::uint32_t type,
    ViewId sourceView,
    ViewId targetView,
    std::uint32_t objectId,
    std::int32_t value) noexcept {
    foundation::CanonicalHash hash;
    hash.WriteString("presentation.event");
    hash.WriteU32(domain);
    hash.WriteU64(tick);
    hash.WriteU32(ordinal);
    hash.WriteU32(type);
    hash.WriteU64(sourceView);
    hash.WriteU64(targetView);
    hash.WriteU32(objectId);
    hash.WriteI32(value);
    const auto result = hash.Value();
    return result == 0 ? 1u : result;
}

} // namespace rts::presentation
