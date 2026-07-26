#include <RTSEngine/RtsPresentation/DesktopController.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace rts::rts_presentation {
namespace {

bool pressed(const platform::PointerState& pointer,
             platform::PointerButton button) noexcept {
    return pointer.pressed[static_cast<std::size_t>(button)];
}

bool released(const platform::PointerState& pointer,
              platform::PointerButton button) noexcept {
    return pointer.released[static_cast<std::size_t>(button)];
}

bool down(const platform::PointerState& pointer,
          platform::PointerButton button) noexcept {
    return pointer.down[static_cast<std::size_t>(button)];
}

} // namespace

DesktopController::DesktopController(DesktopControllerConfig config) noexcept
    : config_(config) {
    config_.playerTeam = std::max<std::uint32_t>(1u, config_.playerTeam);
    config_.issuer = std::max<std::uint32_t>(1u, config_.issuer);
    config_.clickRadiusWorld = std::max(0.1f, config_.clickRadiusWorld);
    config_.dragThresholdPixels = std::max(1.0f, config_.dragThresholdPixels);
    config_.minimumWorldWidth = std::max(2.0f, config_.minimumWorldWidth);
    config_.maximumWorldWidth = std::max(
        config_.minimumWorldWidth, config_.maximumWorldWidth);
}

DesktopControllerResult DesktopController::update(
    const platform::InputState& input,
    const gameplay::WorldSnapshot& snapshot,
    DesktopControllerFrame frame) {
    DesktopControllerResult result;
    pruneSelection(snapshot);
    updateCamera(input, snapshot, frame, result);

    const auto& pointer = input.pointer();
    pointerWorld_ = screenToWorld(pointer.x, pointer.y,
                                  frame.framebufferWidth,
                                  frame.framebufferHeight);

    if (input.keyPressed(platform::KeyCode::Escape)) {
        const bool changed = mode_ != DesktopInteractionMode::Select ||
                             !selection_.empty();
        mode_ = DesktopInteractionMode::Select;
        selection_.clear();
        drag_ = {};
        result.selectionChanged = changed;
        result.modeChanged = changed;
    }
    if (input.keyPressed(platform::KeyCode::B)) {
        mode_ = mode_ == DesktopInteractionMode::Build
            ? DesktopInteractionMode::Select
            : DesktopInteractionMode::Build;
        drag_ = {};
        result.modeChanged = true;
    }
    if (input.keyPressed(platform::KeyCode::A)) {
        mode_ = mode_ == DesktopInteractionMode::AttackMove
            ? DesktopInteractionMode::Select
            : DesktopInteractionMode::AttackMove;
        drag_ = {};
        result.modeChanged = true;
    }

    const bool append = (input.modifiers() &
        platform::PlatformModifierShift) != 0;
    if (input.keyPressed(platform::KeyCode::S)) {
        issueSimple(gameplay::CommandType::Stop, frame.targetTick,
                    append, result);
    }
    if (input.keyPressed(platform::KeyCode::H)) {
        issueSimple(gameplay::CommandType::HoldPosition, frame.targetTick,
                    append, result);
    }

    if (!frame.pointerCaptured &&
        pressed(pointer, platform::PointerButton::Left)) {
        if (mode_ == DesktopInteractionMode::Build) {
            auto command = makeCommand(gameplay::CommandType::Build,
                                       frame.targetTick);
            command.targetX = static_cast<std::int32_t>(
                std::floor(pointerWorld_.x));
            command.targetY = static_cast<std::int32_t>(
                std::floor(pointerWorld_.y));
            command.definitionId = config_.buildingDefinitionId;
            result.commands.push_back(command);
            mode_ = DesktopInteractionMode::Select;
            result.modeChanged = true;
        } else {
            drag_.active = true;
            drag_.startX = pointer.x;
            drag_.startY = pointer.y;
            drag_.currentX = pointer.x;
            drag_.currentY = pointer.y;
        }
    }
    if (drag_.active && down(pointer, platform::PointerButton::Left)) {
        drag_.currentX = pointer.x;
        drag_.currentY = pointer.y;
    }
    if (drag_.active && released(pointer, platform::PointerButton::Left)) {
        drag_.currentX = pointer.x;
        drag_.currentY = pointer.y;
        finishSelection(input, snapshot, frame, result);
        drag_.active = false;
    }

    if (!frame.pointerCaptured &&
        released(pointer, platform::PointerButton::Right)) {
        issueContextCommand(input, snapshot, frame, result);
    }
    return result;
}

