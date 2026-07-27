#pragma once

#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/Diplomacy.h>
#include <RTSEngine/Rts/SimulationTypes.h>
#include <RTSEngine/Rts/Vision.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

struct AiTeamState final {
    std::uint32_t teamId{};
    std::uint32_t nextSequence{1};
    std::uint32_t thinkIntervalTicks{8};
    GridPoint fallbackObjective{};
};

class AiRuntime final {
public:
    bool registerTeam(
        std::uint32_t teamId,
        GridPoint fallbackObjective,
        std::uint32_t thinkIntervalTicks = 8) {
        if (teamId == 0 || thinkIntervalTicks == 0) return false;
        const auto found = lowerBound(teamId);
        if (found != teams_.end() && found->teamId == teamId) return false;
        teams_.insert(
            found,
            AiTeamState{
                teamId,
                1,
                std::max<std::uint32_t>(1, thinkIntervalTicks),
                fallbackObjective});
        return true;
    }

    const std::vector<AiTeamState>& teams() const noexcept { return teams_; }

    void emitCommands(
        const ecs::World& world,
        const VisionRuntime& vision,
        const DiplomacyRuntime& diplomacy,
        std::uint64_t tick,
        TickCommandStream& commands) {
        for (auto& team : teams_) {
            if (tick % team.thinkIntervalTicks != 0) continue;
            emitTeamCommands(world, vision, diplomacy, tick, team, commands);
        }
    }

private:
    struct Candidate final {
        ecs::Entity entity{};
        Position position{};
    };

    auto lowerBound(std::uint32_t teamId) noexcept {
        return std::lower_bound(
            teams_.begin(), teams_.end(), teamId,
            [](const AiTeamState& state, std::uint32_t value) {
                return state.teamId < value;
            });
    }

    static std::int32_t distance(Position first, Position second) noexcept {
        const auto dx = first.x > second.x ? first.x - second.x : second.x - first.x;
        const auto dy = first.y > second.y ? first.y - second.y : second.y - first.y;
        return dx + dy;
    }

    static void emitTeamCommands(
        const ecs::World& world,
        const VisionRuntime& vision,
        const DiplomacyRuntime& diplomacy,
        std::uint64_t tick,
        AiTeamState& state,
        TickCommandStream& commands) {
        std::vector<Candidate> enemies;
        world.eachRef<Position, Team, Health>(
            [&](ecs::Entity entity,
                const Position& position,
                const Team& team,
                const Health& health) {
                if (health.current <= 0 ||
                    !diplomacy.hostile(state.teamId, team.id) ||
                    !vision.visible(state.teamId, {position.x, position.y})) {
                    return;
                }
                enemies.push_back({entity, position});
            });
        std::sort(
            enemies.begin(), enemies.end(),
            [](const Candidate& first, const Candidate& second) {
                return first.entity < second.entity;
            });

        world.eachRef<Position, Team, Health, Weapon, CombatDirective>(
            [&](ecs::Entity entity,
                const Position& position,
                const Team& team,
                const Health& health,
                const Weapon&,
                const CombatDirective&) {
                if (team.id != state.teamId || health.current <= 0) return;

                ecs::Entity best{};
                std::int32_t bestDistance =
                    std::numeric_limits<std::int32_t>::max();
                for (const auto& enemy : enemies) {
                    const auto candidateDistance = distance(position, enemy.position);
                    if (candidateDistance < bestDistance ||
                        (candidateDistance == bestDistance &&
                         (!best.valid() || enemy.entity < best))) {
                        best = enemy.entity;
                        bestDistance = candidateDistance;
                    }
                }

                TickCommand command;
                command.targetTick = tick;
                command.issuer = state.teamId;
                command.sequence = state.nextSequence++;
                command.subject = entity;
                if (best.valid()) {
                    command.type = CommandType::Attack;
                    command.targetEntity = best;
                } else {
                    command.type = CommandType::AttackMove;
                    command.targetX = state.fallbackObjective.x;
                    command.targetY = state.fallbackObjective.y;
                }
                commands.submitDetailed(std::move(command));
            });
    }

    std::vector<AiTeamState> teams_;
};

} // namespace rts::gameplay
