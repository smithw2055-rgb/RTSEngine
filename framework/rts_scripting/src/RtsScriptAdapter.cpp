#include <RTSEngine/RtsScripting/RtsScriptAdapter.h>

#include <RTSEngine/Rts/SimulationTypes.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace rts::gameplay::scripting {
namespace {

bool ready(const std::shared_ptr<RtsScriptContext>& context) noexcept {
    return context && context->session && context->activeTeam != 0;
}

std::int64_t firstUnit(const std::shared_ptr<RtsScriptContext>& context,
                       std::int32_t teamId) {
    if (!ready(context) || teamId <= 0) return 0;
    ecs::Entity selected{};
    const auto& world = context->session->simulation().world();
    world.eachRef<Team, Health>(
        [&](ecs::Entity entity, const Team& team, const Health& health) {
            if (team.id != static_cast<std::uint32_t>(teamId) ||
                health.current <= 0) {
                return;
            }
            if (!selected.valid() || entity < selected) selected = entity;
        });
    return PackScriptEntity(selected);
}

std::int64_t firstVisibleEnemy(
    const std::shared_ptr<RtsScriptContext>& context,
    std::int32_t teamId) {
    if (!ready(context) || teamId <= 0) return 0;
    ecs::Entity selected{};
    const auto& simulation = context->session->simulation();
    const auto& world = simulation.world();
    world.eachRef<Position, Team, Health>(
        [&](ecs::Entity entity, const Position& position,
            const Team& team, const Health& health) {
            if (health.current <= 0 ||
                !context->session->diplomacy().hostile(
                    static_cast<std::uint32_t>(teamId), team.id) ||
                !simulation.vision().visible(
                    static_cast<std::uint32_t>(teamId),
                    {position.x, position.y})) {
                return;
            }
            if (!selected.valid() || entity < selected) selected = entity;
        });
    return PackScriptEntity(selected);
}

std::int32_t unitCount(const std::shared_ptr<RtsScriptContext>& context,
                       std::int32_t teamId) {
    if (!ready(context) || teamId <= 0) return 0;
    std::int32_t count = 0;
    context->session->simulation().world().eachRef<Team, Health>(
        [&](ecs::Entity, const Team& team, const Health& health) {
            if (team.id == static_cast<std::uint32_t>(teamId) &&
                health.current > 0 &&
                count != std::numeric_limits<std::int32_t>::max()) {
                ++count;
            }
        });
    return count;
}

void attackMove(const std::shared_ptr<RtsScriptContext>& context,
                std::int64_t subject, std::int32_t x, std::int32_t y) {
    if (!ready(context)) return;
    context->intents.push_back({
        RtsScriptIntentType::AttackMove,
        UnpackScriptEntity(subject), {}, x, y});
}

void attack(const std::shared_ptr<RtsScriptContext>& context,
            std::int64_t subject, std::int64_t target) {
    if (!ready(context)) return;
    context->intents.push_back({
        RtsScriptIntentType::Attack,
        UnpackScriptEntity(subject), UnpackScriptEntity(target), 0, 0});
}

} // namespace

std::int64_t PackScriptEntity(ecs::Entity entity) noexcept {
    if (!entity.valid()) return 0;
    const auto packed =
        (static_cast<std::uint64_t>(entity.generation) << 32u) |
        static_cast<std::uint64_t>(entity.index);
    return static_cast<std::int64_t>(packed);
}

ecs::Entity UnpackScriptEntity(std::int64_t value) noexcept {
    if (value == 0) return {};
    const auto packed = static_cast<std::uint64_t>(value);
    return {
        static_cast<std::uint32_t>(packed & 0xffffffffu),
        static_cast<std::uint32_t>(packed >> 32u)};
}

void RtsScriptContext::begin(
    RtsGameSession& value,
    std::uint32_t teamId,
    std::uint64_t targetTick) {
    session = &value;
    activeTeam = teamId;
    nextTick = targetTick;
    intents.clear();
}

void RtsScriptContext::clear() noexcept {
    session = nullptr;
    activeTeam = 0;
    nextTick = 0;
    intents.clear();
}

