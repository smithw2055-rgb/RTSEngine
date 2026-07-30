#pragma once

#include <RTSEngine/Presentation/PresentationEvents.h>
#include <RTSEngine/Presentation/PresentationScene.h>
#include <RTSEngine/Rts/G3GameSession.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::rts_presentation {

inline constexpr std::uint32_t kG3PresentationEventDomain = 4u;
inline constexpr presentation::ViewId kG3SyntheticViewMask =
    std::uint64_t{1} << 63u;

struct G3AbilityTelegraph final {
    presentation::PresentationEventId eventId{};
    std::uint64_t tick{};
    std::uint32_t abilityId{};
    presentation::ViewId casterView{};
    presentation::ViewId targetView{};
    float x{};
    float y{};
};

struct G3StatusVisual final {
    std::uint64_t instanceId{};
    std::uint32_t statusId{};
    presentation::ViewId targetView{};
    std::uint16_t stacks{};
    std::uint64_t remainingTicks{};
};

struct G3PresentationFrame final {
    std::uint64_t tick{};
    std::vector<presentation::PresentationEntity> projectiles;
    std::vector<presentation::PresentationEvent> events;
    std::vector<G3AbilityTelegraph> telegraphs;
    std::vector<G3StatusVisual> statuses;
};

class G3PresentationBridge final {
public:
    static G3PresentationFrame extract(
        const gameplay::RtsG3GameSession& session,
        std::uint32_t observedTeam,
        const presentation::VisualCatalog* visuals = nullptr) {
        G3PresentationFrame result;
        result.tick =
            session.base().simulation().snapshot().tick;
        appendProjectiles(session, observedTeam, visuals, result);
        appendEvents(session, observedTeam, result);
        appendStatuses(session, observedTeam, result);
        return result;
    }

private:
    static presentation::ViewId entityView(
        ecs::Entity entity) noexcept {
        return entity.valid()
            ? presentation::MakeViewId(
                  entity.index, entity.generation)
            : 0;
    }

    static presentation::ViewId projectileView(
        std::uint64_t projectileId) noexcept {
        return kG3SyntheticViewMask |
               (projectileId &
                ~kG3SyntheticViewMask);
    }

    static bool visibleCell(
        const gameplay::RtsG3GameSession& session,
        std::uint32_t observedTeam,
        gameplay::GridPoint point) {
        return observedTeam == 0 ||
               session.base().simulation().vision().visible(
                   observedTeam, point);
    }

    static bool visibleEntity(
        const gameplay::RtsG3GameSession& session,
        std::uint32_t observedTeam,
        ecs::Entity entity) {
        if (!entity.valid()) return false;
        const auto& world =
            session.base().simulation().world();
        const auto* team =
            world.try_get<gameplay::Team>(entity);
        const auto* position =
            world.try_get<gameplay::Position>(entity);
        if (!position) return false;
        return observedTeam == 0 ||
               (team && team->id == observedTeam) ||
               visibleCell(
                   session,
                   observedTeam,
                   {position->x, position->y});
    }

    static bool visibleProjectile(
        const gameplay::RtsG3GameSession& session,
        std::uint32_t observedTeam,
        const gameplay::ProjectileState& projectile) {
        if (observedTeam == 0 ||
            projectile.sourceTeam == observedTeam ||
            visibleEntity(
                session, observedTeam, projectile.target)) {
            return true;
        }
        return visibleCell(
            session,
            observedTeam,
            projectile.position.cell());
    }

    static void resolveEventPosition(
        const gameplay::RtsG3GameSession& session,
        const gameplay::G3Event& event,
        float& x,
        float& y) {
        const auto& world =
            session.base().simulation().world();
        const auto* position =
            event.entity.valid()
            ? world.try_get<gameplay::Position>(
                  event.entity)
            : nullptr;
        if (!position && event.secondary.valid()) {
            position =
                world.try_get<gameplay::Position>(
                    event.secondary);
        }
        if (position) {
            x = static_cast<float>(position->x);
            y = static_cast<float>(position->y);
        } else {
            x = static_cast<float>(event.point.x);
            y = static_cast<float>(event.point.y);
        }
    }

    static bool visibleEvent(
        const gameplay::RtsG3GameSession& session,
        std::uint32_t observedTeam,
        const gameplay::G3Event& event) {
        if (observedTeam == 0) return true;
        if (visibleEntity(
                session, observedTeam, event.entity) ||
            visibleEntity(
                session, observedTeam, event.secondary)) {
            return true;
        }
        return visibleCell(
            session, observedTeam, event.point);
    }

