#include <RTSEngine/Runtime/DesktopFrameLoop.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace rts::runtime {

DesktopFrameLoop::DesktopFrameLoop(platform::Platform& platform,
                                   StepFunction step,
                                   DesktopFrameLoopConfig config)
    : platform_(platform),
      config_(std::move(config)),
      simulation_(std::move(step),
                  config_.ticksPerSecond,
                  config_.maximumTicksPerFrame) {}

DesktopFrameLoop::~DesktopFrameLoop() { shutdown(); }

bool DesktopFrameLoop::initialize() {
    if (initialized_) return true;
    if (!(config_.ticksPerSecond > 0.0) ||
        config_.maximumTicksPerFrame == 0 ||
        !(config_.maximumFrameSeconds > 0.0) ||
        !platform_.createWindow(config_.window, window_) ||
        !window_.valid() ||
        !platform_.windowState(window_, windowState_)) {
        if (window_.valid()) (void)platform_.destroyWindow(window_);
        window_ = {};
        return false;
    }
    previousSeconds_ = platform_.monotonicSeconds();
    initialized_ = true;
    quitRequested_ = false;
    return true;
}

DesktopFrameResult DesktopFrameLoop::advanceFrame(
    const BeforeSimulationFunction& beforeSimulation,
    const RenderFunction& render) {
    DesktopFrameResult result;
    if (!initialized_ && !initialize()) {
        result.quitRequested = true;
        return result;
    }

    input_.beginFrame();
    std::vector<platform::PlatformEvent> events;
    platform_.pollEvents(events);
    input_.apply(events);
    if (!platform_.windowState(window_, windowState_)) {
        quitRequested_ = true;
    }

    const auto now = platform_.monotonicSeconds();
    const auto rawSeconds = std::max(0.0, now - previousSeconds_);
    previousSeconds_ = now;
    const auto frameSeconds = std::min(rawSeconds, config_.maximumFrameSeconds);

    DesktopFrameContext context;
    context.window = window_;
    context.windowState = windowState_;
    context.input = &input_;
    context.frameIndex = frameIndex_++;
    context.frameSeconds = frameSeconds;
    context.currentTick = simulation_.CurrentTick();
    context.targetTick = context.currentTick + 1u;

    if (beforeSimulation) beforeSimulation(context);
    result.stepPlan = simulation_.AdvanceFrame(frameSeconds);
    context.currentTick = simulation_.CurrentTick();
    result.context = context;
    if (render && windowState_.framebufferWidth > 0 &&
        windowState_.framebufferHeight > 0) {
        result.rendered = render(context, result.stepPlan);
    }

    quitRequested_ = quitRequested_ || input_.quitRequested() ||
                     windowState_.closeRequested;
    result.quitRequested = quitRequested_;
    return result;
}

void DesktopFrameLoop::shutdown() noexcept {
    if (window_.valid()) (void)platform_.destroyWindow(window_);
    window_ = {};
    windowState_ = {};
    initialized_ = false;
}

bool DesktopFrameLoop::initialized() const noexcept { return initialized_; }
bool DesktopFrameLoop::quitRequested() const noexcept { return quitRequested_; }
platform::WindowHandle DesktopFrameLoop::window() const noexcept { return window_; }
const platform::WindowState& DesktopFrameLoop::windowState() const noexcept {
    return windowState_;
}
const platform::InputState& DesktopFrameLoop::input() const noexcept {
    return input_;
}
void DesktopFrameLoop::resetSimulationTick(sim::Tick currentTick) noexcept {
    simulation_.Reset(currentTick);
}

sim::Tick DesktopFrameLoop::currentTick() const noexcept {
    return simulation_.CurrentTick();
}

} // namespace rts::runtime
