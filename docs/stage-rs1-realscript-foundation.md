# Stage RS1 — RealScript Foundation

Stage RS1 introduces the optional RealScript embedding boundary used by future RTS gameplay scripting. It deliberately stops before attaching scripts to `RtsGameSession`, ECS entities, lockstep, archives, or AI; those responsibilities belong to later scripting stages.

## Dependency boundary

RealScript is disabled by default so the existing RTSEngine build remains dependency-free:

```bash
cmake -S . -B build \
  -DRTSENGINE_BUILD_TESTS=ON \
  -DRTSENGINE_ENABLE_REALSCRIPT=ON
```

When no local source directory is provided, CMake fetches the pinned RS0 revision:

```text
50d13a0074d9555391f63e0dd0b1a3c2b07b040e
```

A workspace or package manager may supply the same SDK source tree directly:

```bash
cmake -S . -B build \
  -DRTSENGINE_ENABLE_REALSCRIPT=ON \
  -DRTSENGINE_REALSCRIPT_SOURCE_DIR=/path/to/RealScript
```

The scripting module links the productized `RealScript::GameSdk` target. RealScript tests, installation, and toolchain-JIT tests are disabled in the nested build because RTSEngine owns the integration boundary.

## Module boundary

```text
RTSEngine::Scripting
  ├─ RTSEngine::Foundation
  ├─ RTSEngine::Assets
  └─ RealScript::GameSdk
```

No simulation, RTS, tower-defense, Roguelite, render, audio, UI, network, or platform module depends on `RTSEngine::Scripting` in Stage RS1. Applications opt in and later framework adapters may depend on it; the authoritative simulation kernel remains independent from a concrete scripting runtime.

## Script assets

Two cooked asset types are added without renumbering existing asset kinds:

- `ScriptModule`: one verified `.rsbc` module payload;
- `ScriptBundle`: a canonical program manifest that references one or more module assets.

A bundle stores only stable identity and module references:

```text
RealScript SDK compatibility version
Game SDK package version
host API hash
program content hash
sorted module asset IDs and payload hashes
```

The bundle cooked asset declares each module as an ordinary `AssetDependency`. Requesting a bundle through `AssetManager` therefore loads and validates all required modules before the host links a program.

## Host facade

`RealScriptHost` owns the engine-side loading policy around a caller-provided `GameApi`:

1. decode and validate the cooked bundle;
2. verify the pinned RealScript SDK versions;
3. compare the compiler-visible host API hash;
4. resolve every module through `AssetManager`;
5. verify each module payload hash;
6. decode, verify, and link the `.rsbc` program through `GameProgramLoader`;
7. compare the final program content hash;
8. publish a `ScriptProgram` backed by `EngineRuntime`.

A program exposes bounded invocation through `ScriptExecutionPolicy`. Strict determinism is enabled by default, so only RealScript bindings marked `Deterministic` may execute. Instruction, recursion, and incremental-GC budgets are explicit per call.

## Failure model

Loading never exposes a partially linked program. `ScriptLoadResult` reports one stable failure category plus structured diagnostics, including:

- invalid or unloaded bundle;
- SDK version mismatch;
- host API mismatch;
- missing module;
- module payload mismatch;
- bytecode decode, verification, or link failure;
- final program content mismatch.

## Validation

The dedicated `rts_scripting_tests` integration target covers:

- source compilation to `.rsbc` through the pinned RealScript SDK;
- ScriptModule and ScriptBundle cooked-asset round trips;
- recursive bundle dependency loading through `AssetManager`;
- verified program loading and invocation;
- instruction-budget exhaustion;
- Strict-mode rejection of non-deterministic host bindings;
- host API mismatch rejection before execution.

GitHub Actions checks the integration on Ubuntu and Windows in a separate Debug matrix. The existing RTSEngine matrix remains unchanged and continues to build with RealScript disabled.

## Next stage

Stage RS2 will add the deterministic RTS adapter:

- immutable per-tick query views;
- command-intent buffering;
- Team AI script instances;
- `OnThink` and domain-event dispatch;
- normal `TickCommand` submission through `RtsGameSession`;
- no direct mutable ECS access.