    static void appendProjectiles(
        const gameplay::RtsG3GameSession& session,
        std::uint32_t observedTeam,
        const presentation::VisualCatalog* visuals,
        G3PresentationFrame& output) {
        output.projectiles.reserve(
            session.projectiles().size());
        for (const auto& projectile :
             session.projectiles()) {
            if (!visibleProjectile(
                    session,
                    observedTeam,
                    projectile)) {
                continue;
            }
            presentation::PresentationEntity entity;
            entity.viewId =
                projectileView(projectile.id);
            entity.kind =
                presentation::SceneEntityKind::Projectile;
            entity.definitionId =
                projectile.definitionId;
            entity.teamId = projectile.sourceTeam;
            entity.x =
                static_cast<float>(projectile.position.x) /
                gameplay::FixedPosition2D::kOne;
            entity.y =
                static_cast<float>(projectile.position.y) /
                gameplay::FixedPosition2D::kOne;
            entity.visible = true;
            entity.layer =
                presentation::RenderLayer::
                    ProjectileAndEffect;
            if (visuals) {
                const auto* binding = visuals->resolve(
                    entity.kind, entity.definitionId);
                if (binding) {
                    entity.spriteAsset =
                        binding->spriteAsset;
                    entity.animationAsset =
                        binding->animationAsset;
                    entity.layer = binding->layer;
                    entity.sortBias =
                        binding->sortBias;
                }
            }
            output.projectiles.push_back(entity);
        }
        std::sort(
            output.projectiles.begin(),
            output.projectiles.end(),
            [](const auto& first, const auto& second) {
                return first.viewId < second.viewId;
            });
    }

    static void appendEvents(
        const gameplay::RtsG3GameSession& session,
        std::uint32_t observedTeam,
        G3PresentationFrame& output) {
        const auto& sourceEvents = session.events();
        output.events.reserve(sourceEvents.size());
        for (std::size_t ordinal = 0;
             ordinal < sourceEvents.size();
             ++ordinal) {
            const auto& source = sourceEvents[ordinal];
            if (!visibleEvent(
                    session, observedTeam, source)) {
                continue;
            }
            presentation::PresentationEvent event;
            event.tick = source.tick;
            event.domain =
                kG3PresentationEventDomain;
            event.type =
                static_cast<std::uint32_t>(
                    source.type) + 1u;
            event.sourceView =
                entityView(source.entity);
            event.targetView =
                entityView(source.secondary);
            event.objectId = source.objectId;
            event.value = source.value;
            resolveEventPosition(
                session, source, event.x, event.y);
            event.id =
                presentation::MakePresentationEventId(
                    event.domain,
                    event.tick,
                    static_cast<std::uint32_t>(
                        ordinal),
                    event.type,
                    event.sourceView,
                    event.targetView,
                    event.objectId,
                    event.value);
            output.events.push_back(event);

            if (source.type ==
                gameplay::G3EventType::
                    AbilityCastStarted) {
                output.telegraphs.push_back({
                    event.id,
                    event.tick,
                    event.objectId,
                    event.sourceView,
                    event.targetView,
                    event.x,
                    event.y});
            }
        }
        std::stable_sort(
            output.events.begin(),
            output.events.end(),
            [](const auto& first, const auto& second) {
                return first.tick < second.tick ||
                       (first.tick == second.tick &&
                        first.id < second.id);
            });
        std::stable_sort(
            output.telegraphs.begin(),
            output.telegraphs.end(),
            [](const auto& first, const auto& second) {
                return first.tick < second.tick ||
                       (first.tick == second.tick &&
                        first.eventId < second.eventId);
            });
    }

    static void appendStatuses(
        const gameplay::RtsG3GameSession& session,
        std::uint32_t observedTeam,
        G3PresentationFrame& output) {
        const auto nextTick =
            session.base().simulation().nextExpectedTick();
        output.statuses.reserve(
            session.statuses().size());
        for (const auto& status : session.statuses()) {
            if (!visibleEntity(
                    session,
                    observedTeam,
                    status.target)) {
                continue;
            }
            output.statuses.push_back({
                status.instanceId,
                status.definitionId,
                entityView(status.target),
                status.stacks,
                status.expiresAtTick > nextTick
                    ? status.expiresAtTick - nextTick
                    : 0});
        }
        std::sort(
            output.statuses.begin(),
            output.statuses.end(),
            [](const auto& first, const auto& second) {
                return first.instanceId <
                       second.instanceId;
            });
    }
};

} // namespace rts::rts_presentation
