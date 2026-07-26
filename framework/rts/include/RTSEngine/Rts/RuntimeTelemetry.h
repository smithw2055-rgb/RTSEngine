#pragma once

#include <algorithm>
#include <cstdint>

namespace rts::gameplay {

struct NavigationTickTelemetry final {
    std::uint64_t tick{};
    std::uint32_t agents{};
    std::uint32_t pathRequests{};
    std::uint32_t demandGroups{};
    std::uint32_t sharedDemandGroups{};
    std::uint32_t peakDemandPerGoal{};
    std::uint32_t flowAssignments{};
    std::uint32_t pathCacheAssignments{};
    std::uint32_t attackAssignments{};
    std::uint32_t pathFailures{};
    std::uint64_t assignedPathPoints{};
};

struct MovementTickTelemetry final {
    std::uint64_t tick{};
    std::uint32_t agents{};
    std::uint32_t substeps{};
    std::uint32_t occupancyRebuilds{};
    std::uint32_t peakOccupiedCells{};
    std::uint32_t peakCellOccupancy{};
    std::uint32_t intents{};
    std::uint32_t accepted{};
    std::uint32_t rejected{};
    std::uint32_t blockedSignals{};
    std::uint32_t yieldedMoves{};
    std::uint32_t repathRecoveries{};
    std::uint32_t completedMoves{};
};

struct RuntimeTelemetryTotals final {
    std::uint64_t navigationTicks{};
    std::uint64_t movementTicks{};
    std::uint64_t navigationAgents{};
    std::uint64_t pathRequests{};
    std::uint64_t demandGroups{};
    std::uint64_t sharedDemandGroups{};
    std::uint64_t flowAssignments{};
    std::uint64_t pathCacheAssignments{};
    std::uint64_t attackAssignments{};
    std::uint64_t pathFailures{};
    std::uint64_t assignedPathPoints{};
    std::uint64_t movementAgents{};
    std::uint64_t movementSubsteps{};
    std::uint64_t occupancyRebuilds{};
    std::uint64_t movementIntents{};
    std::uint64_t movementAccepted{};
    std::uint64_t movementRejected{};
    std::uint64_t blockedSignals{};
    std::uint64_t yieldedMoves{};
    std::uint64_t repathRecoveries{};
    std::uint64_t completedMoves{};
    std::uint32_t peakDemandPerGoal{};
    std::uint32_t peakPathRequestsPerTick{};
    std::uint32_t peakMovementAgents{};
    std::uint32_t peakMovementIntents{};
    std::uint32_t peakMovementRejected{};
    std::uint32_t peakOccupiedCells{};
    std::uint32_t peakCellOccupancy{};
};

class RuntimeTelemetry final {
public:
    void reset() noexcept {
        lastNavigation_ = {};
        lastMovement_ = {};
        totals_ = {};
    }

    void beginNavigationTick(std::uint64_t tick) noexcept {
        lastNavigation_ = {};
        lastNavigation_.tick = tick;
        ++totals_.navigationTicks;
    }

    void recordNavigationAgent() noexcept {
        ++lastNavigation_.agents;
        ++totals_.navigationAgents;
    }

    void recordPathRequest() noexcept {
        ++lastNavigation_.pathRequests;
        ++totals_.pathRequests;
        totals_.peakPathRequestsPerTick = std::max(
            totals_.peakPathRequestsPerTick,
            lastNavigation_.pathRequests);
    }

    void recordDemandGroup(
        std::uint32_t demand,
        bool shared) noexcept {
        ++lastNavigation_.demandGroups;
        ++totals_.demandGroups;
        if (shared) {
            ++lastNavigation_.sharedDemandGroups;
            ++totals_.sharedDemandGroups;
        }
        lastNavigation_.peakDemandPerGoal = std::max(
            lastNavigation_.peakDemandPerGoal, demand);
        totals_.peakDemandPerGoal = std::max(
            totals_.peakDemandPerGoal, demand);
    }

