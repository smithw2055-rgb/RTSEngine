#pragma once

#include <RTSEngine/Ecs/Scheduler.h>
#include <RTSEngine/Ecs/World.h>
#include <RTSEngine/Rts/Navigation.h>
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
    std::int32_t cellsPerTick{1};
};

enum class OrderType : std::uint8_t { Move };

struct Order {
    OrderType type{OrderType::Move};
    GridPoint target{};
};

struct OrderQueue {
    std::vector<Order> pending;
};

struct MovementAgent {
    std::vector<GridPoint> path;
    std::size_t nextPoint{};
    std::uint64_t pathRevision{};
};

enum class CommandType : std::uint8_t {
    Move,
    Stop
};

struct TickCommand {
    std::uint64_t targetTick{};
    std::uint32_t issuer{};
    std::uint32_t sequence{};
    CommandType type{CommandType::Move};
    ecs::Entity subject{};
    std::int32_t targetX{};
    std::int32_t targetY{};
    bool append{};
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
            return a.issuer != b.issuer ? a.issuer < b.issuer : a.sequence < b.sequence;
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
    MoveCompleted,
    OrderStopped,
    PathReady,
    PathFailed
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
    std::uint32_t queuedOrders{};
};

struct WorldSnapshot {
    std::uint64_t tick{};
    std::uint64_t worldHash{};
    std::vector<SnapshotEntity> entities;
};

class RtsSimulation {
public:
    RtsSimulation(std::int32_t width = 32, std::int32_t height = 32)
        : navigation_(width, height) {
        scheduler_.add(ecs::Stage::Command, 0, 100, [this](ecs::World& world, const ecs::SystemContext& context) {
            for (const auto& command : activeCommands_) {
                if (!world.alive(command.subject)) {
                    continue;
                }
                auto* queue = world.try_get<OrderQueue>(command.subject);
                auto* agent = world.try_get<MovementAgent>(command.subject);
                if (!queue || !agent) {
                    continue;
                }

                if (command.type == CommandType::Stop) {
                    queue->pending.clear();
                    agent->path.clear();
                    agent->nextPoint = 0;
                    events_.push_back({context.tick, DomainEventType::OrderStopped, command.subject});
                    continue;
                }

                if (!command.append) {
                    queue->pending.clear();
                    agent->path.clear();
                    agent->nextPoint = 0;
                }
                queue->pending.push_back({OrderType::Move, {command.targetX, command.targetY}});
                events_.push_back({context.tick, DomainEventType::MoveAccepted, command.subject});
            }
        });

        scheduler_.add(ecs::Stage::Navigation, 0, 200, [this](ecs::World& world, const ecs::SystemContext& context) {
            for (const auto entity : world.view<Position, OrderQueue, MovementAgent>()) {
                auto* position = world.try_get<Position>(entity);
                auto* queue = world.try_get<OrderQueue>(entity);
                auto* agent = world.try_get<MovementAgent>(entity);
                if (!position || !queue || !agent || queue->pending.empty()) {
                    continue;
                }

                if (agent->pathRevision != navigation_.revision()) {
                    agent->path.clear();
                    agent->nextPoint = 0;
                }
                if (!agent->path.empty()) {
                    continue;
                }

                const auto target = queue->pending.front().target;
                if (position->x == target.x && position->y == target.y) {
                    queue->pending.erase(queue->pending.begin());
                    events_.push_back({context.tick, DomainEventType::MoveCompleted, entity});
                    continue;
                }

                const auto path = GridPathfinder::find(navigation_, {position->x, position->y}, target);
                agent->pathRevision = navigation_.revision();
                if (!path.found) {
                    queue->pending.erase(queue->pending.begin());
                    events_.push_back({context.tick, DomainEventType::PathFailed, entity});
                    continue;
                }
                agent->path = path.points;
                agent->nextPoint = 0;
                events_.push_back({context.tick, DomainEventType::PathReady, entity});
            }
        });

        scheduler_.add(ecs::Stage::Simulation, 0, 300, [this](ecs::World& world, const ecs::SystemContext& context) {
            for (const auto entity : world.view<Position, MoveSpeed, OrderQueue, MovementAgent>()) {
                auto* position = world.try_get<Position>(entity);
                const auto* speed = world.try_get<MoveSpeed>(entity);
                auto* queue = world.try_get<OrderQueue>(entity);
                auto* agent = world.try_get<MovementAgent>(entity);
                if (!position || !speed || !queue || !agent || agent->path.empty()) {
                    continue;
                }

                const auto amount = std::max<std::int32_t>(1, speed->cellsPerTick);
                for (std::int32_t step = 0; step < amount && agent->nextPoint < agent->path.size(); ++step) {
                    const auto point = agent->path[agent->nextPoint++];
                    position->x = point.x;
                    position->y = point.y;
                }

                if (agent->nextPoint == agent->path.size()) {
                    agent->path.clear();
                    agent->nextPoint = 0;
                    if (!queue->pending.empty()) {
                        queue->pending.erase(queue->pending.begin());
                    }
                    events_.push_back({context.tick, DomainEventType::MoveCompleted, entity});
                }
            }
        });

        scheduler_.add(ecs::Stage::Snapshot, 0, 400, [this](ecs::World& world, const ecs::SystemContext& context) {
            buildSnapshot(world, context.tick);
        });
    }

