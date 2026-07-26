#include <RTSEngine/RtsPresentation/RtsPresentationRuntime.h>
#include <RTSEngine/RtsPresentation/RtsPresentationEvents.h>

#include <utility>

namespace rts::rts_presentation {

RtsPresentationRuntime::RtsPresentationRuntime(
    ExtractionOptions options) noexcept : options_(options) {}

bool RtsPresentationRuntime::registerVisual(
    presentation::VisualBinding binding) {
    return catalog_.upsert(std::move(binding));
}

bool RtsPresentationRuntime::registerFallbackVisual(
    presentation::SceneEntityKind kind,
    presentation::LogicalAssetId spriteAsset,
    presentation::LogicalAssetId animationAsset,
    presentation::RenderLayer layer,
    std::int32_t sortBias) {
    return catalog_.setFallback(
        kind, spriteAsset, animationAsset, layer, sortBias);
}

bool RtsPresentationRuntime::registerEventBinding(
    presentation::PresentationEventBinding binding) {
    return eventCatalog_.upsert(std::move(binding));
}

bool RtsPresentationRuntime::publishSnapshot(
    const gameplay::WorldSnapshot& snapshot) {
    return publishSnapshot(snapshot, {});
}

bool RtsPresentationRuntime::publishSnapshot(
    const gameplay::WorldSnapshot& snapshot,
    const std::vector<gameplay::DomainEvent>& events) {
    auto scene = RtsPresentationExtractor::extract(snapshot, catalog_, options_);
    const auto* previous = scenes_.ready() ? &scenes_.current() : nullptr;
    auto extractedEvents = RtsPresentationEventExtractor::extract(
        events, scene, previous);
    if (!scenes_.publish(std::move(scene))) return false;
    pendingEvents_.insert(pendingEvents_.end(),
                          extractedEvents.begin(), extractedEvents.end());
    return true;
}

presentation::RenderPacket RtsPresentationRuntime::buildRenderPacket(
    float alpha,
    presentation::InterpolationOptions options) const {
    return presentation::RenderPacketBuilder::build(
        scenes_.sample(alpha, options));
}

presentation::PresentationCueBatch RtsPresentationRuntime::consumeCues() {
    auto pending = std::move(pendingEvents_);
    pendingEvents_.clear();
    return eventConsumer_.consume(std::move(pending), eventCatalog_);
}

const presentation::VisualCatalog& RtsPresentationRuntime::catalog() const noexcept {
    return catalog_;
}

const presentation::PresentationEventCatalog&
RtsPresentationRuntime::eventCatalog() const noexcept {
    return eventCatalog_;
}

const presentation::PresentationSceneBuffer&
RtsPresentationRuntime::sceneBuffer() const noexcept {
    return scenes_;
}

const ExtractionOptions& RtsPresentationRuntime::extractionOptions() const noexcept {
    return options_;
}

void RtsPresentationRuntime::setExtractionOptions(
    ExtractionOptions options) noexcept {
    options_ = options;
}

std::size_t RtsPresentationRuntime::pendingEventCount() const noexcept {
    return pendingEvents_.size();
}

void RtsPresentationRuntime::clearEventHistory() {
    pendingEvents_.clear();
    eventConsumer_.clear();
}

} // namespace rts::rts_presentation

namespace rts::rts_presentation {
void RtsPresentationRuntime::reset() noexcept {
    scenes_.clear();
    pendingEvents_.clear();
    eventConsumer_.clear();
}
} // namespace rts::rts_presentation
