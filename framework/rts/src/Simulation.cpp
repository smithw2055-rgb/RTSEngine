#include <RTSEngine/Rts/Simulation.h>

namespace rts::gameplay {

RtsSimulation::RtsSimulation(std::int32_t width, std::int32_t height)
    : navigation_(width, height),
      vision_(width, height),
      influence_(width, height),
      movement_(width, height),
      building_(resources_, navigation_),
      combat_(width, height) {
    installSystems();
}

RtsSimulation::~RtsSimulation() = default;

} // namespace rts::gameplay