    ecs::Entity createUnit(Position position, MoveSpeed speed) {
        const auto entity = world_.create();
        world_.emplace<Position>(entity, position);
        world_.emplace<MoveSpeed>(entity, speed);
        world_.emplace<OrderQueue>(entity, OrderQueue{});
        world_.emplace<MovementAgent>(entity, MovementAgent{});
        return entity;
    }

    bool submit(TickCommand command) { return commands_.submit(command); }
    bool setBlocked(GridPoint point, bool blocked) { return navigation_.setBlocked(point, blocked); }
    const NavigationGrid& navigation() const noexcept { return navigation_; }

    void step(std::uint64_t tick) {
        events_.clear();
        activeCommands_ = commands_.consume(tick);
        scheduler_.run(world_, tick);
    }

    const WorldSnapshot& snapshot() const noexcept { return snapshot_; }
    const std::vector<DomainEvent>& events() const noexcept { return events_; }
    const ecs::World& world() const noexcept { return world_; }

private:
    void buildSnapshot(ecs::World& world, std::uint64_t tick) {
        snapshot_.tick = tick;
        snapshot_.entities.clear();

        foundation::CanonicalHash hash;
        hash.WriteU64(tick);
        hash.WriteU64(navigation_.revision());
        for (const auto blocked : navigation_.blockers()) {
            hash.WriteU8(blocked);
        }

        for (const auto entity : world.view<Position, OrderQueue, MovementAgent>()) {
            const auto* position = world.try_get<Position>(entity);
            const auto* queue = world.try_get<OrderQueue>(entity);
            const auto* agent = world.try_get<MovementAgent>(entity);
            const bool moving = !agent->path.empty() || !queue->pending.empty();
            snapshot_.entities.push_back({entity, position->x, position->y, moving,
                                          static_cast<std::uint32_t>(queue->pending.size())});
            hash.WriteU32(entity.index);
            hash.WriteU32(entity.generation);
            hash.WriteI32(position->x);
            hash.WriteI32(position->y);
            hash.WriteBool(moving);
            hash.WriteU32(static_cast<std::uint32_t>(queue->pending.size()));
            for (const auto& order : queue->pending) {
                hash.WriteU8(static_cast<std::uint8_t>(order.type));
                hash.WriteI32(order.target.x);
                hash.WriteI32(order.target.y);
            }
            hash.WriteU64(agent->pathRevision);
            hash.WriteU64(static_cast<std::uint64_t>(agent->nextPoint));
            hash.WriteU64(static_cast<std::uint64_t>(agent->path.size()));
            for (const auto point : agent->path) {
                hash.WriteI32(point.x);
                hash.WriteI32(point.y);
            }
        }
        snapshot_.worldHash = hash.Value();
    }

    ecs::World world_;
    ecs::Scheduler scheduler_;
    TickCommandStream commands_;
    NavigationGrid navigation_;
    std::vector<TickCommand> activeCommands_;
    std::vector<DomainEvent> events_;
    WorldSnapshot snapshot_;
};

} // namespace rts::gameplay
