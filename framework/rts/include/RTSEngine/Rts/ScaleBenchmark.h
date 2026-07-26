#pragma once

#include <RTSEngine/Rts/Simulation.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace rts::gameplay {

struct ScaleBenchmarkConfig final {
    std::string name;
    std::int32_t width{};
    std::int32_t height{};
    std::int32_t startColumns{};
    std::int32_t startRows{};
    std::int32_t wallX{};
    std::int32_t gapMinimumY{};
    std::int32_t gapMaximumY{};
    GridPoint goal{};
    std::uint64_t ticks{};

    std::uint32_t agentCount() const noexcept {
        return static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(startColumns) *
            static_cast<std::uint64_t>(startRows));
    }
};

inline ScaleBenchmarkConfig Make1000AgentBenchmark(
    std::uint64_t ticks = 64) {
    return {
        "shared-goal-1000",
        64,
        64,
        40,
        25,
        48,
        30,
        34,
        {63, 63},
        ticks};
}

inline ScaleBenchmarkConfig Make2500AgentBenchmark(
    std::uint64_t ticks = 48) {
    return {
        "shared-goal-2500",
        80,
        80,
        50,
        50,
        62,
        36,
        43,
        {79, 79},
        ticks};
}

enum ScaleBudgetFailure : std::uint32_t {
    ScaleBudgetNone = 0,
    ScaleBudgetZeroWorldHash = 1u << 0u,
    ScaleBudgetUnitCountMismatch = 1u << 1u,
    ScaleBudgetPositionOverlap = 1u << 2u,
    ScaleBudgetFlowBuilds = 1u << 3u,
    ScaleBudgetFlowExtractionFailure = 1u << 4u,
    ScaleBudgetPathSearches = 1u << 5u,
    ScaleBudgetExpandedNodes = 1u << 6u,
    ScaleBudgetPathFailure = 1u << 7u,
    ScaleBudgetPeakCellOccupancy = 1u << 8u,
    ScaleBudgetReservationGrowth = 1u << 9u,
    ScaleBudgetFlowAssignments = 1u << 10u,
    ScaleBudgetPeakIntents = 1u << 11u
};

struct ScaleRegressionBudget final {
    std::uint64_t maximumFlowBuilds{1};
    std::uint64_t maximumFlowExtractionFailures{};
    std::uint64_t maximumPathSearches{};
    std::uint64_t maximumExpandedNodes{};
    std::uint64_t maximumPathFailures{};
    std::uint32_t maximumPeakCellOccupancy{1};
    std::uint32_t maximumPeakMovementIntents{};
    std::uint64_t minimumFlowAssignments{};
    bool requireStableReservationCapacity{true};
    bool requireUniquePositions{true};
};

inline ScaleRegressionBudget MakeScaleRegressionBudget(
    const ScaleBenchmarkConfig& config) {
    const auto agents = config.agentCount();
    ScaleRegressionBudget budget;
    budget.maximumPathSearches = std::max<std::uint64_t>(
        8u, static_cast<std::uint64_t>(agents) / 100u);
    budget.maximumExpandedNodes =
        static_cast<std::uint64_t>(agents) * 8u;
    budget.maximumPeakMovementIntents = agents;
    budget.minimumFlowAssignments = agents;
    return budget;
}

struct ScaleBenchmarkReport final {
    std::string scenario;
    std::uint32_t agents{};
    std::uint64_t ticks{};
    std::uint64_t finalWorldHash{};
    std::int64_t elapsedMicroseconds{};
    std::int64_t averageTickMicroseconds{};
    std::int64_t p95TickMicroseconds{};
    std::int64_t maximumTickMicroseconds{};
    std::uint32_t finalUnitCount{};
    std::uint32_t uniqueFinalPositions{};
    bool reservationCapacityStable{};
    std::size_t reservationIntentCapacity{};
    std::size_t reservationResolutionCapacity{};
    std::size_t reservationRejectedCapacity{};
    GridFlowFieldStats flow;
    GridPathCacheStats paths;
    RuntimeTelemetryTotals telemetry;
    std::uint32_t budgetFailures{};

    bool budgetPassed() const noexcept {
        return budgetFailures == ScaleBudgetNone;
    }
};

