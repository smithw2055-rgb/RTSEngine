#pragma once

#include <algorithm>
#include <cstdint>

namespace rts::sim {

using Tick = std::uint64_t;

struct FrameStepPlan final {
    Tick firstTick{0};
    std::uint32_t tickCount{0};
    double alpha{0.0};
    double droppedSeconds{0.0};
    bool simulationBehind{false};
};

class FixedStepClock final {
public:
    explicit FixedStepClock(double ticksPerSecond = 30.0,
                            std::uint32_t maxTicksPerFrame = 4,
                            double maxAccumulatedSeconds = 0.25) noexcept
        : tickSeconds_(1.0 / ticksPerSecond),
          maxTicksPerFrame_(maxTicksPerFrame),
          maxAccumulatedSeconds_(maxAccumulatedSeconds) {}

    [[nodiscard]] FrameStepPlan Advance(double frameSeconds) noexcept {
        FrameStepPlan plan;
        plan.firstTick = currentTick_ + 1;

        const double sanitized = std::max(0.0, frameSeconds);
        accumulator_ += sanitized;
        if (accumulator_ > maxAccumulatedSeconds_) {
            plan.droppedSeconds = accumulator_ - maxAccumulatedSeconds_;
            accumulator_ = maxAccumulatedSeconds_;
        }

        const auto available = static_cast<std::uint32_t>(accumulator_ / tickSeconds_);
        plan.tickCount = std::min(available, maxTicksPerFrame_);
        accumulator_ -= static_cast<double>(plan.tickCount) * tickSeconds_;
        plan.simulationBehind = available > maxTicksPerFrame_;
        plan.alpha = std::clamp(accumulator_ / tickSeconds_, 0.0, 1.0);
        return plan;
    }

    void CommitTick() noexcept { ++currentTick_; }

    [[nodiscard]] Tick CurrentTick() const noexcept { return currentTick_; }
    [[nodiscard]] double TickSeconds() const noexcept { return tickSeconds_; }

private:
    double tickSeconds_{1.0 / 30.0};
    double accumulator_{0.0};
    std::uint32_t maxTicksPerFrame_{4};
    double maxAccumulatedSeconds_{0.25};
    Tick currentTick_{0};
};

} // namespace rts::sim
