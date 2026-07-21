#pragma once

#include <RTSEngine/Ecs/Scheduler.h>
#include <RTSEngine/Ecs/World.h>
#include <rts/foundation/CanonicalHash.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rts::gameplay {

struct Position {
    std::int32_t x{};
    std::int32_t y{};
};

struct MoveSpeed {
    std::int32_t unitsPerTick{1};
};

struct MoveOrder {
    std::int32_t targetX{};
    std::int32_t targetY{};
    bool active{};
};

enum class CommandType : std::uint8_t {
    Move
};

struct TickCommand {
    std::uint64_t targetTick{};
    std::uint32_t issuer{};
    std::uint32_t sequence{};
    CommandType type{CommandType::Move};
    ecs::Entity subject{};
    std::int32_t targetX{};
    std::int32_t targetY{};
};

class TickCommandStream {
public:
    bool submit(TickCommand command) {
        if (command.targetTick < committedThrough_) {
            return false;
        }
        commands_.push_back(command);
        return true;
    }

    std::vector<TickCommand> consume(std::uint64_t tick) {
        committedThrough_ = tick + 1;
        std::vector<TickCommand> result;
        auto iterator = commands_.begin();
        while (iterator != commands_.end()) {
            if (iterator->targetTick == tick) {
                result.push_back(*iterator);
                iterator = commands_.erase(iterator);
            } else {
                ++iterator;
            }
        }

        std::sort(result.begin(), result.end(), [](const TickCommand& a, const TickCommand& b) {
            if (a.issuer != b.issuer) {
                return a.issuer < b.issuer;
            }
            return a.sequence < b.sequence;
        });
        result.erase(std::unique(result.begin(), result.end(), [](const TickCommand& a, const TickCommand& b) {
            return a.issuer == b.issuer && a.sequence == b.sequence;
        }), result.end());
        return result;
    }

private:
    std::vector<TickCommand> commands_;
    std::uint64_t committedThrough_{};
};

enum class DomainEventType : std::uint8_t {
    MoveAccepted,
    MoveCompleted
};

struct DomainEvent {
    std::uint64_t tick{};
    DomainEventType type{};
    ecs::Entity entity{};
};

struct SnapshotEntity {
    ecs::Entity entity{};
    std::int32_t x{};
    std::int32_t y{};
    bool moving{};
};

struct WorldSnapshot {
    std::uint64_t tick{};
    std::uint64_t worldHash{};
    std::vector<SnapshotEntity> entities;
};

class RtsSimulation {
public:
    RtsSimulation() {
        scheduler_.add(ecs::Stage::Command, 0, 100, [this](ecs::World& world, const ecs::SystemContext& context) {
            for (const auto& command : activeCommands_) {
                if (command.type != CommandType::Move || !world.alive(command.subject)) {
                    continue;
                }
                auto* order = world.try_get<MoveOrder>(command.subject);
                if (!order) {
                    continue;
                }
                order->targetX = command.targetX;
                order->targetY = command.targetY;
                order->active = true;
                events_.push_back({context.tick, DomainEventType::MoveAccepted, command.subject});
            }
        });

        scheduler_.add(ecs::Stage::Simulation, 0, 200, [this](ecs::World& world, const ecs::SystemContext& context) {
            for (const ecs::Entity entity : world.view<Position, MoveSpeed, MoveOrder>()) {
                auto* position = world.try_get<Position>(entity);
                const auto* speed = world.try_get<MoveSpeed>(entity);
                auto* order = world.try_get<MoveOrder>(entity);
                if (!position || !speed || !order || !order->active) {
                    continue;
                }

                position->x = approach(position->x, order->targetX, speed->unitsPerTick);
                position->y = approach(position->y, order->targetY, speed->unitsPerTick);
                if (position->x == order->targetX && position->y == order->targetY) {
                    order->active = false;
                    events_.push_back({context.tick, DomainEventType::MoveCompleted, entity});
                }
            }
        });

        scheduler_.add(ecs::Stage::Snapshot, 0, 300, [this](ecs::World& world, const ecs::SystemContext& context) {
            buildSnapshot(world, context.tick);
        });
    }

    ecs::Entity createUnit(Position position, MoveSpeed speed) {
        const ecs::Entity entity = world_.create();
        world_.emplace<Position>(entity, position);
        world_.emplace<MoveSpeed>(entity, speed);
        world_.emplace<MoveOrder>(entity, MoveOrder{});
        return entity;
    }

    bool submit(TickCommand command) { return commands_.submit(command); }

    void step(std::uint64_t tick) {
        events_.clear();
        activeCommands_ = commands_.consume(tick);
        scheduler_.run(world_, tick);
    }

    const WorldSnapshot& snapshot() const noexcept { return snapshot_; }
    const std::vector<DomainEvent>& events() const noexcept { return events_; }
    const ecs::World& world() const noexcept { return world_; }

private:
    static std::int32_t approach(std::int32_t value, std::int32_t target, std::int32_t amount) {
        const std::int32_t step = amount < 0 ? -amount : amount;
        if (value < target) {
            return value + std::min(step, target - value);
        }
        if (value > target) {
            return value - std::min(step, value - target);
        }
        return value;
    }

    void buildSnapshot(ecs::World& world, std::uint64_t tick) {
        snapshot_.tick = tick;
        snapshot_.entities.clear();

        foundation::CanonicalHash hash;
        hash.WriteU64(tick);
        for (const ecs::Entity entity : world.view<Position, MoveOrder>()) {
            const auto* position = world.try_get<Position>(entity);
            const auto* order = world.try_get<MoveOrder>(entity);
            snapshot_.entities.push_back({entity, position->x, position->y, order->active});
            hash.WriteU32(entity.index);
            hash.WriteU32(entity.generation);
            hash.WriteI32(position->x);
            hash.WriteI32(position->y);
            hash.WriteBool(order->active);
            hash.WriteI32(order->targetX);
            hash.WriteI32(order->targetY);
        }
        snapshot_.worldHash = hash.Value();
    }

    ecs::World world_;
    ecs::Scheduler scheduler_;
    TickCommandStream commands_;
    std::vector<TickCommand> activeCommands_;
    std::vector<DomainEvent> events_;
    WorldSnapshot snapshot_;
};

} // namespace rts::gameplay
