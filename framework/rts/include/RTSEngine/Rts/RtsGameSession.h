#pragma once

#include <RTSEngine/Rts/AiRuntime.h>
#include <RTSEngine/Rts/Diplomacy.h>
#include <RTSEngine/Rts/Simulation.h>
#include <RTSEngine/Rts/TargetAuthorization.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace rts::gameplay {

class RtsGameSessionArchive;

enum class SessionCommandResult : std::uint8_t {
    Accepted,
    DuplicateIdentity,
    Late,
    Unauthorized,
    AlliedTarget,
    ProducerRestricted,
    QueueFull,
    SupplyBlocked
};

struct ProducerPolicy final {
    std::uint32_t buildingDefinitionId{};
    std::uint32_t queueCapacity{5};
    std::vector<std::uint32_t> allowedUnitDefinitions;
};

struct TeamSupplyLimit final {
    std::uint32_t teamId{};
    std::uint32_t capacity{};
};

class RtsGameSession final {
public:
    RtsGameSession(std::int32_t width = 32, std::int32_t height = 32)
        : simulation_(width, height) {
        BindRtsTargetAuthorization(
            simulation_,
            &diplomacy_,
            [](const void* context,
               std::uint32_t observerTeam,
               std::uint32_t targetTeam) {
                const auto* diplomacy =
                    static_cast<const DiplomacyRuntime*>(context);
                return diplomacy &&
                       diplomacy->hostile(observerTeam, targetTeam);
            });
    }

    bool registerBuilding(BuildingDefinition value) {
        return simulation_.registerBuilding(std::move(value));
    }

    bool registerUnit(UnitDefinition value) {
        return simulation_.registerUnit(std::move(value));
    }

    bool registerProducerPolicy(ProducerPolicy policy) {
        if (simulation_.configurationFrozen() ||
            policy.buildingDefinitionId == 0 || policy.queueCapacity == 0) {
            return false;
        }
        std::sort(
            policy.allowedUnitDefinitions.begin(),
            policy.allowedUnitDefinitions.end());
        policy.allowedUnitDefinitions.erase(
            std::unique(
                policy.allowedUnitDefinitions.begin(),
                policy.allowedUnitDefinitions.end()),
            policy.allowedUnitDefinitions.end());
        const auto found = lowerProducer(policy.buildingDefinitionId);
        if (found != producers_.end() &&
            found->buildingDefinitionId == policy.buildingDefinitionId) {
            *found = std::move(policy);
        } else {
            producers_.insert(found, std::move(policy));
        }
        return true;
    }

    bool setTeamSupplyCapacity(
        std::uint32_t teamId,
        std::uint32_t capacity) {
        if (simulation_.configurationFrozen() || teamId == 0) return false;
        const auto found = lowerSupply(teamId);
        if (found != supply_.end() && found->teamId == teamId) {
            found->capacity = capacity;
        } else {
            supply_.insert(found, TeamSupplyLimit{teamId, capacity});
        }
        return true;
    }

    bool setRelation(
        std::uint32_t firstTeam,
        std::uint32_t secondTeam,
        DiplomaticRelation relation) {
        return !simulation_.configurationFrozen() &&
               diplomacy_.setRelation(firstTeam, secondTeam, relation);
    }

    bool registerAiTeam(
        std::uint32_t teamId,
        GridPoint fallbackObjective,
        std::uint32_t thinkIntervalTicks = 8) {
        return !simulation_.configurationFrozen() &&
               ai_.registerTeam(
                   teamId, fallbackObjective, thinkIntervalTicks);
    }

    void setResources(std::int32_t value) noexcept {
        simulation_.setResources(value);
    }

    bool setTeamResource(
        std::uint32_t teamId,
        ResourceTypeId resourceType,
        ResourceAmount value) {
        return simulation_.setTeamResource(
            teamId, resourceType, value);
    }

    bool setBlocked(GridPoint point, bool blocked) {
        return simulation_.setBlocked(point, blocked);
    }

    bool setRequiredRoute(GridPoint start, GridPoint goal) noexcept {
        return simulation_.setRequiredRoute(start, goal);
    }

    ecs::Entity createUnit(
        Position position,
        MoveSpeed speed,
        std::uint32_t teamId = 1,
        CombatStats combat = {},
        std::int32_t visionRange = 6) {
        return simulation_.createUnit(
            position, speed, teamId, combat, visionRange);
    }

    ecs::Entity createUnitDefinition(
        std::uint32_t definitionId,
        Position position,
        std::uint32_t teamId = 1) {
        return simulation_.createUnitDefinition(
            definitionId, position, teamId);
    }

    ecs::Entity createResourceNode(
        Position position,
        ResourceTypeId resourceType,
        ResourceAmount amount) {
        return simulation_.createResourceNode(
            position, resourceType, amount);
    }

