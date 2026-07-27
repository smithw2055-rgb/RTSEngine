#pragma once

#include <RTSEngine/Rts/AiRuntime.h>
#include <RTSEngine/Rts/Diplomacy.h>
#include <RTSEngine/Rts/Simulation.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace rts::gameplay {

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
        : simulation_(width, height) {}

    bool registerBuilding(BuildingDefinition definition) {
        return simulation_.registerBuilding(std::move(definition));
    }

    bool registerUnit(UnitDefinition definition) {
        return simulation_.registerUnit(std::move(definition));
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
            return true;
        }
        producers_.insert(found, std::move(policy));
        return true;
    }

    bool setTeamSupplyCapacity(
        std::uint32_t teamId,
        std::uint32_t capacity) {
        if (simulation_.configurationFrozen() || teamId == 0) return false;
        const auto found = lowerSupply(teamId);
        if (found != supply_.end() && found->teamId == teamId) {
            found->capacity = capacity;
            return true;
        }
        supply_.insert(found, TeamSupplyLimit{teamId, capacity});
        return true;
    }

    bool setRelation(
        std::uint32_t firstTeam,
        std::uint32_t secondTeam,
        DiplomaticRelation relation) {
        if (simulation_.configurationFrozen()) return false;
        return diplomacy_.setRelation(firstTeam, secondTeam, relation);
    }

    bool registerAiTeam(
        std::uint32_t teamId,
        GridPoint fallbackObjective,
        std::uint32_t thinkIntervalTicks = 8) {
        if (simulation_.configurationFrozen()) return false;
        return ai_.registerTeam(
            teamId, fallbackObjective, thinkIntervalTicks);
    }

    void setResources(std::int32_t available) noexcept {
        simulation_.setResources(available);
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

    SessionCommandResult submitDetailed(TickCommand command) {
        if (command.type == CommandType::Attack) {
            const auto attackResult = validateAttack(command);
            if (attackResult != SessionCommandResult::Accepted) {
                return attackResult;
            }
        }
        if (command.type == CommandType::Train) {
            const auto productionResult = validateProduction(command);
            if (productionResult != SessionCommandResult::Accepted) {
                return productionResult;
            }
        }

        const bool production = command.type == CommandType::Train;
        const TrainReservation reservation{
            command.targetTick,
            command.issuer,
            command.sequence,
            command.subject};
        const auto streamResult =
            simulation_.submitDetailed(std::move(command));
        const auto result = translate(streamResult);
        if (production && result == SessionCommandResult::Accepted &&
            !hasReservation(reservation)) {
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
                reservations_.begin(), reservations_.end(),
                [tick](const TrainReservation& reservation) {
                    return reservation.targetTick <= tick;
                }),
            reservations_.end());

        const auto nextTick = simulation_.nextExpectedTick();
        ai_.emitCommands(
            simulation_.world(),
            simulation_.vision(),
            diplomacy_,
            nextTick,
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
                if (team.id != teamId) return;
                addSaturated(used, queue.items.size());
            });
        for (const auto& reservation : reservations_) {
            if (reservation.teamId == teamId) addSaturated(used, 1u);
        }
        return used;
    }

    std::uint32_t supplyCapacity(std::uint32_t teamId) const noexcept {
        const auto found = lowerSupply(teamId);
        return found != supply_.end() && found->teamId == teamId
            ? found->capacity
            : std::numeric_limits<std::uint32_t>::max();
    }

    std::size_t pendingTrainReservations() const noexcept {
        return reservations_.size();
    }

    const RtsSimulation& simulation() const noexcept { return simulation_; }
    const DiplomacyRuntime& diplomacy() const noexcept { return diplomacy_; }
    const AiRuntime& ai() const noexcept { return ai_; }

private:
    struct TrainReservation final {
        std::uint64_t targetTick{};
        std::uint32_t teamId{};
        std::uint32_t sequence{};
        ecs::Entity producer{};
    };

    static auto reservationKey(const TrainReservation& reservation) noexcept {
        return std::tie(
            reservation.targetTick,
            reservation.teamId,
            reservation.sequence,
            reservation.producer);
    }

    static bool reservationLess(
        const TrainReservation& first,
        const TrainReservation& second) noexcept {
        return reservationKey(first) < reservationKey(second);
    }

    bool hasReservation(const TrainReservation& reservation) const noexcept {
        const auto found = std::lower_bound(
            reservations_.begin(), reservations_.end(),
            reservation, reservationLess);
        return found != reservations_.end() &&
               !reservationLess(reservation, *found) &&
               !reservationLess(*found, reservation);
    }

    void insertReservation(TrainReservation reservation) {
        reservations_.insert(
            std::lower_bound(
                reservations_.begin(), reservations_.end(),
                reservation, reservationLess),
            reservation);
    }

    std::size_t pendingForProducer(ecs::Entity producer) const noexcept {
        return static_cast<std::size_t>(std::count_if(
            reservations_.begin(), reservations_.end(),
            [producer](const TrainReservation& reservation) {
                return reservation.producer == producer;
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

    auto lowerProducer(std::uint32_t definitionId) noexcept {
        return std::lower_bound(
            producers_.begin(), producers_.end(), definitionId,
            [](const ProducerPolicy& policy, std::uint32_t value) {
                return policy.buildingDefinitionId < value;
            });
    }

    auto lowerProducer(std::uint32_t definitionId) const noexcept {
        return std::lower_bound(
            producers_.begin(), producers_.end(), definitionId,
            [](const ProducerPolicy& policy, std::uint32_t value) {
                return policy.buildingDefinitionId < value;
            });
    }

    auto lowerSupply(std::uint32_t teamId) noexcept {
        return std::lower_bound(
            supply_.begin(), supply_.end(), teamId,
            [](const TeamSupplyLimit& entry, std::uint32_t value) {
                return entry.teamId < value;
            });
    }

    auto lowerSupply(std::uint32_t teamId) const noexcept {
        return std::lower_bound(
            supply_.begin(), supply_.end(), teamId,
            [](const TeamSupplyLimit& entry, std::uint32_t value) {
                return entry.teamId < value;
            });
    }

    SessionCommandResult validateAttack(const TickCommand& command) const {
        const auto* attackerTeam =
            simulation_.world().try_get<Team>(command.subject);
        const auto* targetTeam =
            simulation_.world().try_get<Team>(command.targetEntity);
        if (!attackerTeam || !targetTeam ||
            attackerTeam->id != command.issuer) {
            return SessionCommandResult::Unauthorized;
        }
        return diplomacy_.hostile(attackerTeam->id, targetTeam->id)
            ? SessionCommandResult::Accepted
            : SessionCommandResult::AlliedTarget;
    }

    SessionCommandResult validateProduction(const TickCommand& command) const {
        const auto* team = simulation_.world().try_get<Team>(command.subject);
        const auto* building =
            simulation_.world().try_get<Building>(command.subject);
        const auto* queue =
            simulation_.world().try_get<ProductionQueue>(command.subject);
        if (!team || !building || !queue || team->id != command.issuer) {
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