inline std::uint32_t ValidateScaleBenchmark(
    const ScaleBenchmarkReport& report,
    const ScaleRegressionBudget& budget) noexcept {
    std::uint32_t failures = ScaleBudgetNone;
    if (report.finalWorldHash == 0) failures |= ScaleBudgetZeroWorldHash;
    if (report.finalUnitCount != report.agents) {
        failures |= ScaleBudgetUnitCountMismatch;
    }
    if (budget.requireUniquePositions &&
        report.uniqueFinalPositions != report.finalUnitCount) {
        failures |= ScaleBudgetPositionOverlap;
    }
    if (report.flow.builds > budget.maximumFlowBuilds) {
        failures |= ScaleBudgetFlowBuilds;
    }
    if (report.flow.extractionFailures >
        budget.maximumFlowExtractionFailures) {
        failures |= ScaleBudgetFlowExtractionFailure;
    }
    if (report.paths.searches > budget.maximumPathSearches) {
        failures |= ScaleBudgetPathSearches;
    }
    if (report.paths.expandedNodes > budget.maximumExpandedNodes) {
        failures |= ScaleBudgetExpandedNodes;
    }
    if (report.telemetry.pathFailures > budget.maximumPathFailures) {
        failures |= ScaleBudgetPathFailure;
    }
    if (report.telemetry.peakCellOccupancy >
        budget.maximumPeakCellOccupancy) {
        failures |= ScaleBudgetPeakCellOccupancy;
    }
    if (budget.requireStableReservationCapacity &&
        !report.reservationCapacityStable) {
        failures |= ScaleBudgetReservationGrowth;
    }
    if (report.telemetry.flowAssignments < budget.minimumFlowAssignments) {
        failures |= ScaleBudgetFlowAssignments;
    }
    if (report.telemetry.peakMovementIntents >
        budget.maximumPeakMovementIntents) {
        failures |= ScaleBudgetPeakIntents;
    }
    return failures;
}

inline ScaleBenchmarkReport RunScaleBenchmark(
    const ScaleBenchmarkConfig& config,
    ScaleRegressionBudget budget) {
    ScaleBenchmarkReport report;
    report.scenario = config.name;
    report.agents = config.agentCount();
    report.ticks = config.ticks;

    RtsSimulation simulation(config.width, config.height);
    simulation.reserveMovementAgents(report.agents);
    const auto initialIntentCapacity =
        simulation.movementReservations().intentCapacity();
    const auto initialResolutionCapacity =
        simulation.movementReservations().resolutionCapacity();
    const auto initialRejectedCapacity =
        simulation.movementReservations().rejectedCapacity();
    report.reservationCapacityStable =
        initialIntentCapacity >= report.agents &&
        initialResolutionCapacity >= report.agents &&
        initialRejectedCapacity >= report.agents;

    for (std::int32_t y = 0; y < config.height; ++y) {
        if (y >= config.gapMinimumY && y <= config.gapMaximumY) continue;
        if (!simulation.setBlocked({config.wallX, y}, true)) {
            report.budgetFailures = ScaleBudgetFlowBuilds;
            return report;
        }
    }

    std::uint32_t sequence = 1;
    for (std::int32_t y = 0; y < config.startRows; ++y) {
        for (std::int32_t x = 0; x < config.startColumns; ++x) {
            const auto unit = simulation.createUnit({x, y}, {1});
            TickCommand move;
            move.targetTick = 0;
            move.issuer = 1;
            move.sequence = sequence++;
            move.type = CommandType::Move;
            move.subject = unit;
            move.targetX = config.goal.x;
            move.targetY = config.goal.y;
            if (!simulation.submit(move)) {
                report.budgetFailures = ScaleBudgetUnitCountMismatch;
                return report;
            }
        }
    }

    std::vector<std::int64_t> tickTimes;
    tickTimes.reserve(static_cast<std::size_t>(config.ticks));
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t tick = 0; tick < config.ticks; ++tick) {
        const auto tickStarted = std::chrono::steady_clock::now();
        simulation.step(tick);
        const auto tickElapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - tickStarted).count();
        tickTimes.push_back(tickElapsed);

        report.reservationCapacityStable =
            report.reservationCapacityStable &&
            simulation.movementReservations().intentCapacity() ==
                initialIntentCapacity &&
            simulation.movementReservations().resolutionCapacity() ==
                initialResolutionCapacity &&
            simulation.movementReservations().rejectedCapacity() ==
                initialRejectedCapacity;
    }
    report.elapsedMicroseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();

    if (!tickTimes.empty()) {
        std::sort(tickTimes.begin(), tickTimes.end());
        std::int64_t total = 0;
        for (const auto value : tickTimes) total += value;
        report.averageTickMicroseconds =
            total / static_cast<std::int64_t>(tickTimes.size());
        const auto p95Index = std::min<std::size_t>(
            tickTimes.size() - 1,
            (tickTimes.size() * 95u + 99u) / 100u - 1u);
        report.p95TickMicroseconds = tickTimes[p95Index];
        report.maximumTickMicroseconds = tickTimes.back();
    }

    report.finalWorldHash = simulation.snapshot().worldHash;
    report.flow = simulation.flowFields().stats();
    report.paths = simulation.pathCache().stats();
    report.telemetry = simulation.telemetry().totals();
    report.reservationIntentCapacity =
        simulation.movementReservations().intentCapacity();
    report.reservationResolutionCapacity =
        simulation.movementReservations().resolutionCapacity();
    report.reservationRejectedCapacity =
        simulation.movementReservations().rejectedCapacity();

    std::vector<std::uint64_t> positions;
    positions.reserve(simulation.snapshot().entities.size());
    for (const auto& entity : simulation.snapshot().entities) {
        if (entity.kind != SnapshotKind::Unit) continue;
        ++report.finalUnitCount;
        positions.push_back(
            static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(entity.y)) *
                static_cast<std::uint64_t>(config.width) +
            static_cast<std::uint32_t>(entity.x));
    }
    std::sort(positions.begin(), positions.end());
    report.uniqueFinalPositions = static_cast<std::uint32_t>(
        std::unique(positions.begin(), positions.end()) - positions.begin());
    report.budgetFailures = ValidateScaleBenchmark(report, budget);
    return report;
}

