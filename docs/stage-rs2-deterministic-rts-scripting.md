# Stage RS2 — Deterministic RTS Scripting Adapter

Stage RS2 connects the productized RealScript runtime to `RtsGameSession` without allowing scripts to bypass authoritative command validation, fixed-tick ordering, visibility rules, or deterministic execution budgets.

## Module boundary

```text
RTSEngine::RtsScripting
  ├─ RTSEngine::Rts
  └─ RTSEngine::Scripting
```

`RTSEngine::Rts` remains independent from RealScript. Applications opt into the adapter and call it immediately after a successful `RtsGameSession::stepDetailed()`.

## Immutable read view

`RtsScriptReadView` captures a Team-specific value view from the completed `WorldSnapshot`:

- current completed Tick;
- Team resources and Supply;
- own entities;
- neutral or hostile entities only when currently visible;
- stable entity ordering;
- positions, health, movement, queue and producer state.

No `ecs::World`, component address, mutable simulation object, renderer, platform service, wall clock, file system, or network object is exposed.

Entity identities are packed from the generational `ecs::Entity` index and generation into a RealScript `long`. Zero remains the invalid/null entity identity.

## Host API

The deterministic `Engine.Rts` module provides bounded queries:

```text
CompletedTick, TargetTick, Team
ResourceAvailable, UsedSupply, SupplyCapacity
IsAlive, EntityTeam, EntityX, EntityY, Health, IsIdle
FindIdleUnit, FindIdleProducer, FindNearestVisibleEnemy
```

Mutation-shaped functions only append command intents:

```text
Move, AttackMove, Stop, HoldPosition
Attack, Gather, Train, Research
```

The binding layer validates basic ownership, visibility, target kind, map bounds, definition IDs, and per-Tick intent limits before accepting an intent. It never mutates the simulation.

## Team script lifecycle

A Team script is a rooted RealScript object with the following supported callbacks:

```text
OnCreate(int teamId)                         optional
OnStart()                                    optional
OnEvent(int type, long entity,
        long secondary, int value)           optional
OnThink(long targetTick)                     required
```

Teams execute in ascending `teamId` order. Within one Team the order is:

```text
OnStart
visible/relevant DomainEvents in publication order
OnThink when targetTick % thinkIntervalTicks == 0
intent submission in script call order
```

If any callback fails, every intent produced by that Team during the current adapter pass is discarded. Other Teams continue deterministically.

## Command submission

After callbacks succeed, intents become ordinary `TickCommand` values:

- `targetTick` is the next simulation Tick;
- `issuer` is the scripted Team;
- `sequence` is monotonic per Team;
- submission uses `RtsGameSession::submitDetailed()`;
- accepted and rejected results are reported through `RtsScriptCommandOutcome`.

The adapter does not reorder or directly apply commands. The existing Session remains the authority for ownership, diplomacy, visibility, producer policy, queue capacity, Supply, research, resources and command identity.

Built-in `AiRuntime` and scripted AI may not own the same Team. This prevents command sequence collisions and ambiguous authority.

## Determinism and safety

Each Team defines:

- think interval;
- maximum intents per Tick;
- instruction budget;
- recursion budget;
- incremental GC work budget;
- Strict determinism policy.

Strict RealScript execution remains enabled by default. The adapter API registers only deterministic bindings.

Script execution after a completed Tick cannot change the current `WorldSnapshot` or World Hash. It can only enqueue validated commands for the next Tick.

## Validation

`rts_rts_scripting_tests` covers:

- RealScript Team object creation and lifecycle resolution;
- immutable visible-world capture;
- hidden enemy filtering;
- deterministic unit and target selection;
- next-Tick Attack intent conversion;
- unchanged World Hash before command execution;
- stable command identity across identical runs;
- rejection when built-in AI already owns the Team.

GitHub Actions builds and runs both RS1 and RS2 scripting tests on Ubuntu and Windows using the pinned RealScript RS0 SDK revision.

## Deferred to RS3

Stage RS2 intentionally does not persist script objects or sequence state. Stage RS3 will add:

- canonical script instance state archives;
- script state in authoritative hashes;
- rollback checkpoint integration;
- replay and reconnect restoration;
- ScriptBundle/content identity in multiplayer compatibility checks.
