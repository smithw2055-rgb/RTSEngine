# Persistence and Replay Contract

## Scope

RTSEngine separates three persistence contracts:

1. **Replay archives** reconstruct an authoritative session from an ordered command log and verify deterministic world-hash checkpoints.
2. **Run save archives** persist the outer roguelite progression contract: run state, modifier stacks, resources, gameplay modifier profile, named RNG stream states, hash checkpoints, and pending command streams for the Roguelite, TowerDefense, and RTS layers.
3. **ECS world archives** persist validated entity-registry state and schema-driven component pools as the foundation for direct mid-wave restoration.

The current `RunSaveSchema` does not yet embed the ECS world archive. Direct mid-wave resume still requires navigation and framework-internal state plus registration of the authoritative RTS component schemas.

## Canonical binary format

All archive fields are written explicitly in little-endian order through `foundation::BinaryWriter` and read with bounded `foundation::BinaryReader` operations. The compatibility aliases in `rts::sim` remain available to existing replay and save code. Raw object memory, compiler padding, pointers, `typeid`, native container layout, and platform endianness never enter the format.

Top-level archives begin with an explicit magic value and schema version. Readers reject:

- unknown magic values
- zero or unsupported schema versions
- the wrong archive kind
- truncated input
- invalid enum values
- excessive collection counts or payload sizes
- pending commands older than the stream's committed boundary
- invalid RNG increments
- duplicate or non-canonical records
- trailing bytes not described by the schema

## Component schema registry

Authoritative components are registered through `ecs::ComponentSchemaRegistry`. Each schema owns:

```text
stable component type ID (uint32)
current schema version (uint16)
human-readable diagnostic name
field-by-field save callback
version-aware load callback
canonical hash callback
```

The serialized component payload contract is:

```text
component type ID
stored component schema version
bounded payload byte count
canonical field payload
```

The C++ RTTI type is used only to bind runtime callbacks and construct the matching typed component pool. It is never persisted or hashed. Stable IDs must remain assigned to the same semantic component for the life of shipped save compatibility. A removed component ID must stay reserved rather than being reused.

Registration rejects zero IDs, zero versions, duplicate IDs, duplicate C++ component types, missing callbacks, and registrations after the registry is frozen. Persistent component types must be default constructible, move constructible, and move assignable so a version-aware loader can construct canonical values before insertion.

Load callbacks receive the stored schema version and must explicitly migrate supported historical payloads into the current canonical component representation. Payload readers are bounded and must consume the complete record; truncated and trailing payload bytes are rejected.

## Entity registry state

`EntityRegistryState` stores:

```text
generation for every entity slot
alive flag for every entity slot
free-slot stack in exact reuse order
```

The free-slot stack is not sorted because its order determines which entity index the next `create()` call will reuse. Validation requires:

- slot zero remains reserved with generation zero and is never alive or free
- every allocated slot has a nonzero generation
- every free index is unique, in range, and not alive
- every non-alive allocated slot appears exactly once in the free stack
- every alive slot is absent from the free stack
- configured entity-count bounds are respected

Restoration validates the complete candidate before replacing the live registry, so a rejected state leaves the existing registry unchanged.

## ECS world archive

`ecs::WorldArchive` version 1 stores:

```text
world magic and version
EntityRegistryState
component-pool count
for each non-empty pool, ordered by stable component type ID:
    component type ID
    stored schema version
    component count
    for each component, ordered by Entity:
        entity index and generation
        bounded payload byte count
        canonical component payload
```

The schema registry must be frozen before writing or reading. Every non-empty component pool must have a registered stable schema; unknown or transient pools cause the write to fail rather than being silently omitted.

Pool and entity records are emitted in canonical order, independent of component insertion order and sparse-set swap-removal history. A reader rejects duplicate or out-of-order pool IDs, duplicate or out-of-order entities, dead or stale entity handles, unknown schemas, unsupported component versions, excessive counts, truncated payloads, and unconsumed bytes.

World restoration is transactional. The reader constructs and validates a staging `World`, restores its entity registry, creates typed pools through the schema registry, and only move-replaces the destination world after the entire archive has been consumed successfully. Corrupt input therefore cannot partially overwrite the running world.

## RTS replay archive

`RtsReplay` contains:

```text
first tick
last tick
complete ordered TickCommand log
pending deterministic command-stream state
named RNG stream states
world-hash checkpoints
```

Commands are normalized by target tick, issuer, and sequence. Duplicates with the same `(targetTick, issuer, sequence)` are removed deterministically.

A replay consumer creates the same initial content and world, submits the recorded commands, advances through the checkpoint ticks, and compares each generated world hash with the stored checkpoint. Any mismatch identifies the first divergent tick.

## Roguelite run-save archive

`RunSaveSchema` contains:

```text
save tick
root seed
RunState
ResourceLedger
resolved TeamModifierProfile
ModifierStack list
named RNG stream states
world-hash checkpoints
pending Run command stream
pending TowerDefense command stream
pending RTS command stream
```

The three command streams retain their own `committedThrough` boundaries so a restored outer layer cannot re-accept stale commands or reorder future commands.

## RNG state

A named random stream persists its stream ID, algorithm state, and odd PCG increment. Restoration reproduces the next generated value exactly. Archive schemas must be versioned whenever the RNG algorithm or state interpretation changes.

## Versioning rules

- Archive schema versions are monotonically increasing positive integers.
- New optional fields require an explicit newer decoder path; they are never inferred from remaining bytes.
- Component and content-definition versions are independent from the outer archive version.
- Migration code must transform old fields into the current canonical representation before simulation resumes.
- A save is accepted only when its content rules and component schemas are compatible with the runtime.

## Next persistence slice

The next persistence milestone will add:

```text
stable schemas for authoritative RTS components
navigation-grid dimensions, blockers, route constraints, and revision
RtsSimulation world archive composition
construction and production identifier continuity
save -> restore -> continue world-hash equivalence
```

`WaveDirector` internals and embedding the completed world archive into `RunSaveSchema` follow after the RTS-layer restoration contract is stable.