realscript::game::GameApi CreateRtsScriptApi(
    const std::shared_ptr<RtsScriptContext>& context) {
    realscript::game::GameApi api;
    (void)api.function(
        "Engine.Rts", "UnitCount",
        [context](std::int32_t teamId) { return unitCount(context, teamId); });
    (void)api.function(
        "Engine.Rts", "FirstUnit",
        [context](std::int32_t teamId) { return firstUnit(context, teamId); });
    (void)api.function(
        "Engine.Rts", "FirstVisibleEnemy",
        [context](std::int32_t teamId) {
            return firstVisibleEnemy(context, teamId);
        });
    (void)api.function(
        "Engine.Rts", "AttackMove",
        [context](std::int64_t subject, std::int32_t x, std::int32_t y) {
            attackMove(context, subject, x, y);
        });
    (void)api.function(
        "Engine.Rts", "Attack",
        [context](std::int64_t subject, std::int64_t target) {
            attack(context, subject, target);
        });
    return api;
}

RtsTeamScriptDriver::RtsTeamScriptDriver(
    RtsGameSession& session,
    std::shared_ptr<::rts::scripting::ScriptProgram> program,
    std::shared_ptr<RtsScriptContext> context)
    : session_(session),
      program_(std::move(program)),
      context_(std::move(context)) {}

bool RtsTeamScriptDriver::registerTeam(RtsTeamScriptConfig config) {
    if (!program_ || !program_->valid() || !context_ ||
        config.teamId == 0 || config.thinkIntervalTicks == 0 ||
        config.nextSequence == 0 || config.entryPoint.empty()) {
        return false;
    }
    const auto found = std::lower_bound(
        teams_.begin(), teams_.end(), config.teamId,
        [](const RtsTeamScriptConfig& value, std::uint32_t id) {
            return value.teamId < id;
        });
    if (found != teams_.end() && found->teamId == config.teamId) return false;
    teams_.insert(found, std::move(config));
    return true;
}

bool RtsTeamScriptDriver::removeTeam(std::uint32_t teamId) {
    const auto found = std::lower_bound(
        teams_.begin(), teams_.end(), teamId,
        [](const RtsTeamScriptConfig& value, std::uint32_t id) {
            return value.teamId < id;
        });
    if (found == teams_.end() || found->teamId != teamId) return false;
    teams_.erase(found);
    return true;
}

std::size_t RtsTeamScriptDriver::afterStep(std::uint64_t completedTick) {
    if (!program_ || !program_->valid() || !context_) return 0;
    const auto targetTick = session_.simulation().nextExpectedTick();
    std::size_t accepted = 0;
    for (auto& team : teams_) {
        if (completedTick % team.thinkIntervalTicks != 0) continue;
        context_->begin(session_, team.teamId, targetTick);
        const auto execution = program_->invoke(
            team.entryPoint,
            {static_cast<std::int64_t>(team.teamId),
             realscript::runtime::LongValue{
                 static_cast<std::int64_t>(completedTick)}});
        if (!execution.succeeded) {
            errors_.push_back({completedTick, team.teamId, execution.error});
            context_->clear();
            continue;
        }
        for (const auto& intent : context_->intents) {
            if (submitIntent(team, targetTick, intent)) ++accepted;
        }
        context_->clear();
    }
    return accepted;
}

bool RtsTeamScriptDriver::submitIntent(
    RtsTeamScriptConfig& team,
    std::uint64_t targetTick,
    const RtsScriptIntent& intent) {
    if (!intent.subject.valid() ||
        team.nextSequence == std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    TickCommand command;
    command.targetTick = targetTick;
    command.issuer = team.teamId;
    command.sequence = team.nextSequence;
    command.subject = intent.subject;
    if (intent.type == RtsScriptIntentType::Attack) {
        if (!intent.target.valid()) return false;
        command.type = CommandType::Attack;
        command.targetEntity = intent.target;
    } else {
        command.type = CommandType::AttackMove;
        command.targetX = intent.x;
        command.targetY = intent.y;
    }
    const auto result = session_.submitDetailed(std::move(command));
    if (result != SessionCommandResult::Accepted) return false;
    ++team.nextSequence;
    return true;
}

} // namespace rts::gameplay::scripting