    ResourceAmount resourceAvailable(
        std::uint32_t teamId,
        ResourceTypeId resourceType) const noexcept {
        return simulation_.resourceAvailable(teamId, resourceType);
    }

    SessionCommandResult submitDetailed(TickCommand command) {
        const bool production = command.type == CommandType::Train;
        const TrainReservation reservation{
            command.targetTick,
            command.issuer,
            command.sequence,
            command.subject,
            command.definitionId};

        if (production) {
            const auto existing = findReservation(reservation);
            if (existing != reservations_.end()) {
                return translate(
                    simulation_.submitDetailed(std::move(command)));
            }
        }

        if (command.type == CommandType::Attack) {
            const auto result = validateAttack(command);
            if (result != SessionCommandResult::Accepted) return result;
        }
        if (production) {
            const auto result = validateProduction(command);
            if (result != SessionCommandResult::Accepted) return result;
        }

        const auto result = translate(
            simulation_.submitDetailed(std::move(command)));
        if (production && result == SessionCommandResult::Accepted) {
            insertReservation(reservation);
        }
        return result;
    }

    bool submit(TickCommand command) {
        return submitDetailed(std::move(command)) ==
               SessionCommandResult::Accepted;
    }

    RtsStepResult stepDetailed(std::uint64_t tick) {
        const auto result = simulation_.stepDetailed(tick);
        if (result != RtsStepResult::Advanced) return result;

        reservations_.erase(
            std::remove_if(
                reservations_.begin(),
                reservations_.end(),
                [tick](const TrainReservation& value) {
                    return value.targetTick <= tick;
                }),
            reservations_.end());

        ai_.emitCommands(
            simulation_.world(),
            simulation_.vision(),
            diplomacy_,
            simulation_.nextExpectedTick(),
            [this](TickCommand command) {
                submitDetailed(std::move(command));
            });
        return result;
    }

    bool step(std::uint64_t tick) {
        return stepDetailed(tick) == RtsStepResult::Advanced;
    }

    bool stepNext() {
        return step(simulation_.nextExpectedTick());
    }

    std::uint32_t usedSupply(std::uint32_t teamId) const {
        std::uint32_t used = 0;
        simulation_.world().eachRef<Team, TunableStats>(
            [&](ecs::Entity,
                const Team& team,
                const TunableStats& stats) {
                if (team.id == teamId && !stats.building &&
                    used != std::numeric_limits<std::uint32_t>::max()) {
                    ++used;
                }
            });
        simulation_.world().eachRef<Team, ProductionQueue>(
            [&](ecs::Entity,
                const Team& team,
                const ProductionQueue& queue) {
                if (team.id == teamId) {
                    addSaturated(used, queue.items.size());
                }
            });
        for (const auto& reservation : reservations_) {
            if (reservation.teamId == teamId) {
                addSaturated(used, 1u);
            }
        }
        return used;
    }

    std::uint32_t supplyCapacity(std::uint32_t teamId) const noexcept {
        const auto found = lowerSupply(teamId);
        std::uint32_t capacity =
            found != supply_.end() && found->teamId == teamId
                ? found->capacity
                : std::numeric_limits<std::uint32_t>::max();
        simulation_.world().eachRef<Team, SupplyProvider>(
            [&](ecs::Entity,
                const Team& team,
                const SupplyProvider& provider) {
                if (team.id != teamId) return;
                const auto remaining =
                    std::numeric_limits<std::uint32_t>::max() - capacity;
                capacity += std::min(provider.capacity, remaining);
            });
        return capacity;
    }

    std::size_t pendingTrainReservations() const noexcept {
        return reservations_.size();
    }

    const RtsSimulation& simulation() const noexcept { return simulation_; }
    const DiplomacyRuntime& diplomacy() const noexcept { return diplomacy_; }
    const AiRuntime& ai() const noexcept { return ai_; }

private:
    friend class RtsGameSessionArchive;

    struct TrainReservation final {
        std::uint64_t targetTick{};
        std::uint32_t teamId{};
        std::uint32_t sequence{};
        ecs::Entity producer{};
        std::uint32_t unitDefinitionId{};
    };

    using ProducerIterator = std::vector<ProducerPolicy>::iterator;
    using ProducerConstIterator =
        std::vector<ProducerPolicy>::const_iterator;
    using SupplyIterator = std::vector<TeamSupplyLimit>::iterator;
    using SupplyConstIterator =
        std::vector<TeamSupplyLimit>::const_iterator;
    using ReservationConstIterator =
        std::vector<TrainReservation>::const_iterator;

    static auto reservationIdentity(
        const TrainReservation& value) noexcept {
        return std::tie(
            value.targetTick, value.teamId, value.sequence);
    }

    static bool reservationLess(
        const TrainReservation& first,
        const TrainReservation& second) noexcept {
        return reservationIdentity(first) < reservationIdentity(second);
    }