void DesktopController::decorate(presentation::RenderPacket& packet) const {
    for (const auto selected : selection_) {
        const auto iterator = std::find_if(
            packet.sprites.begin(), packet.sprites.end(),
            [selected](const presentation::SpriteInstance& sprite) {
                return sprite.viewId == presentation::MakeViewId(
                    selected.index, selected.generation);
            });
        if (iterator == packet.sprites.end()) continue;
        packet.worldOverlays.push_back(
            {iterator->x, iterator->y, 1.25f, 1.25f,
             0.18f, 0.85f, 1.0f, 0.28f,
             presentation::RenderLayer::SelectionAndDecal,
             render::BlendMode::Alpha, -100});
    }
    if (mode_ == DesktopInteractionMode::Build) {
        const auto cellX = std::floor(pointerWorld_.x) + 0.5f;
        const auto cellY = std::floor(pointerWorld_.y) + 0.5f;
        packet.worldOverlays.push_back(
            {cellX, cellY, 2.0f, 2.0f,
             0.20f, 0.95f, 0.45f, 0.32f,
             presentation::RenderLayer::SelectionAndDecal,
             render::BlendMode::Alpha, -90});
    }
    presentation::RenderPacketBuilder::sort(packet);
}

const presentation::Camera2D& DesktopController::camera() const noexcept {
    return camera_;
}

void DesktopController::setCamera(presentation::Camera2D camera) noexcept {
    if (camera.valid()) camera_ = camera;
}

DesktopInteractionMode DesktopController::mode() const noexcept { return mode_; }
void DesktopController::setMode(DesktopInteractionMode mode) noexcept {
    mode_ = mode;
    drag_ = {};
}
const std::vector<ecs::Entity>& DesktopController::selection() const noexcept {
    return selection_;
}
const SelectionDrag& DesktopController::selectionDrag() const noexcept {
    return drag_;
}

gameplay::GridPoint DesktopController::pointerWorldCell() const noexcept {
    return {static_cast<std::int32_t>(std::floor(pointerWorld_.x)),
            static_cast<std::int32_t>(std::floor(pointerWorld_.y))};
}

void DesktopController::clearSelection() noexcept { selection_.clear(); }

bool DesktopController::entityEquals(ecs::Entity a, ecs::Entity b) noexcept {
    return a.index == b.index && a.generation == b.generation;
}

bool DesktopController::isUnit(
    const gameplay::SnapshotEntity& entity) noexcept {
    return entity.kind == gameplay::SnapshotKind::Unit;
}

bool DesktopController::containsEntity(
    const std::vector<ecs::Entity>& values,
    ecs::Entity entity) noexcept {
    return std::any_of(values.begin(), values.end(),
                       [entity](ecs::Entity value) {
                           return entityEquals(value, entity);
                       });
}

float DesktopController::square(float value) noexcept { return value * value; }

DesktopController::WorldPoint DesktopController::screenToWorld(
    float x,
    float y,
    std::uint32_t width,
    std::uint32_t height) const noexcept {
    if (width == 0 || height == 0) return {camera_.centerX, camera_.centerY};
    const auto normalizedX = x / static_cast<float>(width);
    const auto normalizedY = y / static_cast<float>(height);
    const auto left = camera_.centerX - camera_.worldWidth * 0.5f;
    const auto top = camera_.centerY - camera_.worldHeight * 0.5f;
    return {left + normalizedX * camera_.worldWidth,
            top + normalizedY * camera_.worldHeight};
}

