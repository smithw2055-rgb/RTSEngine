# Persistence and Replay Contract

## Scope

RTSEngine separates two persistence products:

1. **Replay archives** reconstruct an authoritative session from an ordered command log and verify deterministic world-hash checkpoints.
2. **Run save archives** persist the outer roguelite progression contract: run state, modifier stacks, resources, gameplay modifier profile, named RNG stream states, hash checkpoints, and pending command streams for the Roguelite, TowerDefense, and RTS layers.

The current run-save schema is a progression/checkpoint contract. It does not yet serialize an arbitrary mid-wave ECS world image. Direct mid-wave resume will be added after entity-registry, component-pool, navigation, and framework-internal restoration are completed.

## Canonical binary format

All archive fields are written explicitly in little-endian order through `foundation::BinaryWriter` and read with bounded `foundation::BinaryReader` operations. The compatibility aliases in `rts::sim` remain available to existing replay and save code. Raw object memory, compiler padding, pointers, `typeid`, native container layout, and platform endianness never enter the format.

Every archive begins with:

```text
magic
schema version
archive kind
```

Readers reject:

- unknown magic values
- zero or unsupported future schema versions
- the wrong archive kind
- truncated input
- invalid enum values
- excessive collection counts
- pending commands older than the stream's committed boundary
- invalid RNG increments
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

The serialized component record is:

```text
component type ID
stored component schema version
bounded payload byte count
canonical field payload
```

The C++ RTTI type is used only to bind a runtime callback to a component class. It is never persisted or hashed. Stable IDs must remain assigned to the same semantic component for the life of shipped save compatibility. A removed component ID must stay reserved rather than being reused.

Registration rejects zero IDs, zero versions, duplicate IDs, duplicate C++ component types, missing callbacks, and registrations after the registry is frozen. Descriptor enumeration is sorted by stable type ID so validation and tooling do not inherit `unordered_map` iteration order.

Load callbacks receive the stored schema version and must explicitly migrate supported historical payloads into the current canonical component representation. Payload readers are bounded and must consume the complete record; truncated and trailing payload bytes are rejected.

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

Commands are normalized by:

```text
target tick
issuer
sequence
```

Duplicates with the same `(targetTick, issuer, sequence)` are removed deterministically.

A replay consumer creates the same initial content/world, submits the recorded commands, advances through the checkpoint ticks, and compares each generated world hash with the stored checkpoint. Any mismatch identifies the first divergent tick.

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

A named random stream persists:

```text
stream ID
algorithm state
odd PCG increment
```

Restoration reproduces the next generated value exactly. Archive schemas must be versioned whenever the RNG algorithm or state interpretation changes.

## Versioning rules

- Archive schema versions are monotonically increasing positive integers.
- New optional fields require an explicit newer decoder path; they are never inferred from remaining bytes.
- Component and content-definition versions are independent from the outer archive version.
- Migration code must transform old fields into the current canonical representation before simulation resumes.
- A save is accepted only when its content rules and component schemas are compatible with the runtime.

## Next persistence slice

The next persistence milestone will add:

```text
EntityRegistry state and validation
schema-driven component-pool records
navigation-grid state
construction and production IDs
WaveDirector internal state
full mid-wave world restoration
save -> restore -> continue hash-equivalence tests
```