    ReservationConstIterator findReservation(
        const TrainReservation& value) const noexcept {
        const auto found = std::lower_bound(
            reservations_.begin(),
            reservations_.end(),
            value,
            reservationLess);
        return found != reservations_.end() &&
               reservationIdentity(*found) == reservationIdentity(value)
            ? found
            : reservations_.end();
    }

    void insertReservation(TrainReservation value) {
        reservations_.insert(
            std::lower_bound(
                reservations_.begin(),
                reservations_.end(),
                value,
                reservationLess),
            value);
    }

    std::size_t pendingForProducer(ecs::Entity producer) const noexcept {
        return static_cast<std::size_t>(std::count_if(
            reservations_.begin(),
            reservations_.end(),
            [producer](const TrainReservation& value) {
                return value.producer == producer;
            }));
    }

    static void addSaturated(
        std::uint32_t& value,
        std::size_t additional) noexcept {
        const auto remaining =
            std::numeric_limits<std::uint32_t>::max() - value;
        value += static_cast<std::uint32_t>(
            std::min<std::size_t>(additional, remaining));
    }

    ProducerIterator lowerProducer(std::uint32_t id) noexcept {
        return std::lower_bound(
            producers_.begin(),
            producers_.end(),
            id,
            [](const ProducerPolicy& policy, std::uint32_t value) {
                return policy.buildingDefinitionId < value;
            });
    }

    ProducerConstIterator lowerProducer(std::uint32_t id) const noexcept {
        return std::lower_bound(
            producers_.begin(),
            producers_.end(),
            id,
            [](const ProducerPolicy& policy, std::uint32_t value) {
                return policy.buildingDefinitionId < value;
            });
    }

    SupplyIterator lowerSupply(std::uint32_t teamId) noexcept {
        return std::lower_bound(
            supply_.begin(),
            supply_.end(),
            teamId,
            [](const TeamSupplyLimit& entry, std::uint32_t value) {
                return entry.teamId < value;
            });
    }

    SupplyConstIterator lowerSupply(std::uint32_t teamId) const noexcept {
        return std::lower_bound(
            supply_.begin(),
            supply_.end(),
            teamId,
            [](const TeamSupplyLimit& entry, std::uint32_t value) {
                return entry.teamId < value;
            });
    }

    SessionCommandResult validateAttack(
        const TickCommand& command) const {
        const auto* attacker =
            simulation_.world().try_get<Team>(command.subject);
        const auto* target =
            simulation_.world().try_get<Team>(command.targetEntity);
        if (!attacker || !target || attacker->id != command.issuer) {
            return SessionCommandResult::Unauthorized;
        }
        return diplomacy_.hostile(attacker->id, target->id)
            ? SessionCommandResult::Accepted
            : SessionCommandResult::AlliedTarget;
    }

    SessionCommandResult validateProduction(
        const TickCommand& command) const {
        const auto* team =
            simulation_.world().try_get<Team>(command.subject);
        const auto* building =
            simulation_.world().try_get<Building>(command.subject);
        const auto* queue =
            simulation_.world().try_get<ProductionQueue>(command.subject);
        if (!team || !building || !queue ||
            team->id != command.issuer) {
            return SessionCommandResult::Unauthorized;
        }
        const auto policy = lowerProducer(building->definitionId);
        if (policy == producers_.end() ||
            policy->buildingDefinitionId != building->definitionId ||
            !std::binary_search(
                policy->allowedUnitDefinitions.begin(),
                policy->allowedUnitDefinitions.end(),
                command.definitionId)) {
            return SessionCommandResult::ProducerRestricted;
        }
        if (queue->items.size() + pendingForProducer(command.subject) >=
            policy->queueCapacity) {
            return SessionCommandResult::QueueFull;
        }
        if (usedSupply(team->id) >= supplyCapacity(team->id)) {
            return SessionCommandResult::SupplyBlocked;
        }
        return SessionCommandResult::Accepted;
    }

    static SessionCommandResult translate(
        sim::CommandSubmitResult result) noexcept {
        switch (result) {
        case sim::CommandSubmitResult::Accepted:
            return SessionCommandResult::Accepted;
        case sim::CommandSubmitResult::DuplicateIdentity:
            return SessionCommandResult::DuplicateIdentity;
        case sim::CommandSubmitResult::Late:
            return SessionCommandResult::Late;
        case sim::CommandSubmitResult::Unauthorized:
            return SessionCommandResult::Unauthorized;
        }
        return SessionCommandResult::Unauthorized;
    }

    RtsSimulation simulation_;
    DiplomacyRuntime diplomacy_;
    AiRuntime ai_;
    std::vector<ProducerPolicy> producers_;
    std::vector<TeamSupplyLimit> supply_;
    std::vector<TrainReservation> reservations_;
};

} // namespace rts::gameplay
