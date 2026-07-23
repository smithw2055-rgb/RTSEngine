#pragma once

#include <RTSEngine/Presentation/RenderPacket.h>
#include <RTSEngine/Presentation/PresentationEvents.h>
#include <RTSEngine/RtsPresentation/RtsPresentationEvents.h>
#include <RTSEngine/RtsPresentation/RtsPresentationExtractor.h>

#include <utility>
#include <vector>

namespace rts::rts_presentation {

class RtsPresentationRuntime final {
public:
    explicit RtsPresentationRuntime(ExtractionOptions options = {}) noexcept
        : options_(options) {}

    bool registerVisual(presentation::VisualBinding binding) {
        return catalog_.upsert(std::move(binding));
    }

    bool registerEventBinding(
        presentation::PresentationEventBinding binding) {
        return eventCatalog_.upsert(std::move(binding));
    }

    bool publishSnapshot(const gameplay::WorldSnapshot& snapshot) {
        return publishSnapshot(snapshot, {});
    }

    bool publishSnapshot(
        const gameplay::WorldSnapshot& snapshot,
        const std::vector<gameplay::DomainEvent>& events) {
        auto scene = RtsPresentationExtractor::extract(
            snapshot, catalog_, options_);
        const auto* previous = scenes_.ready() ? &scenes_.current() : nullptr;
        auto extractedEvents = RtsPresentationEventExtractor::extract(
            events, scene, previous);
        if (!scenes_.publish(std::move(scene))) return false;
        pendingEvents_.insert(
            pendingEvents_.end(),
            extractedEvents.begin(), extractedEvents.end());
        return true;
    }

    presentation::RenderPacket buildRenderPacket(
        float alpha,
        presentation::InterpolationOptions options = {}) const {
        return presentation::RenderPacketBuilder::build(
            scenes_.sample(alpha, options));
    }

    presentation::PresentationCueBatch consumeCues() {
        auto pending = std::move(pendingEvents_);
        pendingEvents_.clear();
        return eventConsumer_.consume(std::move(pending), eventCatalog_);
    }

    const presentation::VisualCatalog& catalog() const noexcept {
        return catalog_;
    }

    const presentation::PresentationEventCatalog& eventCatalog() const noexcept {
        return eventCatalog_;
    }

    const presentation::PresentationSceneBuffer& sceneBuffer() const noexcept {
        return scenes_;
    }

    const ExtractionOptions& extractionOptions() const noexcept {
        return options_;
    }

    void setExtractionOptions(ExtractionOptions options) noexcept {
        options_ = options;
    }

    std::size_t pendingEventCount() const noexcept {
        return pendingEvents_.size();
    }

    void clearEventHistory() {
        pendingEvents_.clear();
        eventConsumer_.clear();
    }

private:
    ExtractionOptions options_{};
    presentation::VisualCatalog catalog_;
    presentation::PresentationSceneBuffer scenes_;
    presentation::PresentationEventCatalog eventCatalog_;
    presentation::PresentationEventConsumer eventConsumer_;
    std::vector<presentation::PresentationEvent> pendingEvents_;
};

} // namespace rts::rts_presentation
