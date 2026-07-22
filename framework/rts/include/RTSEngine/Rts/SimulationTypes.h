#pragma once

#include <RTSEngine/Ecs/Entity.h>
#include <RTSEngine/Rts/BaseBuilding.h>
#include <RTSEngine/Rts/GameplayModifiers.h>
#include <RTSEngine/Rts/Navigation.h>
#include <rts/sim/DeterministicCommandStream.h>

#include <cstddef>
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

enum class OrderType : std::uint8_t {
    Move,
    AttackMove
};

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
    GridPoint pathGoal{};
    bool hasPathGoal{};
    bool combatPath{};
    ecs::Entity chaseTarget{};
    GridPoint chaseTargetPosition{};
};

struct UnitDefinition {
    std::uint32_t id{};
    std::int32_t cost{};
    std::uint32_t trainTicks{1};
    std::int32_t cellsPerTick{1};
    CombatStats combat{};
};

enum class CommandType : std::uint8_t {
    Move,
    Stop,
    Build,
    CancelConstruction,
    Train,
    CancelProduction,
    SetRally,
    Attack,
    AttackMove,
    HoldPosition
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
    std::uint32_t definitionId{};
    std::uint32_t objectId{};
    ecs::Entity targetEntity{};
};

using TickCommandStream = sim::DeterministicCommandStream<TickCommand>;

enum class DomainEventType : std::uint8_t {
    MoveAccepted,
    MoveCompleted,
    OrderStopped,
    PathReady,
    PathFailed,
    ConstructionAccepted,
    ConstructionRejected,
    ConstructionCancelled,
    ConstructionCompleted,
    ConstructionDestroyed,
    ProductionAccepted,
    ProductionRejected,
    ProductionCancelled,
    ProductionCompleted,
    RallyPointChanged,
    AttackAccepted,
    AttackRejected,
    AttackMoveAccepted,
    HoldPositionAccepted,
    AttackTargetLost,
    TargetAcquired,
    WeaponFired,
    DamageApplied,
    EntityDied,
    BountyAwarded
};

struct DomainEvent {
    std::uint64_t tick{};
    DomainEventType type{};
    ecs::Entity entity{};
    std::uint32_t objectId{};
    std::uint32_t reason{};
    ecs::Entity secondary{};
    std::int32_t value{};
};

enum class SnapshotKind : std::uint8_t {
    Unit,
    Construction,
    Building
};

struct SnapshotEntity {
    ecs::Entity entity{};
    std::int32_t x{};
    std::int32_t y{};
    bool moving{};
    std::uint32_t queuedOrders{};
    SnapshotKind kind{SnapshotKind::Unit};
    std::uint32_t definitionId{};
    std::uint32_t progressTicks{};
    std::uint32_t requiredTicks{};
    std::uint32_t productionQueueSize{};
    std::uint32_t teamId{};
    std::int32_t healthCurrent{};
    std::int32_t healthMaximum{};
    std::int32_t armor{};
    std::uint32_t cooldownRemaining{};
    ecs::Entity target{};
    CombatMode combatMode{CombatMode::Guard};
};

struct WorldSnapshot {
    std::uint64_t tick{};
    std::uint64_t worldHash{};
    ResourceLedger resources{};
    std::vector<TeamModifierEntry> teamModifiers;
    std::vector<SnapshotEntity> entities;
};

} // namespace rts::gameplay
