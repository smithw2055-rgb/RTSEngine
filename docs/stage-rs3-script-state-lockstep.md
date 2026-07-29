# Stage RS3 — Script State, Archive, Hash and Lockstep

Stage RS3 makes the authoritative `RtsScriptSession` part of the same deterministic timeline as `RtsGameSession`.

## Persistent state

Each scripted Team persists:

- Team ID and script type;
- think interval and maximum intents per Tick;
- instruction, recursion and GC budgets;
- Strict determinism mode;
- next command sequence;
- enabled and started lifecycle flags;
- restricted RealScript object fields.

The archive never stores `ObjectRef`, heap slots, `NativeHandle`, registry IDs or C++ addresses. Restore creates a new object in the active Runtime/ManagedHeap and applies the validated field state.

## Combined identity and hash

`RtsScriptSessionArchive` combines:

- `RtsGameSessionArchive`;
- ScriptBundle asset identity and payload hash;
- RealScript SDK and Game SDK compatibility versions;
- Host API and program-content hashes;
- canonical Team script state.

A restore is rejected before use when program identity differs. Failed restores roll both game and script state back to their previous snapshots.

## Scripted lockstep

`RtsScriptLockstepSession` retains the existing command-frame, prediction, checkpoint and desync model. After every successful normal or replayed simulation Tick it runs:

```text
RtsScriptSession::processCompletedTick(completedTick)
```

The resulting next-Tick commands, script fields and sequence counters are included in the combined checkpoint and authoritative hash.

This covers:

- prediction rollback;
- deterministic replay;
- time-machine seek;
- desync hash reports;
- reconnect and spectator continuation;
- reconstruction in another Runtime and ManagedHeap.

The dependency-free `RtsLockstepSession` remains unchanged for games that do not enable RealScript.

## RealScript dependency

RTSEngine is pinned to merged RealScript commit:

```text
e07b519acd3935def0f6d1079e3d366472fa30b5
```

That revision restores RSBC 0.5 game-object method, constructor and property metadata without changing the bytecode wire format.
