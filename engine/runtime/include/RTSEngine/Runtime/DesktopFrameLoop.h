#pragma once

#include <RTSEngine/Platform/InputState.h>
#include <RTSEngine/Platform/Platform.h>
#include <rts/sim/SimulationHost.h>

#include <cstdint>
#include <functional>

namespace rts::runtime {

struct DesktopFrameLoopConfig final {
    platform::WindowDescription window{};
    double ticksPerSecond{30.0};
    std::uint32_t maximumTicksPerFrame{4};
    double maximumFrameSeconds{0.25};
};

struct DesktopFrameContext final {
    platform::WindowHandle window{};
    platform::WindowState windowState{};
    const platform::InputState* input{};
    std::uint64_t frameIndex{};
    double frameSeconds{};
    sim::Tick targetTick{};
    sim::Tick currentTick{};
};

struct DesktopFrameResult final {
    DesktopFrameContext context{};
    sim::FrameStepPlan stepPlan{};
    bool rendered{};
    bool quitRequested{};
};

class DesktopFrameLoop final {
public:
    using StepFunction = sim::SimulationHost::StepFunction;
    using BeforeSimulationFunction =
        std::function<void(const DesktopFrameContext&)>;
    using RenderFunction = std::function<bool(
        const DesktopFrameContext&, const sim::FrameStepPlan&)>;

    DesktopFrameLoop(platform::Platform& platform,
                     StepFunction step,
                     DesktopFrameLoopConfig config = {});
    ~DesktopFrameLoop();

    DesktopFrameLoop(const DesktopFrameLoop&) = delete;
    DesktopFrameLoop& operator=(const DesktopFrameLoop&) = delete;

    bool initialize();
    DesktopFrameResult advanceFrame(
        const BeforeSimulationFunction& beforeSimulation = {},
        const RenderFunction& render = {});
    void shutdown() noexcept;

    bool initialized() const noexcept;
    bool quitRequested() const noexcept;
    platform::WindowHandle window() const noexcept;
    const platform::WindowState& windowState() const noexcept;
    const platform::InputState& input() const noexcept;
    void resetSimulationTick(sim::Tick currentTick) noexcept;
    sim::Tick currentTick() const noexcept;

private:
    platform::Platform& platform_;
    DesktopFrameLoopConfig config_{};
    sim::SimulationHost simulation_;
    platform::WindowHandle window_{};
    platform::WindowState windowState_{};
    platform::InputState input_{};
    double previousSeconds_{};
    std::uint64_t frameIndex_{};
    bool initialized_{};
    bool quitRequested_{};
};

} // namespace rts::runtime
