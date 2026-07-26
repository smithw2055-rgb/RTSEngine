#pragma once

#include <RTSEngine/Presentation/PresentationEvents.h>
#include <RTSEngine/Presentation/RenderPacket.h>
#include <RTSEngine/RtsPresentation/RtsPresentationExtractor.h>
#include <RTSEngine/RtsPresentation/RtsPresentationEvents.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rts::rts_presentation {

class RtsPresentationRuntime final {
public:
    explicit RtsPresentationRuntime(ExtractionOptions options = {}) noexcept;

    bool registerVisual(presentation::VisualBinding binding);
    bool registerFallbackVisual(
        presentation::SceneEntityKind kind,
        presentation::LogicalAssetId spriteAsset,
        presentation::LogicalAssetId animationAsset = 0,
        presentation::RenderLayer layer = presentation::RenderLayer::WorldEntity,
        std::int32_t sortBias = 0);
    bool registerEventBinding(
        presentation::PresentationEventBinding binding);

    bool publishSnapshot(const gameplay::WorldSnapshot& snapshot);
    bool publishSnapshot(
        const gameplay::WorldSnapshot& snapshot,
        const std::vector<gameplay::DomainEvent>& events);

    presentation::RenderPacket buildRenderPacket(
        float alpha,
        presentation::InterpolationOptions options = {}) const;
    presentation::PresentationCueBatch consumeCues();

    const presentation::VisualCatalog& catalog() const noexcept;
    const presentation::PresentationEventCatalog& eventCatalog() const noexcept;
    const presentation::PresentationSceneBuffer& sceneBuffer() const noexcept;
    const ExtractionOptions& extractionOptions() const noexcept;
    void setExtractionOptions(ExtractionOptions options) noexcept;
    std::size_t pendingEventCount() const noexcept;
    void clearEventHistory();
    void reset() noexcept;

private:
    ExtractionOptions options_{};
    presentation::VisualCatalog catalog_;
    presentation::PresentationSceneBuffer scenes_;
    presentation::PresentationEventCatalog eventCatalog_;
    presentation::PresentationEventConsumer eventConsumer_;
    std::vector<presentation::PresentationEvent> pendingEvents_;
};

} // namespace rts::rts_presentation
