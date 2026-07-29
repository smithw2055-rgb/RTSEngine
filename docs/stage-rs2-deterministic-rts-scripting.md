# Stage RS2: Deterministic RTS scripting adapter

Stage RS2 connects the optional RealScript runtime to the RTS session without allowing scripts to mutate ECS state directly.

## Boundary

`RTSEngine::RtsScripting` depends on `RTSEngine::Rts` and `RTSEngine::Scripting`. The RTS framework remains independent from RealScript.

Scripts receive deterministic read-only functions from `Engine.Rts` and emit command intents. The adapter converts accepted intents into ordinary `TickCommand` values for the next simulation tick and submits them through `RtsGameSession::submitDetailed()`.

The first vertical slice exposes:

- `UnitCount(team)`
- `FirstUnit(team)`
- `FirstVisibleEnemy(team)`
- `Attack(subject, target)`
- `AttackMove(subject, x, y)`

Entity values are represented as stable 64-bit `(generation,index)` values, never pointers or native handles.

## Team AI lifecycle

`RtsTeamScriptDriver::afterStep(completedTick)` is called after the authoritative tick has completed. Teams are stored in ascending team ID order. Each team has a fixed think interval, stable entry point and persisted-ready next command sequence.

The default entry point is:

```text
Game.AI::OnThink(int team, long completedTick)
```

All script execution continues to use the strict deterministic policy and budgets from Stage RS1.

## Deliberate limits

Stage RS2 does not yet serialize script object fields, driver state or pending script events. It also does not replace the built-in AI automatically. Applications must not register both AI drivers for the same team.

Stage RS3 will add script state archives, canonical hashing, rollback, replay and reconnect integration.
