#include <RTSEngine/Rts/ScaleBenchmark.h>

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using namespace rts::gameplay;

void printFailure(const ScaleBenchmarkReport& report) {
    if (report.budgetPassed()) return;
    WriteScaleBenchmarkJson(std::cerr, report);
    std::cerr << '\n';
}

void assertBudget(const ScaleBenchmarkReport& report) {
    printFailure(report);
    const auto proportionalSearches = report.agents / 100u;
    const auto maximumPathSearches =
        proportionalSearches < 8u ? 8u : proportionalSearches;
    const auto maximumExpandedNodes =
        static_cast<std::uint64_t>(report.agents) * 4u;

    assert(report.elapsedMicroseconds >= 0);
    assert(report.averageTickMicroseconds >= 0);
    assert(report.p95TickMicroseconds >= 0);
    assert(report.maximumTickMicroseconds >= report.p95TickMicroseconds);
    assert(report.budgetPassed());
    assert(report.budgetFailures == ScaleBudgetNone);
    assert(report.finalWorldHash != 0);
    assert(report.finalUnitCount == report.agents);
    assert(report.uniqueFinalPositions == report.agents);
    assert(report.reservationCapacityStable);
    assert(report.flow.builds == 1);
    assert(report.flow.extractionFailures == 0);
    assert(report.flow.pathExtractions >= report.agents);
    assert(report.flow.extractedPathPoints >= report.agents);
    assert(report.paths.searches <= maximumPathSearches);
    assert(report.paths.expandedNodes <= maximumExpandedNodes);
    assert(report.telemetry.pathFailures == 0);
    assert(report.telemetry.flowAssignments >= report.agents);
    assert(report.telemetry.peakCellOccupancy == 1);
    assert(report.telemetry.peakMovementIntents <= report.agents);
}

void assertDeterministic(
    const ScaleBenchmarkReport& first,
    const ScaleBenchmarkReport& second) {
    assert(first.scenario == second.scenario);
    assert(first.agents == second.agents);
    assert(first.ticks == second.ticks);
    assert(first.finalWorldHash == second.finalWorldHash);
    assert(first.finalUnitCount == second.finalUnitCount);
    assert(first.uniqueFinalPositions == second.uniqueFinalPositions);
    assert(first.reservationCapacityStable ==
           second.reservationCapacityStable);
    assert(first.reservationIntentCapacity ==
           second.reservationIntentCapacity);
    assert(first.reservationResolutionCapacity ==
           second.reservationResolutionCapacity);
    assert(first.reservationRejectedCapacity ==
           second.reservationRejectedCapacity);
    assert(first.flow.hits == second.flow.hits);
    assert(first.flow.misses == second.flow.misses);
    assert(first.flow.builds == second.flow.builds);
    assert(first.flow.visitedCells == second.flow.visitedCells);
    assert(first.flow.pathExtractions == second.flow.pathExtractions);
    assert(first.flow.extractedPathPoints ==
           second.flow.extractedPathPoints);
    assert(first.flow.extractionFailures ==
           second.flow.extractionFailures);
    assert(first.paths.hits == second.paths.hits);
    assert(first.paths.misses == second.paths.misses);
    assert(first.paths.searches == second.paths.searches);
    assert(first.paths.expandedNodes == second.paths.expandedNodes);
    assert(first.telemetry.pathRequests == second.telemetry.pathRequests);
    assert(first.telemetry.flowAssignments ==
           second.telemetry.flowAssignments);
    assert(first.telemetry.pathCacheAssignments ==
           second.telemetry.pathCacheAssignments);
    assert(first.telemetry.pathFailures == second.telemetry.pathFailures);
    assert(first.telemetry.movementIntents ==
           second.telemetry.movementIntents);
    assert(first.telemetry.movementAccepted ==
           second.telemetry.movementAccepted);
    assert(first.telemetry.movementRejected ==
           second.telemetry.movementRejected);
    assert(first.telemetry.yieldedMoves ==
           second.telemetry.yieldedMoves);
    assert(first.telemetry.repathRecoveries ==
           second.telemetry.repathRecoveries);
    assert(first.telemetry.peakMovementIntents ==
           second.telemetry.peakMovementIntents);
    assert(first.telemetry.peakOccupiedCells ==
           second.telemetry.peakOccupiedCells);
    assert(first.telemetry.peakCellOccupancy ==
           second.telemetry.peakCellOccupancy);
    assert(first.budgetFailures == second.budgetFailures);
}

void testScenario(const ScaleBenchmarkConfig& config) {
    const auto first = RunScaleBenchmark(config);
    const auto second = RunScaleBenchmark(config);
    assertBudget(first);
    assertBudget(second);
    assertDeterministic(first, second);

    std::ostringstream json;
    WriteScaleBenchmarkJson(json, first);
    const auto value = json.str();
    assert(!value.empty());
    assert(value.front() == '{');
    assert(value.back() == '}');
    assert(value.find("\"scenario\":\"") != std::string::npos);
    assert(value.find("\"budget_passed\":true") != std::string::npos);
    assert(value.find("\"p95_tick_us\":") != std::string::npos);
}

} // namespace

int main() {
    testScenario(Make1000AgentBenchmark(48));
    testScenario(Make2500AgentBenchmark(24));
    std::cout << "scale telemetry regression tests passed\n";
    return 0;
}
