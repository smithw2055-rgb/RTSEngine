#pragma once

#include <RTSEngine/Presentation/PresentationEvents.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <cstdint>
#include <vector>

namespace rts::rts_presentation {

constexpr std::uint32_t RtsEventTypeCode(
    gameplay::DomainEventType type) noexcept {
    return static_cast<std::uint32_t>(type) + 1u;
}

class RtsPresentationEventExtractor final {
public:
    static std::vector<presentation::PresentationEvent> extract(
        const std::vector<gameplay::DomainEvent>& events,
        const presentation::PresentationScene& current,
        const presentation::PresentationScene* previous = nullptr);

private:
    static const presentation::PresentationEntity* find(
        const presentation::PresentationScene& scene,
        presentation::ViewId viewId) noexcept;
    static void locate(
        presentation::ViewId source,
        presentation::ViewId target,
        const presentation::PresentationScene& current,
        const presentation::PresentationScene* previous,
        float& x,
        float& y) noexcept;
};

} // namespace rts::rts_presentation
