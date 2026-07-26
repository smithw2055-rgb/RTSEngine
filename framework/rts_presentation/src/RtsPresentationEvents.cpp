#include <RTSEngine/RtsPresentation/RtsPresentationEvents.h>

#include <algorithm>
#include <cstddef>

namespace rts::rts_presentation {

std::vector<presentation::PresentationEvent>
RtsPresentationEventExtractor::extract(
    const std::vector<gameplay::DomainEvent>& events,
    const presentation::PresentationScene& current,
    const presentation::PresentationScene* previous) {
    std::vector<presentation::PresentationEvent> output;
    output.reserve(events.size());
    const auto domain = static_cast<std::uint32_t>(
        presentation::PresentationEventDomain::Rts);

    for (std::size_t ordinal = 0; ordinal < events.size(); ++ordinal) {
        const auto& source = events[ordinal];
        presentation::PresentationEvent event;
        event.tick = source.tick;
        event.domain = domain;
        event.type = RtsEventTypeCode(source.type);
        event.sourceView = source.entity.valid()
            ? presentation::MakeViewId(
                  source.entity.index, source.entity.generation)
            : 0;
        event.targetView = source.secondary.valid()
            ? presentation::MakeViewId(
                  source.secondary.index, source.secondary.generation)
            : 0;
        event.objectId = source.objectId;
        event.value = source.value;
        locate(event.sourceView, event.targetView,
               current, previous, event.x, event.y);
        event.id = presentation::MakePresentationEventId(
            event.domain, event.tick,
            static_cast<std::uint32_t>(ordinal), event.type,
            event.sourceView, event.targetView,
            event.objectId, event.value);
        output.push_back(event);
    }

    std::stable_sort(
        output.begin(), output.end(),
        [](const auto& a, const auto& b) {
            return a.tick < b.tick ||
                   (a.tick == b.tick && a.id < b.id);
        });
    return output;
}

const presentation::PresentationEntity* RtsPresentationEventExtractor::find(
    const presentation::PresentationScene& scene,
    presentation::ViewId viewId) noexcept {
    if (viewId == 0) return nullptr;
    const auto iterator = std::lower_bound(
        scene.entities.begin(), scene.entities.end(), viewId,
        [](const presentation::PresentationEntity& value,
           presentation::ViewId id) {
            return value.viewId < id;
        });
    return iterator != scene.entities.end() && iterator->viewId == viewId
        ? &*iterator : nullptr;
}

void RtsPresentationEventExtractor::locate(
    presentation::ViewId source,
    presentation::ViewId target,
    const presentation::PresentationScene& current,
    const presentation::PresentationScene* previous,
    float& x,
    float& y) noexcept {
    const presentation::PresentationEntity* entity = find(current, source);
    if (!entity) entity = find(current, target);
    if (!entity && previous) entity = find(*previous, source);
    if (!entity && previous) entity = find(*previous, target);
    if (entity) {
        x = entity->x;
        y = entity->y;
    }
}

} // namespace rts::rts_presentation
