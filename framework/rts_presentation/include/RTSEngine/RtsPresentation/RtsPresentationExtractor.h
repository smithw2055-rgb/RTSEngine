#pragma once

#include <RTSEngine/Presentation/PresentationScene.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <cstdint>

namespace rts::rts_presentation {

struct ExtractionOptions final {
    std::uint32_t observedTeam{1};
    bool hideUnseenEnemies{true};
    bool includeUnboundEntities{true};
};

class RtsPresentationExtractor final {
public:
    static presentation::PresentationScene extract(
        const gameplay::WorldSnapshot& snapshot,
        const presentation::VisualCatalog& catalog,
        ExtractionOptions options = {});

private:
    static presentation::SceneEntityKind mapKind(
        gameplay::SnapshotKind value) noexcept;
    static void copyVisibility(
        const gameplay::WorldSnapshot& snapshot,
        std::uint32_t teamId,
        presentation::VisibilityMask& output);
    static bool isVisible(
        const gameplay::SnapshotEntity& entity,
        const presentation::VisibilityMask& visibility,
        const ExtractionOptions& options) noexcept;
};

} // namespace rts::rts_presentation
