#include <RTSEngine/Rts/AuthoritativeStateHash.h>
#include <RTSEngine/Rts/ResearchStateHash.h>
#include <RTSEngine/Rts/Simulation.h>
#include <RTSEngine/Rts/TargetAuthorization.h>

#include <algorithm>
#include <vector>

namespace rts::gameplay {
namespace {

struct TargetAuthorizationBinding final {
    const RtsSimulation* simulation{};
    const void* context{};
    TargetAuthorizationCallback callback{};
};

std::vector<TargetAuthorizationBinding>& TargetBindings() {
    static std::vector<TargetAuthorizationBinding> bindings;
    return bindings;
}

const TargetAuthorizationBinding* FindTargetBinding(
    const RtsSimulation& simulation) noexcept {
    const auto& bindings = TargetBindings();
    const auto found = std::find_if(
        bindings.begin(), bindings.end(),
        [&simulation](const TargetAuthorizationBinding& binding) {
            return binding.simulation == &simulation;
        });
    return found == bindings.end() ? nullptr : &*found;
}

} // namespace

void BindRtsTargetAuthorization(
    const RtsSimulation& simulation,
    const void* context,
    TargetAuthorizationCallback callback) noexcept {
    auto& bindings = TargetBindings();
    const auto found = std::find_if(
        bindings.begin(), bindings.end(),
        [&simulation](const TargetAuthorizationBinding& binding) {
            return binding.simulation == &simulation;
        });
    if (!callback) {
        if (found != bindings.end()) bindings.erase(found);
        return;
    }
    if (found != bindings.end()) {
        found->context = context;
        found->callback = callback;
    } else {
        bindings.push_back({&simulation, context, callback});
    }
}

void UnbindRtsTargetAuthorization(
    const RtsSimulation& simulation) noexcept {
    auto& bindings = TargetBindings();
    bindings.erase(
        std::remove_if(
            bindings.begin(), bindings.end(),
            [&simulation](const TargetAuthorizationBinding& binding) {
                return binding.simulation == &simulation;
            }),
        bindings.end());
}

bool IsRtsTargetAuthorized(
    const RtsSimulation& simulation,
    std::uint32_t observerTeam,
    std::uint32_t targetTeam) noexcept {
    const auto* binding = FindTargetBinding(simulation);
    return binding && binding->callback
        ? binding->callback(binding->context, observerTeam, targetTeam)
        : observerTeam != targetTeam;
}

RtsSimulation::RtsSimulation(std::int32_t width, std::int32_t height)
    : navigation_(width, height),
      vision_(width, height),
      influence_(width, height),
      movement_(width, height),
      building_(economy_, navigation_),
      combat_(width, height) {
    combat_.setVisibilityFilter(
        this,
        [](const void* context,
           std::uint32_t observerTeam,
           ecs::Entity target,
           std::int32_t targetX,
           std::int32_t targetY) {
            const auto* simulation =
                static_cast<const RtsSimulation*>(context);
            if (!simulation) return false;
            const auto* targetTeam =
                simulation->world().try_get<Team>(target);
            return targetTeam &&
                   IsRtsTargetAuthorized(
                       *simulation, observerTeam, targetTeam->id) &&
                   simulation->vision().visible(
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
        ecs::Stage::Navigation,
        -30,
        170,
        [this](ecs::World& world, const ecs::SystemContext&) {
            world.eachRef<Team, CombatDirective, CombatTarget, MovementAgent>(
                [this, &world](
                    ecs::Entity,
                    Team& team,
                    CombatDirective& directive,
                    CombatTarget& target,
                    MovementAgent& agent) {
                    const auto authorized =
                        [this, &world, teamId = team.id](ecs::Entity entity) {
                            if (!entity.valid() || !world.alive(entity)) {
                                return false;
                            }
                            const auto* targetTeam =
                                world.try_get<Team>(entity);
                            return targetTeam && IsRtsTargetAuthorized(
                                *this, teamId, targetTeam->id);
                        };

                    if (directive.forcedTarget.valid() &&
                        !authorized(directive.forcedTarget)) {
                        directive.forcedTarget = {};
                        directive.mode = CombatMode::Guard;
                        target.entity = {};
                        OrderSystem::clearPath(agent);
                    } else if (target.entity.valid() &&
                               !authorized(target.entity)) {
                        target.entity = {};
                    }
                });
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
                playerTeamId_,
                5u,
                nextResourceNodeId_,
                nextResearchId_,
                HashTechTreeState(tech_),
                HashResearchQueueState(world));
            influenceWorldHash_ = snapshot_.worldHash;
        });
}

RtsSimulation::~RtsSimulation() {
    UnbindRtsTargetAuthorization(*this);
}

} // namespace rts::gameplay
