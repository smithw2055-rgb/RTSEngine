# Stage RS4 — Entity Behaviors and Deterministic Triggers

Stage RS4 extends the authoritative RealScript integration from Team-level AI to sparse per-entity behavior objects. It deliberately avoids a per-entity, per-frame `MonoBehaviour` model.

## Runtime boundary

`RtsEntityBehaviorRuntime` owns one optional rooted ScriptObject for each attached generational ECS Entity. Attachments are canonically ordered by packed Entity identity.

Supported lifecycle callbacks are optional and argument-free:

```text
OnCreate
OnStart
OnEvent
OnTrigger
OnThink
OnDestroy
```

The active context is read through the `Engine.RtsBehavior` module:

```text
Self, Team, CompletedTick, TargetTick
EventType, EventEntity, EventSecondary, EventValue, EventReason
TriggerId, TriggerKind, TriggerValue
IsAlive, EntityX, EntityY, Health, FindNearestVisibleEnemy
Move, AttackMove, Stop, Attack
```

Commands implicitly use `Self` as their subject. A behavior cannot command another entity. Bindings only append bounded intents and never mutate ECS state.

## Scheduling

For each completed Tick, attached behaviors execute in ascending generational Entity order:

```text
OnStart once
relevant DomainEvents in publication order
matching EventType triggers
state triggers in trigger-ID order
OnThink at the configured interval
intent submission for the next Tick
```

Only events whose primary or secondary Entity matches `Self` are dispatched.

## Triggers

The deterministic trigger model supports:

- absolute or recurring Tick triggers;
- health-at-or-below edge triggers;
- visible-enemy edge triggers;
- relevant DomainEvent type triggers.

Trigger definitions have explicit stable IDs. Runtime state persists enabled, fired, latched, and next-fire-Tick values. Recurring state triggers fire on false-to-true transitions instead of every Tick while a condition remains true.

## Command identity

Team AI keeps the low command-sequence range. Entity behaviors use the high range beginning at `0x80000000`, with one monotonic sequence per Team. This prevents ordinary Team AI and entity behavior commands from colliding while retaining the normal `RtsGameSession::submitDetailed()` authority boundary.

## Persistence

Entity behavior state includes:

- exact Script Program identity;
- attached Entity and Team identity;
- script type and execution budgets;
- enabled and started state;
- restricted ScriptObject fields;
- deterministic trigger state;
- per-Team high-range command sequences.

ObjectRef, heap slots, NativeHandle values and C++ addresses are never serialized.

`RtsScriptWorldArchive` combines:

```text
RtsGameSession
Team RtsScriptSession
RtsEntityBehaviorRuntime
```

The combined hash covers all three domains. Restore is transactional and rebuilds behavior objects in the target Runtime/ManagedHeap.

## Scope boundary

RS4 does not run a callback on every entity every Tick by default. `OnThink` is opt-in through a non-zero interval. Multiple behavior objects on one Entity, presentation-only behaviors, animation callbacks and editor visual scripting remain separate future layers.
