#include <RTSEngine/Roguelite/RunSimulation.h>

namespace rts::roguelite {

RunSimulation::RunSimulation(
    std::int32_t width,
    std::int32_t height,
    std::uint64_t rootSeed)
    : tower_(width, height, rootSeed),
      rootSeed_(rootSeed) {}

RunSimulation::~RunSimulation() = default;

} // namespace rts::roguelite