inline ScaleBenchmarkReport RunScaleBenchmark(
    const ScaleBenchmarkConfig& config) {
    return RunScaleBenchmark(config, MakeScaleRegressionBudget(config));
}

inline void WriteScaleBenchmarkJson(
    std::ostream& output,
    const ScaleBenchmarkReport& report) {
    output << '{'
           << "\"scenario\":\"" << report.scenario << "\"," 
           << "\"agents\":" << report.agents << ','
           << "\"ticks\":" << report.ticks << ','
           << "\"final_world_hash\":" << report.finalWorldHash << ','
           << "\"elapsed_us\":" << report.elapsedMicroseconds << ','
           << "\"average_tick_us\":" << report.averageTickMicroseconds << ','
           << "\"p95_tick_us\":" << report.p95TickMicroseconds << ','
           << "\"maximum_tick_us\":" << report.maximumTickMicroseconds << ','
           << "\"final_units\":" << report.finalUnitCount << ','
           << "\"unique_positions\":" << report.uniqueFinalPositions << ','
           << "\"reservation_capacity_stable\":"
           << (report.reservationCapacityStable ? "true" : "false") << ','
           << "\"flow_builds\":" << report.flow.builds << ','
           << "\"flow_visited_cells\":" << report.flow.visitedCells << ','
           << "\"flow_path_extractions\":" << report.flow.pathExtractions << ','
           << "\"flow_extracted_points\":" << report.flow.extractedPathPoints << ','
           << "\"flow_extraction_failures\":"
           << report.flow.extractionFailures << ','
           << "\"path_searches\":" << report.paths.searches << ','
           << "\"path_expanded_nodes\":" << report.paths.expandedNodes << ','
           << "\"navigation_path_requests\":"
           << report.telemetry.pathRequests << ','
           << "\"navigation_flow_assignments\":"
           << report.telemetry.flowAssignments << ','
           << "\"movement_intents\":"
           << report.telemetry.movementIntents << ','
           << "\"movement_accepted\":"
           << report.telemetry.movementAccepted << ','
           << "\"movement_rejected\":"
           << report.telemetry.movementRejected << ','
           << "\"movement_yielded\":"
           << report.telemetry.yieldedMoves << ','
           << "\"movement_repaths\":"
           << report.telemetry.repathRecoveries << ','
           << "\"peak_movement_intents\":"
           << report.telemetry.peakMovementIntents << ','
           << "\"peak_occupied_cells\":"
           << report.telemetry.peakOccupiedCells << ','
           << "\"peak_cell_occupancy\":"
           << report.telemetry.peakCellOccupancy << ','
           << "\"budget_failures\":" << report.budgetFailures << ','
           << "\"budget_passed\":"
           << (report.budgetPassed() ? "true" : "false")
           << '}';
}

} // namespace rts::gameplay
