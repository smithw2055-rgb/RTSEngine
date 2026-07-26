# Persistence and Replay Contract

## Scope

RTSEngine now has a nested authoritative persistence chain:

```text
RunSaveSlotStore
  -> RunSaveEnvelopeCodec
      -> RunSaveSchema v2
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
7. **Run-save envelopes** add product/content/build identity, sequence ordering, flags, payload length, and corruption checksum.
8. **Primary/recovery slots** validate temporary writes, rotate the last good primary, and fall back to recovery when the primary is damaged.

A run can be saved during an active wave, restored into a fresh simulation configured with compatible content, and continued with the same subsequent hashes.

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

## RunSaveSchema v2 and migration

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

`CaptureRunSave` collects these fields from a live `RunSimulation`. `RestoreRunSave` restores from the authoritative image.

`MigrateRunSaveToCurrent` implements the explicit historical policy:

- current authoritative v2 records remain resumable
- current summary-only v2 records remain non-resumable
- v1 records are decoded and re-encoded as v2 summaries
- migration never invents missing world, navigation, wave, entity, modifier, or command state
- unsupported future schemas and corrupt records are rejected

Because version 1 never contained a complete authoritative world, a migrated v1 record can populate save-slot UI or diagnostics but cannot resume an active run.

## Run-save envelope and manifest

`RunSaveEnvelopeCodec` adds:

```text
envelope magic, version, and kind
product ID
content-manifest ID
build ID
monotonic slot sequence
save Tick
payload schema version
flags
payload size
checksum
RunSave payload
```

Product and content-manifest IDs must match exactly. Build ID is informational by default and can optionally be required to match. The manifest save Tick and authoritative/legacy flags must agree with the decoded payload.

The envelope uses deterministic FNV-1a as an accidental-corruption checksum over protected manifest fields and payload. It is not a cryptographic signature, authentication code, encryption layer, or anti-cheat mechanism.

## Primary and recovery slots

`RunSaveSlotStore` uses three files per bounded, path-safe slot name:

```text
<slot>.sav
<slot>.recovery.sav
<slot>.tmp
```

Writes validate and read back the temporary envelope before rotating the current valid primary to recovery and renaming the temporary file to primary. A corrupt primary is removed without deleting a valid recovery. Non-increasing sequence numbers are rejected so stale asynchronous work cannot overwrite a newer local save.

Loads validate the primary first and then fall back to recovery. The result reports which source was used and exposes separate primary/recovery decode errors. Recovery may optionally be promoted back to primary through another validated temporary write.

Portable C++17 provides `flush`, close, read-back, and filesystem rename, but not portable `fsync`, directory synchronization, or Windows `FlushFileBuffers`. This layer therefore provides corruption detection and recovery-slot semantics without claiming guaranteed power-loss durability. A platform adapter must add durable file/directory synchronization for shipping products.

See `docs/save-slot-storage.md` for the complete slot state machine and security/durability boundaries.

## Determinism and corruption tests

The regression suites cover:

- RTS save during movement, construction, production, modifiers, blockers, combat, and future commands
- TowerDefense save during spawning with multiple enemies already resolved and later spawns still pending
- pending future reward commands across restore
- a two-wave Roguelite run saved during the first active wave
- pending Run and RTS commands at the save boundary
- reward selection, modifier application, inter-wave advance, and final run completion
- identical Run, TowerDefense, and RTS hashes on every Tick after restore
- identical final canonical authoritative archives
- transactional rejection of wrong seeds, changed content, incompatible dimensions, and truncation
- every truncation point of a valid run-save envelope
- a nonzero mutation at every individual envelope byte
- trailing bytes, unsupported future versions, and legacy-summary migration
- real temporary-directory primary/recovery rotation, fallback, repair, sequence, and manifest behavior

## Transactional restoration

Every simulation layer decodes and validates candidates before committing them. Higher layers keep canonical backups of the nested authoritative simulation and roll back if a later cross-layer invariant fails. A rejected save therefore leaves the destination simulation unchanged.

The file layer similarly commits only a fully written and read-back-validated temporary envelope. If the primary is damaged, a previously validated recovery remains available.

## Next hardening slice

- Add a platform durability interface for POSIX/macOS/Windows file and directory synchronization.
- Add structured validation diagnostics suitable for save-slot UI and telemetry.
- Add sanitizer-backed coverage-guided fuzz targets and a persistent corruption corpus.
- Define cloud-save conflict metadata and resolution policy.
- Add optional authenticated encryption/signatures when required by the product threat model.
- Establish save-size budgets, retention, cleanup, and observability policies.
