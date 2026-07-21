#pragma once

#include <cstdint>
#include <functional>

#include <rts/sim/FixedStepClock.h>

namespace rts::sim {

struct TickContext final {
    Tick tick{0};
    double tickSeconds{1.0 / 30.0};
};

class SimulationHost final {
public:
    using StepFunction = std::function<void(const TickContext&)>;

    explicit SimulationHost(StepFunction step,
                            double ticksPerSecond = 30.0,
                            std::uint32_t maxTicksPerFrame = 4);

    [[nodiscard]] FrameStepPlan AdvanceFrame(double frameSeconds);
    [[nodiscard]] Tick CurrentTick() const noexcept;

private:
    FixedStepClock clock_;
    StepFunction step_;
};

} // namespace rts::sim
