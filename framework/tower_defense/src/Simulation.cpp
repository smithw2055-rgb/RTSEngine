#include <RTSEngine/TowerDefense/Simulation.h>

namespace rts::tower_defense {

TowerDefenseSimulation::TowerDefenseSimulation(
    std::int32_t width,
    std::int32_t height,
    std::uint64_t rootSeed)
    : rts_(width, height),
      director_(rootSeed),
      rootSeed_(rootSeed) {}

TowerDefenseSimulation::~TowerDefenseSimulation() = default;

} // namespace rts::tower_defense