void DesktopController::updateCamera(
    const platform::InputState& input,
    const gameplay::WorldSnapshot& snapshot,
    DesktopControllerFrame frame,
    DesktopControllerResult& result) {
    const auto previous = camera_;
    const auto delta = std::max(0.0f, frame.deltaSeconds) *
                       config_.panSpeedWorldPerSecond;
    if (input.keyDown(platform::KeyCode::Left)) camera_.centerX -= delta;
    if (input.keyDown(platform::KeyCode::Right)) camera_.centerX += delta;
    if (input.keyDown(platform::KeyCode::Up)) camera_.centerY -= delta;
    if (input.keyDown(platform::KeyCode::Down)) camera_.centerY += delta;
    if (down(input.pointer(), platform::PointerButton::Middle)) {
        const auto pixelsPerWorldX = frame.framebufferWidth == 0
            ? 1.0f
            : static_cast<float>(frame.framebufferWidth) / camera_.worldWidth;
        const auto pixelsPerWorldY = frame.framebufferHeight == 0
            ? 1.0f
            : static_cast<float>(frame.framebufferHeight) / camera_.worldHeight;
        camera_.centerX -= input.pointer().deltaX / pixelsPerWorldX;
        camera_.centerY -= input.pointer().deltaY / pixelsPerWorldY;
    }

    if (frame.framebufferWidth != 0 && frame.framebufferHeight != 0) {
        const auto aspect = static_cast<float>(frame.framebufferHeight) /
                            static_cast<float>(frame.framebufferWidth);
        camera_.worldHeight = camera_.worldWidth * aspect;
    }
    if (input.pointer().wheelY != 0.0f) {
        const auto factor = std::pow(0.88f, input.pointer().wheelY);
        camera_.worldWidth = std::clamp(
            camera_.worldWidth * factor,
            config_.minimumWorldWidth,
            config_.maximumWorldWidth);
        if (frame.framebufferWidth != 0 && frame.framebufferHeight != 0) {
            camera_.worldHeight = camera_.worldWidth *
                static_cast<float>(frame.framebufferHeight) /
                static_cast<float>(frame.framebufferWidth);
        }
    }
    clampCamera(snapshot);
    result.cameraChanged = previous.centerX != camera_.centerX ||
                           previous.centerY != camera_.centerY ||
                           previous.worldWidth != camera_.worldWidth ||
                           previous.worldHeight != camera_.worldHeight;
}

void DesktopController::pruneSelection(
    const gameplay::WorldSnapshot& snapshot) {
    selection_.erase(
        std::remove_if(
            selection_.begin(), selection_.end(),
            [&snapshot](ecs::Entity selected) {
                return std::none_of(
                    snapshot.entities.begin(), snapshot.entities.end(),
                    [selected](const gameplay::SnapshotEntity& entity) {
                        return entityEquals(entity.entity, selected);
                    });
            }),
        selection_.end());
}

const gameplay::SnapshotEntity* DesktopController::entityAt(
    const gameplay::WorldSnapshot& snapshot,
    WorldPoint point,
    bool enemyOnly) const noexcept {
    const gameplay::SnapshotEntity* best = nullptr;
    auto bestDistance = square(config_.clickRadiusWorld);
    for (const auto& entity : snapshot.entities) {
        if (!isUnit(entity)) continue;
        const bool enemy = entity.teamId != 0 &&
                           entity.teamId != config_.playerTeam;
        if (enemyOnly != enemy) continue;
        if (enemy && !visibleToPlayer(snapshot, entity)) continue;
        const auto distance = square(static_cast<float>(entity.x) - point.x) +
                              square(static_cast<float>(entity.y) - point.y);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = &entity;
        }
    }
    return best;
}

bool DesktopController::visibleToPlayer(
    const gameplay::WorldSnapshot& snapshot,
    const gameplay::SnapshotEntity& entity) const noexcept {
    if (entity.teamId == 0 || entity.teamId == config_.playerTeam) return true;
    const auto iterator = std::lower_bound(
        snapshot.visibility.begin(), snapshot.visibility.end(),
        config_.playerTeam,
        [](const gameplay::TeamVisibilitySnapshot& value,
           std::uint32_t team) { return value.teamId < team; });
    if (iterator == snapshot.visibility.end() ||
        iterator->teamId != config_.playerTeam ||
        entity.x < 0 || entity.y < 0 ||
        entity.x >= snapshot.visibilityWidth ||
        entity.y >= snapshot.visibilityHeight) return false;
    const auto index = static_cast<std::size_t>(entity.y) *
                       static_cast<std::size_t>(snapshot.visibilityWidth) +
                       static_cast<std::size_t>(entity.x);
    return index < iterator->current.size() && iterator->current[index] != 0;
}

