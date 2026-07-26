#include <RTSEngine/Rts/ScaleBenchmark.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace rts::gameplay;

void printHuman(const ScaleBenchmarkReport& report) {
    std::cout << "RTSEngine scale benchmark: " << report.scenario << '\n';
    std::cout << "agents=" << report.agents << '\n';
    std::cout << "ticks=" << report.ticks << '\n';
    std::cout << "final_world_hash=" << report.finalWorldHash << '\n';
    std::cout << "elapsed_us=" << report.elapsedMicroseconds << '\n';
    std::cout << "average_tick_us=" << report.averageTickMicroseconds << '\n';
    std::cout << "p95_tick_us=" << report.p95TickMicroseconds << '\n';
    std::cout << "maximum_tick_us=" << report.maximumTickMicroseconds << '\n';
    std::cout << "flow_builds=" << report.flow.builds << '\n';
    std::cout << "flow_visited_cells=" << report.flow.visitedCells << '\n';
    std::cout << "flow_path_extractions=" << report.flow.pathExtractions << '\n';
    std::cout << "flow_extracted_points="
              << report.flow.extractedPathPoints << '\n';
    std::cout << "path_searches=" << report.paths.searches << '\n';
    std::cout << "path_expanded_nodes=" << report.paths.expandedNodes << '\n';
    std::cout << "movement_intents="
              << report.telemetry.movementIntents << '\n';
    std::cout << "movement_accepted="
              << report.telemetry.movementAccepted << '\n';
    std::cout << "movement_rejected="
              << report.telemetry.movementRejected << '\n';
    std::cout << "movement_yielded="
              << report.telemetry.yieldedMoves << '\n';
    std::cout << "movement_repaths="
              << report.telemetry.repathRecoveries << '\n';
    std::cout << "peak_occupied_cells="
              << report.telemetry.peakOccupiedCells << '\n';
    std::cout << "peak_cell_occupancy="
              << report.telemetry.peakCellOccupancy << '\n';
    std::cout << "reservation_capacity_stable="
              << (report.reservationCapacityStable ? "true" : "false")
              << '\n';
    std::cout << "budget_failures=" << report.budgetFailures << '\n';
    std::cout << "budget_passed="
              << (report.budgetPassed() ? "true" : "false") << '\n';
}

bool parseTicks(const std::string& value, std::uint64_t& ticks) {
    if (value.empty()) return false;
    char* end = nullptr;
    const auto parsed = std::strtoull(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed == 0) return false;
    ticks = static_cast<std::uint64_t>(parsed);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool json = false;
    std::string agents = "1000";
    std::uint64_t tickOverride = 0;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--json") {
            json = true;
        } else if (argument.rfind("--agents=", 0) == 0) {
            agents = argument.substr(9);
        } else if (argument.rfind("--ticks=", 0) == 0) {
            if (!parseTicks(argument.substr(8), tickOverride)) return 2;
        } else {
            return 2;
        }
    }

    std::vector<ScaleBenchmarkConfig> configs;
    if (agents == "1000" || agents == "all") {
        configs.push_back(Make1000AgentBenchmark(
            tickOverride == 0 ? 64 : tickOverride));
    }
    if (agents == "2500" || agents == "all") {
        configs.push_back(Make2500AgentBenchmark(
            tickOverride == 0 ? 48 : tickOverride));
    }
    if (configs.empty()) return 2;

    bool passed = true;
    for (const auto& config : configs) {
        const auto report = RunScaleBenchmark(config);
        passed = passed && report.budgetPassed();
        if (json) {
            WriteScaleBenchmarkJson(std::cout, report);
            std::cout << '\n';
        } else {
            printHuman(report);
            if (&config != &configs.back()) std::cout << '\n';
        }
    }
    return passed ? 0 : 3;
}
