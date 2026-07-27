#pragma once

#include <RTSEngine/Rts/SimulationTypes.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace rts::gameplay {

struct AiCommanderConfig final {
    std::uint32_t teamId{};
    std::uint32_t trainUnitDefinitionId{};
    GridPoint attackGoal{};
    std::uint32_t thinkIntervalTicks{8};
    std::uint32_t maximumUnitOrdersPerThink{8};
    std::uint32_t maximumProducerQueue{1};
    std::int32_t minimumResourcesToTrain{};
};

struct AiCommanderState final {
    std::uint32_t nextSequence{1};
    std::uint64_t lastThinkTick{};
    std::uint32_t unitCursor{};
    bool hasThought{};
};

class DeterministicAiCommander final {
public:
    explicit DeterministicAiCommander(AiCommanderConfig config)
        : config_(config) {
        if (config_.thinkIntervalTicks == 0) {
            config_.thinkIntervalTicks = 1;
        }
    }

    const AiCommanderConfig& config() const noexcept { return config_; }
    const AiCommanderState& state() const noexcept { return state_; }

    bool restore(AiCommanderState state) noexcept {
        if (state.nextSequence == 0) return false;
        state_ = state;
        return true;
    }

    bool emit(
        const WorldSnapshot& snapshot,
        std::uint64_t targetTick,
        std::vector<TickCommand>& output) {
        output.clear();
        if (config_.teamId == 0 || !shouldThink(targetTick)) return false;

        maybeTrain(snapshot, targetTick, output);
        emitUnitOrders(snapshot, targetTick, output);
        state_.lastThinkTick = targetTick;
        state_.hasThought = true;
        return !output.empty();
    }

private:
    bool shouldThink(std::uint64_t tick) const noexcept {
        if (!state_.hasThought) return true;
        if (tick <= state_.lastThinkTick) return false;
        return tick - state_.lastThinkTick >= config_.thinkIntervalTicks;
    }

    const TeamEconomySnapshot* economy(
        const WorldSnapshot& snapshot) const noexcept {
        const auto found = std::lower_bound(
            snapshot.teamEconomies.begin(),
            snapshot.teamEconomies.end(),
            config_.teamId,
            [](const TeamEconomySnapshot& entry, std::uint32_t value) {
                return entry.teamId < value;
            });
        return found != snapshot.teamEconomies.end() &&
                       found->teamId == config_.teamId
            ? &*found
            : nullptr;
    }

    const TeamVisibilitySnapshot* visibility(
        const WorldSnapshot& snapshot) const noexcept {
        const auto found = std::lower_bound(
            snapshot.visibility.begin(),
            snapshot.visibility.end(),
            config_.teamId,
            [](const TeamVisibilitySnapshot& entry, std::uint32_t value) {
                return entry.teamId < value;
            });
        return found != snapshot.visibility.end() &&
                       found->teamId == config_.teamId
            ? &*found
            : nullptr;
    }

    bool visible(
        const WorldSnapshot& snapshot,
        const TeamVisibilitySnapshot* layer,
        const SnapshotEntity& entity) const noexcept {
        if (!layer || snapshot.visibilityWidth <= 0 ||
            snapshot.visibilityHeight <= 0 || entity.x < 0 || entity.y < 0 ||
            entity.x >= snapshot.visibilityWidth ||
            entity.y >= snapshot.visibilityHeight) {
            return false;
        }
        const auto offset = static_cast<std::size_t>(entity.y) *
                                static_cast<std::size_t>(
                                    snapshot.visibilityWidth) +
                            static_cast<std::size_t>(entity.x);
        return offset < layer->current.size() && layer->current[offset] != 0;
    }

    void maybeTrain(
        const WorldSnapshot& snapshot,
        std::uint64_t tick,
        std::vector<TickCommand>& output) {
        if (config_.trainUnitDefinitionId == 0) return;
        const auto* account = economy(snapshot);
        if (!account ||
            account->resources.available < config_.minimumResourcesToTrain ||
            static_cast<std::uint64_t>(account->supplyUsed) +
                    account->supplyReserved >=
                account->supplyCapacity) {
            return;
        }

        for (const auto& entity : snapshot.entities) {
            if (entity.kind != SnapshotKind::Building ||
                entity.teamId != config_.teamId ||
                entity.productionQueueSize >= config_.maximumProducerQueue) {
                continue;
            }
            TickCommand command;
            command.targetTick = tick;
            command.issuer = config_.teamId;
            command.sequence = nextSequence();
            command.type = CommandType::Train;
            command.subject = entity.entity;
            command.definitionId = config_.trainUnitDefinitionId;
            output.push_back(command);
            return;
        }
    }

    void emitUnitOrders(
        const WorldSnapshot& snapshot,
        std::uint64_t tick,
        std::vector<TickCommand>& output) {
        const auto* layer = visibility(snapshot);
        ecs::Entity visibleEnemy;
        for (const auto& entity : snapshot.entities) {
            if (entity.teamId == 0 || entity.teamId == config_.teamId ||
                entity.healthCurrent <= 0 ||
                !visible(snapshot, layer, entity)) {
                continue;
            }
            visibleEnemy = entity.entity;
            break;
        }

        std::vector<const SnapshotEntity*> units;
        for (const auto& entity : snapshot.entities) {
            if (entity.kind == SnapshotKind::Unit &&
                entity.teamId == config_.teamId &&
                entity.healthCurrent > 0) {
                units.push_back(&entity);
            }
        }
        if (units.empty() || config_.maximumUnitOrdersPerThink == 0) return;

        const auto start = static_cast<std::size_t>(state_.unitCursor) %
                           units.size();
        const auto count = std::min<std::size_t>(
            config_.maximumUnitOrdersPerThink, units.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto& unit = *units[(start + index) % units.size()];
            TickCommand command;
            command.targetTick = tick;
            command.issuer = config_.teamId;
            command.sequence = nextSequence();
            command.subject = unit.entity;
            if (visibleEnemy.valid()) {
                command.type = CommandType::Attack;
                command.targetEntity = visibleEnemy;
            } else {
                command.type = CommandType::AttackMove;
                command.targetX = config_.attackGoal.x;
                command.targetY = config_.attackGoal.y;
            }
            output.push_back(command);
        }
        state_.unitCursor = static_cast<std::uint32_t>(
            (start + count) % units.size());
    }

    std::uint32_t nextSequence() noexcept {
        const auto result = state_.nextSequence;
        if (state_.nextSequence !=
            std::numeric_limits<std::uint32_t>::max()) {
            ++state_.nextSequence;
        }
        return result;
    }

    AiCommanderConfig config_;
    AiCommanderState state_;
};

} // namespace rts::gameplay
