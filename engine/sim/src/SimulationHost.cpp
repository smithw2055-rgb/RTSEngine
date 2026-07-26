#include <rts/sim/SimulationHost.h>

#include <utility>

namespace rts::sim {

SimulationHost::SimulationHost(StepFunction step,
                               double ticksPerSecond,
                               std::uint32_t maxTicksPerFrame)
    : clock_(ticksPerSecond, maxTicksPerFrame), step_(std::move(step)) {}

FrameStepPlan SimulationHost::AdvanceFrame(double frameSeconds) {
    auto plan = clock_.Advance(frameSeconds);
    for (std::uint32_t i = 0; i < plan.tickCount; ++i) {
        const TickContext context{plan.firstTick + i, clock_.TickSeconds()};
        if (step_) step_(context);
        clock_.CommitTick();
    }
    return plan;
}

Tick SimulationHost::CurrentTick() const noexcept {
    return clock_.CurrentTick();
}

} // namespace rts::sim
