#pragma once

#include <RTSEngine/Presentation/RenderPacket.h>
#include <RTSEngine/RtsPresentation/RtsPresentationExtractor.h>

#include <utility>

namespace rts::rts_presentation {

class RtsPresentationRuntime final {
public:
    explicit RtsPresentationRuntime(ExtractionOptions options = {}) noexcept
        : options_(options) {}

    bool registerVisual(presentation::VisualBinding binding) {
        return catalog_.upsert(std::move(binding));
    }

    bool publishSnapshot(const gameplay::WorldSnapshot& snapshot) {
        return scenes_.publish(
            RtsPresentationExtractor::extract(snapshot, catalog_, options_));
    }

    presentation::RenderPacket buildRenderPacket(
        float alpha,
        presentation::InterpolationOptions options = {}) const {
        return presentation::RenderPacketBuilder::build(
            scenes_.sample(alpha, options));
    }

    const presentation::VisualCatalog& catalog() const noexcept {
        return catalog_;
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

private:
    ExtractionOptions options_{};
    presentation::VisualCatalog catalog_;
    presentation::PresentationSceneBuffer scenes_;
};

} // namespace rts::rts_presentation
