# Save Envelope and Slot Storage

## Purpose

The simulation layers produce deterministic canonical bytes. Product-facing storage wraps those bytes in a validated envelope and writes them through a primary/recovery slot policy. Disk I/O remains outside `RunSimulation`, `TowerDefenseSimulation`, and `RtsSimulation` so authoritative runtime code is independent from user-directory and platform APIs.

The storage chain is:

```text
RunSaveSlotStore
  -> RunSaveEnvelopeCodec
      -> RunSaveSchema v2
          -> RunSimulationArchive
              -> TowerDefenseSimulationArchive
                  -> RtsSimulationArchive
                      -> ECS WorldArchive
```

## Run-save migration policy

`MigrateRunSaveToCurrent` classifies a payload as:

- `CurrentAuthoritative`: current schema with a complete resumable world image
- `CurrentSummaryOnly`: current schema containing diagnostics/progression only
- `MigratedLegacySummary`: version-1 progression data re-encoded in the current schema without inventing missing world state
- `UnsupportedFuture`: a valid header with a newer unsupported schema
- `Corrupt`: malformed, truncated, invalid, or wrong-kind data

Version 1 did not contain a complete authoritative world. Migration therefore preserves its summary fields but deliberately leaves `authoritativeState` empty. Such a record can populate save-slot UI or historical diagnostics, but `RestoreRunSave` rejects it for mid-wave resume. Migration never fabricates entities, wave state, navigation, command queues, or modifier state that were absent from the original archive.

## Save envelope

`RunSaveEnvelopeCodec` adds a fixed versioned envelope around the current run-save payload:

```text
envelope magic
version and kind
product ID
content-manifest ID
build ID
monotonic slot sequence
save Tick
payload schema version
flags
payload byte count
checksum
payload bytes
```

Product, content, and build identifiers are stable 64-bit values. `MakeSaveIdentifier` derives deterministic IDs from canonical names; shipping products may instead inject IDs produced by their content/build pipeline.

Compatibility rules are:

- product ID must match exactly
- content-manifest ID must match exactly
- build ID is informational by default and may optionally be required to match exactly
- unknown envelope versions, kinds, or flags are rejected
- save Tick and authoritative/summary flags must agree with the decoded payload
- excessive payload sizes, truncation, and trailing bytes are rejected

The envelope checksum is deterministic FNV-1a over the protected manifest fields and payload. It detects accidental corruption and torn/partial writes. It is **not** a cryptographic authenticity primitive and must not be treated as protection against malicious modification. Signed, authenticated, encrypted, or anti-cheat saves belong in a product/platform security layer.

## Primary and recovery slots

For slot name `slot_1`, `RunSaveSlotStore` uses:

```text
slot_1.sav
slot_1.recovery.sav
slot_1.tmp
```

A write performs:

1. Validate/migrate the run-save payload and build the checksummed envelope.
2. Reject invalid slot names and non-increasing sequence numbers.
3. Write the complete envelope to the temporary file.
4. Close and read the temporary file back.
5. Verify byte identity, envelope checksum, payload decoding, and manifest compatibility.
6. If the existing primary is valid, rotate it to the recovery path.
7. If the existing primary is corrupt, remove it without deleting a valid recovery.
8. Rename the validated temporary file to the primary path.
9. Read and validate the committed primary again.

A load first validates the primary. If it is absent, corrupt, or incompatible, it tries the recovery. The result identifies which source was used and exposes separate primary/recovery envelope diagnostics. An optional repair operation copies the validated recovery through a temporary file and promotes it back to the primary path.

Slot names accept only ASCII alphanumeric characters, `_`, and `-`, with a bounded length. This prevents path traversal and keeps platform file naming predictable.

## Sequence and rollback protection

Each envelope carries a positive monotonically increasing sequence. A write is rejected when its sequence is not greater than the newest valid primary or recovery sequence. This prevents an older asynchronous result or stale UI action from silently replacing a newer local save.

The sequence is a local ordering contract, not a distributed cloud conflict-resolution system. Cloud synchronization will require device IDs, branch ancestry or vector/lamport metadata, and an explicit conflict policy.

## Durability boundary

The portable C++17 implementation uses binary write, `flush`, close, read-back validation, and same-filesystem `rename`. It provides:

- canonical bytes
- corruption detection
- validated temporary writes
- file-level replacement
- primary/recovery fallback
- recovery promotion

The C++ standard library does not expose a portable directory `fsync`, POSIX `fsync`, macOS full-sync, or Windows `FlushFileBuffers` contract. Consequently, this layer does not claim guaranteed survival across power loss or storage-controller failure immediately after commit. Shipping platform adapters should add durable file and directory synchronization around the same state machine.

## Corruption corpus

`rts_save_slot_tests` validates:

- every truncation point of a valid envelope
- a nonzero mutation at every individual envelope byte
- appended trailing bytes
- legacy version-1 summary migration
- unsupported future schemas
- primary/recovery rotation
- corrupted-primary recovery fallback
- optional primary repair
- stale sequence rejection
- product/content/build compatibility rules
- invalid slot-name rejection

These bounded deterministic cases complement, but do not replace, sanitizer-backed coverage-guided fuzzing.

## Next hardening work

The next product-hardening slice should add:

```text
platform durability adapter (fsync / FlushFileBuffers)
structured validation diagnostics suitable for save-slot UI
coverage-guided decoder fuzz targets and persistent corruption corpus
cloud-save conflict metadata and merge/recovery policy
optional authenticated encryption or signatures
save-size budgets, telemetry, and retention/cleanup policy
```
