#include <RTSEngine/Rts/AuthoritativeStateHash.h>
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
    scheduler_.add(
        ecs::Stage::Snapshot,
        10,
        410,
        [this](ecs::World& world, const ecs::SystemContext&) {
            snapshot_.worldHash = FinalizeRtsAuthoritativeWorldHash(
                snapshot_.worldHash,
                world.entityRegistryHash(),
                requiredPathStart_,
                requiredPathGoal_,
                building_.nextConstructionId(),
                nextProductionId_,
                playerTeamId_);
            influenceWorldHash_ = snapshot_.worldHash;
        });
}

RtsSimulation::~RtsSimulation() = default;

} // namespace rts::gameplay
