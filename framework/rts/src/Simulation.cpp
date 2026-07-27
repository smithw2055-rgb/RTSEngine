#include <RTSEngine/Rts/AuthoritativeStateHash.h>
#include <RTSEngine/Rts/Simulation.h>

namespace rts::gameplay {

RtsSimulation::RtsSimulation(std::int32_t width, std::int32_t height)
    : navigation_(width, height),
      vision_(width, height),
      influence_(width, height),
      movement_(width, height),
      building_(economies_, navigation_),
      combat_(width, height) {
    economies_.ensure(playerTeamId_);
    combat_.setVisibilityFilter(
        &vision_,
        [](const void* context,
           std::uint32_t observerTeam,
           ecs::Entity,
           std::int32_t targetX,
           std::int32_t targetY) {
            const auto* vision = static_cast<const VisionRuntime*>(context);
            return vision && vision->visible(
                observerTeam, {targetX, targetY});
        });

    installSystems();
    scheduler_.add(
        ecs::Stage::Command,
        -100,
        50,
        [this](ecs::World& world, const ecs::SystemContext&) {
            VisionSystem::run(world, navigation_, vision_);
        });
    scheduler_.add(
        ecs::Stage::Combat,
        -100,
        340,
        [this](ecs::World& world, const ecs::SystemContext&) {
            VisionSystem::run(world, navigation_, vision_);
        });
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
