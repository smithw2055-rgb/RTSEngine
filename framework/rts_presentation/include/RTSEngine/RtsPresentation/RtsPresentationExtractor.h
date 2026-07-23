#pragma once

#include <RTSEngine/Presentation/PresentationScene.h>
#include <RTSEngine/Rts/SimulationTypes.h>

#include <algorithm>
#include <cstddef>
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
        ExtractionOptions options = {}) {
        presentation::PresentationScene scene;
        scene.tick = snapshot.tick;
        scene.simulationHash = snapshot.worldHash;
        scene.availableResources = snapshot.resources.available;
        scene.observedTeam = options.observedTeam;
        copyVisibility(snapshot, options.observedTeam, scene.visibility);

        scene.entities.reserve(snapshot.entities.size());
        for (const auto& source : snapshot.entities) {
            const auto kind = mapKind(source.kind);
            const auto* binding = catalog.resolve(kind, source.definitionId);
            if (!binding && !options.includeUnboundEntities) continue;

            presentation::PresentationEntity entity;
            entity.viewId = presentation::MakeViewId(
                source.entity.index, source.entity.generation);
            entity.sourceIndex = source.entity.index;
            entity.sourceGeneration = source.entity.generation;
            entity.kind = kind;
            entity.definitionId = source.definitionId;
            entity.teamId = source.teamId;
            entity.x = static_cast<float>(source.x);
            entity.y = static_cast<float>(source.y);
            entity.healthCurrent = source.healthCurrent;
            entity.healthMaximum = source.healthMaximum;
            entity.progressTicks = source.progressTicks;
            entity.requiredTicks = source.requiredTicks;
            entity.moving = source.moving;
            entity.visible = isVisible(source, scene.visibility, options);
            if (binding) {
                entity.spriteAsset = binding->spriteAsset;
                entity.animationAsset = binding->animationAsset;
                entity.layer = binding->layer;
                entity.sortBias = binding->sortBias;
            }
            scene.entities.push_back(entity);
        }

        std::sort(scene.entities.begin(), scene.entities.end(),
                  [](const auto& a, const auto& b) {
                      return a.viewId < b.viewId;
                  });
        return scene;
    }

private:
    static presentation::SceneEntityKind mapKind(
        gameplay::SnapshotKind value) noexcept {
        switch (value) {
        case gameplay::SnapshotKind::Unit:
            return presentation::SceneEntityKind::Unit;
        case gameplay::SnapshotKind::Construction:
            return presentation::SceneEntityKind::Construction;
        case gameplay::SnapshotKind::Building:
            return presentation::SceneEntityKind::Building;
        }
        return presentation::SceneEntityKind::Unit;
    }

    static void copyVisibility(
        const gameplay::WorldSnapshot& snapshot,
        std::uint32_t teamId,
        presentation::VisibilityMask& output) {
        output = {};
        const auto iterator = std::lower_bound(
            snapshot.visibility.begin(), snapshot.visibility.end(), teamId,
            [](const gameplay::TeamVisibilitySnapshot& value,
               std::uint32_t id) {
                return value.teamId < id;
            });
        if (iterator == snapshot.visibility.end() ||
            iterator->teamId != teamId) {
            return;
        }
        const auto width = snapshot.visibilityWidth;
        const auto height = snapshot.visibilityHeight;
        if (width <= 0 || height <= 0) return;
        const auto expected = static_cast<std::size_t>(width) *
                              static_cast<std::size_t>(height);
        if (iterator->current.size() != expected ||
            iterator->explored.size() != expected) {
            return;
        }
        output.width = width;
        output.height = height;
        output.current = iterator->current;
        output.explored = iterator->explored;
    }

    static bool isVisible(
        const gameplay::SnapshotEntity& entity,
        const presentation::VisibilityMask& visibility,
        const ExtractionOptions& options) noexcept {
        if (!options.hideUnseenEnemies || entity.teamId == 0 ||
            entity.teamId == options.observedTeam) {
            return true;
        }
        if (visibility.width <= 0 || visibility.height <= 0 ||
            entity.x < 0 || entity.y < 0 ||
            entity.x >= visibility.width || entity.y >= visibility.height) {
            return false;
        }
        const auto index = static_cast<std::size_t>(entity.y) *
                           static_cast<std::size_t>(visibility.width) +
                           static_cast<std::size_t>(entity.x);
        return index < visibility.current.size() &&
               visibility.current[index] != 0;
    }
};

} // namespace rts::rts_presentation
