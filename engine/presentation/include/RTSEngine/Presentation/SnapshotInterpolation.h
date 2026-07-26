#pragma once

#include <RTSEngine/Presentation/PresentationScene.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rts::presentation {

enum class ViewLifecycle : std::uint8_t {
    Stable,
    Spawned,
    Despawned
};

struct InterpolationOptions final {
    float teleportDistance{8.0f};
    bool includeDespawned{true};
};

struct InterpolatedEntity final {
    PresentationEntity current{};
    float x{};
    float y{};
    float opacity{1.0f};
    ViewLifecycle lifecycle{ViewLifecycle::Stable};
};

struct InterpolatedScene final {
    std::uint64_t previousTick{};
    std::uint64_t currentTick{};
    std::uint64_t simulationHash{};
    float alpha{};
    std::int32_t availableResources{};
    std::uint32_t observedTeam{};
    VisibilityMask visibility{};
    std::vector<InterpolatedEntity> entities;
};

class PresentationSceneBuffer final {
public:
    bool publish(PresentationScene scene) {
        if (!IsCanonicalScene(scene)) return false;
        if (!ready_) {
            previous_ = scene;
            current_ = std::move(scene);
            ready_ = true;
            return true;
        }
        if (scene.tick <= current_.tick) return false;
        previous_ = std::move(current_);
        current_ = std::move(scene);
        return true;
    }

    void clear() noexcept {
        previous_ = {};
        current_ = {};
        ready_ = false;
    }

    bool ready() const noexcept { return ready_; }

    const PresentationScene& previous() const noexcept { return previous_; }
    const PresentationScene& current() const noexcept { return current_; }

    InterpolatedScene sample(
        float alpha,
        InterpolationOptions options = {}) const {
        if (!ready_) return {};
        return Interpolate(previous_, current_, alpha, options);
    }

    static InterpolatedScene Interpolate(
        const PresentationScene& previous,
        const PresentationScene& current,
        float alpha,
        InterpolationOptions options = {}) {
        InterpolatedScene output;
        output.previousTick = previous.tick;
        output.currentTick = current.tick;
        output.simulationHash = current.simulationHash;
        output.alpha = std::isfinite(alpha)
            ? std::clamp(alpha, 0.0f, 1.0f) : 0.0f;
        output.availableResources = current.availableResources;
        output.observedTeam = current.observedTeam;
        output.visibility = current.visibility;

        std::size_t before = 0;
        std::size_t after = 0;
        output.entities.reserve(
            previous.entities.size() + current.entities.size());
        while (before < previous.entities.size() ||
               after < current.entities.size()) {
            if (before < previous.entities.size() &&
                after < current.entities.size() &&
                previous.entities[before].viewId ==
                    current.entities[after].viewId) {
                output.entities.push_back(interpolateExisting(
                    previous.entities[before], current.entities[after],
                    output.alpha, options));
                ++before;
                ++after;
                continue;
            }

            if (after >= current.entities.size() ||
                (before < previous.entities.size() &&
                 previous.entities[before].viewId <
                     current.entities[after].viewId)) {
                if (options.includeDespawned) {
                    auto entity = previous.entities[before];
                    output.entities.push_back(
                        {entity, entity.x, entity.y,
                         1.0f - output.alpha,
                         ViewLifecycle::Despawned});
                }
                ++before;
                continue;
            }

            auto entity = current.entities[after];
            output.entities.push_back(
                {entity, entity.x, entity.y,
                 output.alpha, ViewLifecycle::Spawned});
            ++after;
        }
        return output;
    }

private:
    static InterpolatedEntity interpolateExisting(
        const PresentationEntity& previous,
        const PresentationEntity& current,
        float alpha,
        const InterpolationOptions& options) noexcept {
        const auto deltaX = current.x - previous.x;
        const auto deltaY = current.y - previous.y;
        const auto distanceSquared = deltaX * deltaX + deltaY * deltaY;
        const auto threshold = std::max(0.0f, options.teleportDistance);
        const bool teleported = distanceSquared > threshold * threshold;
        return {
            current,
            teleported ? current.x : previous.x + deltaX * alpha,
            teleported ? current.y : previous.y + deltaY * alpha,
            1.0f,
            ViewLifecycle::Stable
        };
    }

    PresentationScene previous_{};
    PresentationScene current_{};
    bool ready_{};
};

} // namespace rts::presentation
