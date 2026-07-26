# Save Envelope and Slot Storage

## Purpose

The simulation layers produce deterministic canonical bytes. Product-facing storage wraps those bytes in a validated envelope and writes them through a primary/recovery slot policy. Disk I/O remains outside `RunSimulation`, `TowerDefenseSimulation`, and `RtsSimulation`, so authoritative runtime code stays independent from user-directory and platform APIs.

The storage chain is:

```text
RunSaveSlotStore
  -> IRunSaveDurability / PlatformRunSaveDurability
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
4. Close the stream and invoke `IRunSaveDurability::syncFile`.
5. Read the temporary file back and verify byte identity, checksum, payload decoding, and manifest compatibility.
6. If the existing primary is valid, durably replace the recovery path with it.
7. If the existing primary is corrupt, remove it without deleting a valid recovery.
8. Durably replace the primary path with the validated temporary file.
9. Read and validate the committed primary again.

A load first validates the primary. If it is absent, corrupt, or incompatible, it tries the recovery. The result identifies which source was used and exposes separate primary/recovery envelope diagnostics. An optional repair operation copies the validated recovery through a synchronized temporary file and promotes it back to the primary path.

Slot names accept only ASCII alphanumeric characters, `_`, and `-`, with a bounded length. This prevents path traversal and keeps platform file naming predictable.

## Platform durability interface

`IRunSaveDurability` isolates four operations:

```text
syncFile(path)
replaceFile(source, destination)
removeFile(path)
syncDirectory(path)
```

`RunSaveSlotStore` accepts an injected implementation for testing or product integration and uses `PlatformRunSaveDurability` by default.

### POSIX

The default POSIX implementation uses:

- `fsync` for temporary save files
- `F_FULLFSYNC` with an `fsync` fallback on supported Apple platforms
- `rename` for same-filesystem replacement
- `fsync` on the destination parent directory after rename or unlink

A failed file or directory synchronization is a failed save operation; the temporary file is not promoted as a successful primary.

### Windows

The default Windows implementation uses:

- `CreateFileW` plus `FlushFileBuffers` for temporary save files
- `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` for replacement
- `DeleteFileW` for removal of obsolete/corrupt paths

Windows does not expose a portable directory-handle equivalent to POSIX directory `fsync`; the implementation relies on the platform write-through replacement primitive. Product certification should still test the target filesystem, device class, suspend behavior, and abrupt termination model.

## Structured diagnostics

Every slot write/load returns a `RunSaveDiagnostic` suitable for UI, logs, and telemetry. It contains:

```text
stable diagnostic code
severity
operation stage
primary/recovery source
top-level slot error
envelope error
migration status
durability operation/error
native OS error code
fallback and repair flags
```

Stable string codes are produced by `RunSaveDiagnosticCodeName`, for example:

```text
save.temporary_sync_failed
save.recovery_rotation_failed
save.loaded_recovery
save.loaded_recovery_repair_failed
```

Additional helpers expose stable names for envelope, migration, and durability errors. These identifiers are intentionally non-localized; the product UI maps them to localized messages and recovery actions.

Loading a valid recovery is a successful operation with warning severity. If optional primary repair fails, the loaded save remains usable and the diagnostic records `fallbackUsed=true`, `repairAttempted=true`, and `repairSucceeded=false` together with the durability/native error.

## Sequence and rollback protection

Each envelope carries a positive monotonically increasing sequence. A write is rejected when its sequence is not greater than the newest valid primary or recovery sequence. This prevents an older asynchronous result or stale UI action from silently replacing a newer local save.

The sequence is a local ordering contract, not a distributed cloud conflict-resolution system. Cloud synchronization requires device IDs, branch ancestry or Lamport/vector metadata, and an explicit conflict policy.

## Corruption and durability tests

`rts_save_slot_tests` validates:

- every truncation point of a valid envelope
- a nonzero mutation at every individual envelope byte
- appended trailing bytes
- legacy version-1 summary migration
- unsupported future schemas
- primary/recovery rotation using real temporary directories
- corrupted-primary recovery fallback and repair
- stale sequence rejection
- product/content/build compatibility rules
- invalid slot-name rejection
- injected file-sync and replace failures
- structured diagnostic codes, stages, fallback, repair, durability, and native error fields

The standard Ubuntu and Windows test matrix executes the real platform durability adapter.

## Sanitizers and coverage-guided fuzzing

AddressSanitizer and UndefinedBehaviorSanitizer can be enabled for the normal engine/test suite:

```bash
cmake -S . -B build-sanitize \
  -DRTSENGINE_ENABLE_SANITIZERS=ON \
  -DRTSENGINE_BUILD_TESTS=ON \
  -DRTSENGINE_BUILD_EXAMPLES=OFF
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

A Clang/libFuzzer target is available behind `RTSENGINE_BUILD_FUZZERS`:

```bash
cmake -S . -B build-fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DRTSENGINE_BUILD_TESTS=OFF \
  -DRTSENGINE_BUILD_EXAMPLES=OFF \
  -DRTSENGINE_BUILD_FUZZERS=ON
cmake --build build-fuzz --parallel
./build-fuzz/tests/fuzz/rts_run_save_envelope_fuzz \
  tests/fuzz/corpus/run_save_envelope \
  -artifact_prefix=fuzz-artifacts/ \
  -max_len=8388608
```

The committed seed corpus contains malformed/truncated envelope headers. Newly discovered crashes should be minimized and retained in the corpus so they become permanent regression inputs.

## Remaining product work

The next product-hardening slice should add:

```text
cloud-save device/revision/conflict metadata
conflict resolution and recovery policy
save-size budgets, retention, cleanup, and observability
optional authenticated encryption or signatures
platform fault-injection tests for process kill, suspend, and power-loss simulation
localized UI action mapping for structured diagnostics
```
