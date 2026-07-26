#include <RTSEngine/Platform/InputState.h>
#include <RTSEngine/Rts/Simulation.h>
#include <RTSEngine/RtsPresentation/DesktopController.h>

#include <cassert>
#include <cstdlib>
#include <iostream>

namespace {

using namespace rts;

void check(bool value) {
    assert(value);
    if (!value) std::abort();
}

platform::PlatformEvent pointerEvent(
    platform::PlatformEventType type,
    platform::PointerButton button,
    float x,
    float y) {
    platform::PlatformEvent event;
    event.type = type;
    event.button = button;
    event.x = x;
    event.y = y;
    return event;
}

platform::PlatformEvent keyEvent(
    platform::PlatformEventType type,
    platform::KeyCode key) {
    platform::PlatformEvent event;
    event.type = type;
    event.key = key;
    return event;
}

void testSelectionCommandsAndDecorations() {
    gameplay::RtsSimulation simulation(20, 10);
    const auto player = simulation.createUnit(
        {5, 5}, {1}, 1, {20, 0, 4, 4, 1, 0}, 8);
    const auto enemy = simulation.createUnit(
        {10, 5}, {1}, 2, {20, 0, 2, 3, 1, 0}, 4);
    (void)enemy;
    simulation.step(0);

    rts_presentation::DesktopController controller(
        {1, 1, 77, 1.0f, 4.0f, 10.0f, 8.0f, 40.0f});
    controller.setCamera({10.0f, 5.0f, 20.0f, 10.0f, true});

    platform::InputState input;
    input.beginFrame();
    input.apply(pointerEvent(platform::PlatformEventType::PointerMoved,
                             platform::PointerButton::Left, 250.0f, 250.0f));
    input.apply(pointerEvent(platform::PlatformEventType::PointerDown,
                             platform::PointerButton::Left, 250.0f, 250.0f));
    auto result = controller.update(
        input, simulation.snapshot(), {1, 1000, 500, 1.0f / 60.0f, false});
    check(result.commands.empty());
    check(controller.selectionDrag().active);

    input.beginFrame();
    input.apply(pointerEvent(platform::PlatformEventType::PointerUp,
                             platform::PointerButton::Left, 250.0f, 250.0f));
    result = controller.update(
        input, simulation.snapshot(), {1, 1000, 500, 1.0f / 60.0f, false});
    check(result.selectionChanged);
    check(controller.selection().size() == 1);
    check(controller.selection().front() == player);

    input.beginFrame();
    input.apply(pointerEvent(platform::PlatformEventType::PointerMoved,
                             platform::PointerButton::Right, 400.0f, 250.0f));
    input.apply(pointerEvent(platform::PlatformEventType::PointerDown,
                             platform::PointerButton::Right, 400.0f, 250.0f));
    controller.update(
        input, simulation.snapshot(), {1, 1000, 500, 1.0f / 60.0f, false});

    input.beginFrame();
    input.apply(pointerEvent(platform::PlatformEventType::PointerUp,
                             platform::PointerButton::Right, 400.0f, 250.0f));
    result = controller.update(
        input, simulation.snapshot(), {1, 1000, 500, 1.0f / 60.0f, false});
    check(result.commands.size() == 1);
    check(result.commands.front().type == gameplay::CommandType::Move);
    check(result.commands.front().subject == player);
    check(result.commands.front().targetX == 8);
    check(result.commands.front().targetY == 5);

    input.beginFrame();
    input.apply(pointerEvent(platform::PlatformEventType::PointerMoved,
                             platform::PointerButton::Right, 500.0f, 250.0f));
    input.apply(pointerEvent(platform::PlatformEventType::PointerDown,
                             platform::PointerButton::Right, 500.0f, 250.0f));
    controller.update(
        input, simulation.snapshot(), {2, 1000, 500, 1.0f / 60.0f, false});
    input.beginFrame();
    input.apply(pointerEvent(platform::PlatformEventType::PointerUp,
                             platform::PointerButton::Right, 500.0f, 250.0f));
    result = controller.update(
        input, simulation.snapshot(), {2, 1000, 500, 1.0f / 60.0f, false});
    check(result.commands.size() == 1);
    check(result.commands.front().type == gameplay::CommandType::Attack);
    check(result.commands.front().targetEntity.valid());

    input.beginFrame();
    input.apply(keyEvent(platform::PlatformEventType::KeyDown,
                         platform::KeyCode::B));
    result = controller.update(
        input, simulation.snapshot(), {3, 1000, 500, 1.0f / 60.0f, false});
    check(result.modeChanged);
    check(controller.mode() ==
          rts_presentation::DesktopInteractionMode::Build);

    input.beginFrame();
    input.apply(pointerEvent(platform::PlatformEventType::PointerMoved,
                             platform::PointerButton::Left, 100.0f, 100.0f));
    input.apply(pointerEvent(platform::PlatformEventType::PointerDown,
                             platform::PointerButton::Left, 100.0f, 100.0f));
    result = controller.update(
        input, simulation.snapshot(), {3, 1000, 500, 1.0f / 60.0f, false});
    check(result.commands.size() == 1);
    check(result.commands.front().type == gameplay::CommandType::Build);
    check(result.commands.front().definitionId == 77);
    check(result.commands.front().targetX == 2);
    check(result.commands.front().targetY == 2);

    presentation::RenderPacket packet;
    packet.sprites.push_back(
        {presentation::MakeViewId(player.index, player.generation),
         1, 0, presentation::RenderLayer::WorldEntity,
         5.0f, 5.0f, 1.0f, 0,
         presentation::ViewLifecycle::Stable,
         render::BlendMode::Alpha});
    controller.decorate(packet);
    check(packet.worldOverlays.size() == 1);
    check(packet.worldOverlays.front().layer ==
          presentation::RenderLayer::SelectionAndDecal);
}

} // namespace

int main() {
    testSelectionCommandsAndDecorations();
    std::cout << "desktop controller tests passed\n";
    return 0;
}
