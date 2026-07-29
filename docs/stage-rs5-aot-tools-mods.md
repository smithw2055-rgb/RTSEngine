# Stage RS5 — AOT, Tooling and Mods

Stage RS5 productizes script deployment without weakening the authoritative simulation boundary.

## AOT build integration

`cmake/RTSEngineRealScript.cmake` adds:

```cmake
rtsengine_add_script_aot(
    MyGameScripts
    PROGRAM_NAME MyGameScripts
    CPP_NAMESPACE my_game_scripts
    OPT_LEVEL 2
    SOURCES scripts/gameplay.rs scripts/ai.rs
)
```

The helper delegates code generation to RealScript's verified C++17 AOT pipeline, builds a native static library, links the RTSEngine scripting boundary and exposes the generated manifest through the `RTSENGINE_SCRIPT_AOT_MANIFEST` target property.

CI builds a real optimized AOT smoke library on Ubuntu and Windows.

## Runtime AOT selection

`ScriptAotRegistry` registers application-owned native adapters. Selection requires exact equality for:

- RealScript SDK compatibility version;
- Game SDK package version;
- Host API hash;
- Program content hash.

When several compatible native builds are registered, the highest native build hash is selected deterministically.

`ScriptExecutionFacade` supports:

```text
InterpreterOnly
PreferAot
RequireAot
```

The initial engine adapter accelerates stateless qualified-function invocation. Stateful Team and entity ScriptObjects remain on the interpreter unless a future generated adapter explicitly declares object lifecycle and persistent-state capabilities. This prevents AOT from silently bypassing RS3/RS4 object state contracts.

## Mod package

A `.rtmod` package contains:

- canonical Mod metadata;
- exact ScriptBundle and Script Program identity;
- execution and heap budgets;
- requested capabilities;
- authoritative/presentation classification;
- exact-version dependencies;
- canonical cooked assets, including the referenced ScriptBundle.

The package codec is bounded, versioned and little-endian. Package identity hashes asset schema, payload identity and dependency metadata without relying on file paths or archive ordering.

## Mod sandbox

Capabilities are explicit:

```text
GameplayRead
GameplayCommand
Presentation
FileRead
Network
WallClock
NativeExtension
```

The default authoritative policy permits only deterministic gameplay reads and commands. It rejects Network, WallClock, file access and native extensions. Presentation Mods may opt into a separate policy.

Policies also bound:

- Mod count;
- assets per Mod;
- instruction budget;
- managed heap budget;
- AOT permission.

Authoritative Mods require Strict determinism by default.

## Dependency resolution

`ScriptModResolver` validates:

- unique Mod IDs;
- required and optional dependencies;
- exact dependency versions;
- dependency cycles;
- capability and budget policy;
- canonical package hashes.

Dependencies always load before dependants. Otherwise, ready Mods are ordered by priority and then Mod ID. The resulting ordered set produces a deterministic `modSetHash`.

`CombineScriptModContentHash()` folds the Mod set into the existing multiplayer content identity so peers with different authoritative Mods cannot join the same session.

## Tooling

The `rtsmod` CLI supports:

```text
rtsmod inspect package.rtmod
rtsmod validate package.rtmod [package.rtmod ...]
rtsmod resolve package.rtmod [package.rtmod ...]
```

It prints package identities, validates sandbox policy and outputs deterministic dependency order and Mod-set hash.

## Deliberate boundaries

RS5 does not ship a native-code signing implementation or a custom cryptographic trust store. Platform storefront signatures, an audited signing provider or a game-specific trust service should authorize native AOT and NativeExtension capabilities. Unsigned script-only Mods remain bounded by the capability and deterministic execution policies.