void DesktopController::finishSelection(
    const platform::InputState& input,
    const gameplay::WorldSnapshot& snapshot,
    DesktopControllerFrame frame,
    DesktopControllerResult& result) {
    const auto dx = drag_.currentX - drag_.startX;
    const auto dy = drag_.currentY - drag_.startY;
    const bool additive = (input.modifiers() &
        platform::PlatformModifierShift) != 0;
    if (!additive) selection_.clear();

    if (square(dx) + square(dy) <= square(config_.dragThresholdPixels)) {
        const auto world = screenToWorld(drag_.currentX, drag_.currentY,
                                         frame.framebufferWidth,
                                         frame.framebufferHeight);
        if (const auto* entity = entityAt(snapshot, world, false)) {
            if (!containsEntity(selection_, entity->entity)) {
                selection_.push_back(entity->entity);
            }
        }
    } else {
        const auto first = screenToWorld(
            std::min(drag_.startX, drag_.currentX),
            std::min(drag_.startY, drag_.currentY),
            frame.framebufferWidth, frame.framebufferHeight);
        const auto second = screenToWorld(
            std::max(drag_.startX, drag_.currentX),
            std::max(drag_.startY, drag_.currentY),
            frame.framebufferWidth, frame.framebufferHeight);
        for (const auto& entity : snapshot.entities) {
            if (!isUnit(entity) || entity.teamId != config_.playerTeam) continue;
            const auto x = static_cast<float>(entity.x);
            const auto y = static_cast<float>(entity.y);
            if (x >= first.x && x <= second.x &&
                y >= first.y && y <= second.y &&
                !containsEntity(selection_, entity.entity)) {
                selection_.push_back(entity.entity);
            }
        }
    }
    std::sort(selection_.begin(), selection_.end(),
              [](ecs::Entity a, ecs::Entity b) {
                  return a.index < b.index ||
                         (a.index == b.index && a.generation < b.generation);
              });
    result.selectionChanged = true;
}

void DesktopController::issueContextCommand(
    const platform::InputState& input,
    const gameplay::WorldSnapshot& snapshot,
    DesktopControllerFrame frame,
    DesktopControllerResult& result) {
    if (selection_.empty()) return;
    const bool append = (input.modifiers() &
        platform::PlatformModifierShift) != 0;
    const auto target = entityAt(snapshot, pointerWorld_, true);
    const auto type = target ? gameplay::CommandType::Attack
        : (mode_ == DesktopInteractionMode::AttackMove
            ? gameplay::CommandType::AttackMove
            : gameplay::CommandType::Move);
    for (const auto selected : selection_) {
        auto command = makeCommand(type, frame.targetTick);
        command.subject = selected;
        command.targetX = static_cast<std::int32_t>(std::floor(pointerWorld_.x));
        command.targetY = static_cast<std::int32_t>(std::floor(pointerWorld_.y));
        command.append = append;
        if (target) command.targetEntity = target->entity;
        result.commands.push_back(command);
    }
    if (mode_ == DesktopInteractionMode::AttackMove) {
        mode_ = DesktopInteractionMode::Select;
        result.modeChanged = true;
    }
}

void DesktopController::issueSimple(
    gameplay::CommandType type,
    std::uint64_t targetTick,
    bool append,
    DesktopControllerResult& result) {
    for (const auto selected : selection_) {
        auto command = makeCommand(type, targetTick);
        command.subject = selected;
        command.append = append;
        result.commands.push_back(command);
    }
}

gameplay::TickCommand DesktopController::makeCommand(
    gameplay::CommandType type,
    std::uint64_t tick) noexcept {
    gameplay::TickCommand command;
    command.targetTick = tick;
    command.issuer = config_.issuer;
    command.sequence = nextSequence_++;
    if (nextSequence_ == 0) nextSequence_ = 1;
    command.type = type;
    return command;
}

void DesktopController::clampCamera(
    const gameplay::WorldSnapshot& snapshot) noexcept {
    const auto width = snapshot.visibilityWidth > 0
        ? static_cast<float>(snapshot.visibilityWidth) : camera_.worldWidth;
    const auto height = snapshot.visibilityHeight > 0
        ? static_cast<float>(snapshot.visibilityHeight) : camera_.worldHeight;
    const auto halfWidth = std::min(camera_.worldWidth * 0.5f, width * 0.5f);
    const auto halfHeight = std::min(camera_.worldHeight * 0.5f, height * 0.5f);
    camera_.centerX = std::clamp(camera_.centerX, halfWidth,
                                 std::max(halfWidth, width - halfWidth));
    camera_.centerY = std::clamp(camera_.centerY, halfHeight,
                                 std::max(halfHeight, height - halfHeight));
}

} // namespace rts::rts_presentation