    void recordFlowAssignment(std::uint64_t pathPoints) noexcept {
        ++lastNavigation_.flowAssignments;
        ++totals_.flowAssignments;
        recordAssignedPathPoints(pathPoints);
    }

    void recordPathCacheAssignment(std::uint64_t pathPoints) noexcept {
        ++lastNavigation_.pathCacheAssignments;
        ++totals_.pathCacheAssignments;
        recordAssignedPathPoints(pathPoints);
    }

    void recordAttackAssignment(std::uint64_t pathPoints) noexcept {
        ++lastNavigation_.attackAssignments;
        ++totals_.attackAssignments;
        recordAssignedPathPoints(pathPoints);
    }

    void recordPathFailure() noexcept {
        ++lastNavigation_.pathFailures;
        ++totals_.pathFailures;
    }

    void beginMovementTick(std::uint64_t tick) noexcept {
        lastMovement_ = {};
        lastMovement_.tick = tick;
        ++totals_.movementTicks;
    }

    void recordMovementAgent() noexcept {
        ++lastMovement_.agents;
        ++totals_.movementAgents;
        totals_.peakMovementAgents = std::max(
            totals_.peakMovementAgents,
            lastMovement_.agents);
    }

    void recordMovementSubstep() noexcept {
        ++lastMovement_.substeps;
        ++totals_.movementSubsteps;
    }

    void recordOccupancyRebuild(
        std::uint32_t occupiedCells,
        std::uint32_t maximumCellOccupancy) noexcept {
        ++lastMovement_.occupancyRebuilds;
        ++totals_.occupancyRebuilds;
        lastMovement_.peakOccupiedCells = std::max(
            lastMovement_.peakOccupiedCells, occupiedCells);
        lastMovement_.peakCellOccupancy = std::max(
            lastMovement_.peakCellOccupancy, maximumCellOccupancy);
        totals_.peakOccupiedCells = std::max(
            totals_.peakOccupiedCells, occupiedCells);
        totals_.peakCellOccupancy = std::max(
            totals_.peakCellOccupancy, maximumCellOccupancy);
    }

    void recordReservationBatch(
        std::uint32_t intents,
        std::uint32_t accepted,
        std::uint32_t rejected) noexcept {
        lastMovement_.intents += intents;
        lastMovement_.accepted += accepted;
        lastMovement_.rejected += rejected;
        totals_.movementIntents += intents;
        totals_.movementAccepted += accepted;
        totals_.movementRejected += rejected;
        totals_.peakMovementIntents = std::max(
            totals_.peakMovementIntents,
            lastMovement_.intents);
        totals_.peakMovementRejected = std::max(
            totals_.peakMovementRejected,
            lastMovement_.rejected);
    }

    void recordBlockedSignal() noexcept {
        ++lastMovement_.blockedSignals;
        ++totals_.blockedSignals;
    }

    void recordYieldedMove() noexcept {
        ++lastMovement_.yieldedMoves;
        ++totals_.yieldedMoves;
    }

    void recordRepathRecovery() noexcept {
        ++lastMovement_.repathRecoveries;
        ++totals_.repathRecoveries;
    }

    void recordCompletedMove() noexcept {
        ++lastMovement_.completedMoves;
        ++totals_.completedMoves;
    }

    const NavigationTickTelemetry& lastNavigation() const noexcept {
        return lastNavigation_;
    }

    const MovementTickTelemetry& lastMovement() const noexcept {
        return lastMovement_;
    }

    const RuntimeTelemetryTotals& totals() const noexcept {
        return totals_;
    }

private:
    void recordAssignedPathPoints(std::uint64_t pathPoints) noexcept {
        lastNavigation_.assignedPathPoints += pathPoints;
        totals_.assignedPathPoints += pathPoints;
    }

    NavigationTickTelemetry lastNavigation_;
    MovementTickTelemetry lastMovement_;
    RuntimeTelemetryTotals totals_;
};

} // namespace rts::gameplay
