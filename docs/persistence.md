# Persistence and Replay Contract

## Scope

RTSEngine now has a nested authoritative persistence chain:

```text
RunSaveSchema v2
  -> RunSimulationArchive
      -> TowerDefenseSimulationArchive
          -> RtsSimulationArchive
              -> ECS WorldArchive
```

The contracts serve different purposes:

1. **Replay archives** rebuild an RTS session from ordered commands and verify world-hash checkpoints.
2. **ECS world archives** store validated entity-registry state and schema-driven component pools.
3. **RTS simulation archives** compose the ECS world with navigation, resources, modifiers, pending commands, routes, IDs, content compatibility, and completed-Tick state.
4. **TowerDefense archives** add `WaveDirector`, wave-plan continuity, tracked enemies, base-core state, pending TowerDefense commands, and the nested RTS archive.
5. **Run simulation archives** add run progression, modifier stacks, pending Run commands, inter-wave timing, internal sequences, and the nested TowerDefense archive.
6. **RunSaveSchema v2** wraps the complete authoritative run image together with optional RNG records and diagnostic checkpoints.

A run can now be saved during an active wave, restored into a fresh simulation configured with compatible content, and continued with the same subsequent hashes.

## Canonical binary rules

All fields are written explicitly in little-endian order through `foundation::BinaryWriter` and read with bounded `foundation::BinaryReader` operations. Raw object memory, compiler padding, pointers, RTTI names, native container layouts, and platform endianness never enter an archive.

Readers reject:

- unknown magic values or unsupported schema versions
- invalid enums, entity handles, phases, or identifier ranges
- excessive counts or payload sizes
- pending commands older than their committed boundary
- duplicate or non-canonical records
- truncated payloads or trailing bytes
- incompatible registered content
- cross-layer state that cannot satisfy deterministic invariants

## Component schemas and ECS world state

`ecs::ComponentSchemaRegistry` gives each persistent component a stable `ComponentTypeId`, independent schema version, field-level writer, version-aware reader, and canonical hash callback. Removed IDs stay reserved and are never reused for a different semantic component.

`RegisterRtsComponentSchemas` registers the authoritative RTS set:

```text
Position, MoveSpeed, OrderQueue, MovementAgent
Team, TunableStats, Health, Armor, Weapon
CombatTarget, CombatDirective, Bounty
BuildingFootprint, ConstructionSite, Building
ProductionQueue, RallyPoint
```

`EntityRegistryState` stores every slot generation, alive state, and the exact free-slot stack. Preserving free-stack order guarantees that future entity creation reuses the same IDs after restore.

`ecs::WorldArchive` emits non-empty component pools by stable type ID and components by ordered `Entity`. It is independent of component insertion order and sparse-set swap-removal history. Restoration constructs a staging world and only replaces the destination after every pool and cross-reference validates.

## RTS simulation archive

`RtsSimulationArchive` version 1 stores:

```text
canonical building/unit content hash
length-delimited ECS WorldArchive
NavigationGrid dimensions, blockers, and revision
ResourceLedger
ordered team modifier profiles
pending RTS command stream and committed boundary
required route endpoints
next ConstructionId and ProductionId
player team
last completed Tick and stepped flag
save-boundary RTS world hash
```

Validation confirms that reserved resources equal active construction and production reservations, ID counters are not behind live IDs, building footprints match navigation blockers, paths and order targets remain in bounds, and the reconstructed snapshot reproduces the stored world hash.

Runtime-only events, spatial buckets, active command views, structural buffers, and combat scratch data are cleared or rebuilt rather than serialized.

## TowerDefense simulation archive

`TowerDefenseSimulationArchive` version 1 stores the nested RTS image plus:

```text
root seed
active effective WaveDefinition
WavePlan and WaveState
RewardOffer
pending TowerDefense command stream
tracked enemy records and resolved flags
base-core entity
player team
last Tick, stepped flag, and core-failure flag
save-boundary TowerDefense world hash
```

The active effective wave definition is persisted because Roguelite eligibility can modify its reward pool before a wave starts. Registered lanes, base wave definitions, rewards, and unit definitions remain content supplied by the destination runtime.

The decoder rebuilds `WaveDirector` deterministically from the root seed and active definition, replays due spawns and resolved-enemy counts, reconstructs reward state, and validates tracked entities against the restored RTS world. Partially resolved waves are supported even while later enemies have not spawned yet.

Pending TowerDefense commands are included in the TowerDefense snapshot hash, so future reward or wave commands cannot disappear without causing divergence.

## Roguelite run archive

`RunSimulationArchive` version 1 stores the nested TowerDefense image plus:

```text
canonical run/wave/modifier configuration hash
ordered ModifierStack records
pending Run command stream and committed boundary
RunState
root seed
next-wave Tick
last completed Tick
next internal command sequence
player team
stepped flag
save-boundary Run world hash
```

Modifier stacks are reconstructed against the registered modifier catalog in dependency-compatible passes. The decoder rejects missing definitions, stacks beyond `maxStacks`, unsatisfied prerequisites, exclusions, or gameplay profiles that do not match the restored TowerDefense/RTS layer.

Run phase, wave index, current wave, `nextWaveTick`, TowerDefense phase, and active WaveDirector state are cross-validated before the destination is committed.

## RunSaveSchema v2

Version 2 keeps the diagnostic and compatibility fields from version 1 and adds a length-delimited `authoritativeState` containing `RunSimulationArchive` bytes:

```text
save Tick and root seed
RunState and resource summary
resolved gameplay profile
ModifierStack summary
optional named RNG states
optional world-hash checkpoints
pending Run/TowerDefense/RTS command summaries
authoritative RunSimulation archive
```

`CaptureRunSave` collects these fields from a live `RunSimulation`. `RestoreRunSave` restores from the authoritative image. Version-1 files remain structurally decodable as progression-only records, but they do not contain enough information for direct mid-wave restoration.

The summary fields remain useful for save-slot UI, diagnostics, migration tooling, and corruption inspection without decoding the full nested image.

## Determinism acceptance tests

The regression suites cover:

- RTS save during movement, construction, production, modifiers, blockers, combat, and future commands
- TowerDefense save during spawning with multiple enemies already resolved and later spawns still pending
- pending future reward commands across restore
- a two-wave Roguelite run saved during the first active wave
- pending Run and RTS commands at the save boundary
- reward selection, modifier application, inter-wave advance, and final run completion
- identical Run, TowerDefense, and RTS hashes on every Tick after restore
- identical final canonical authoritative archives
- transactional rejection of wrong seeds, changed modifier content, incompatible dimensions, and truncated data

## Transactional restoration

Every layer decodes and validates candidates before committing them. Higher layers keep canonical backups of the nested authoritative simulation and roll back if a later cross-layer invariant fails. A rejected save therefore leaves the destination simulation unchanged.

## Versioning and next hardening slice

- Archive versions are monotonically increasing positive integers.
- Component versions are independent of outer container versions.
- Historical migration must produce the current canonical representation before simulation resumes.
- Content changes require an explicit migration or a compatibility rejection.

The next persistence hardening slice should add:

```text
explicit v1 -> v2 RunSave migration policy
atomic save replacement with primary and recovery slots
archive checksums and content-manifest identifiers
corruption/fuzz test corpus and bounded decoder fuzzing
user-facing save validation diagnostics
```
