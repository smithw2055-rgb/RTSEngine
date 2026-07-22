# Cloud Save Revision, Transport, and Retention Contract

Cloud synchronization operates on validated `RunSaveEnvelope` records. The authoritative simulation archive remains provider-independent: cloud metadata, transport, conflict preservation, and retention are product-layer services around canonical save bytes.

```text
RunSaveCloudSync
  -> IRunSaveCloudTransport
      -> conditional compare-and-swap remote head
  -> RunSaveCloudBranchStore
      -> named local conflict branches
  -> RunSaveStoragePolicy
      -> inventory, budgets, retention, cleanup
  -> RunSaveEnvelope v2
      -> optional RunSaveCloudRevision
          -> RunSaveSchema v2
              -> authoritative nested simulation archives
```

Envelope version 1 remains readable and is treated as an untracked save without ancestry metadata.

## Revision and conflict model

`RunSaveCloudRevision` stores lineage, canonical revision, authoring device, logical clock, zero to two direct parents, and a bounded vector clock. For revisions in the same lineage, vector-clock dominance identifies descendants; incomparable clocks identify a split branch. The default conflict action is `PreserveBoth`.

Envelope v2 protects cloud metadata with the checksum and validates clock ordering, bounds, flags, parent IDs, counters, and canonical revision identity. Existing product/content/build-only callers remain source compatible.

## Transport and CAS

`IRunSaveCloudTransport` exposes:

```text
fetch(key)
compareExchange(key, expectedRevisionId, envelopeBytes)
```

`expectedRevisionId == 0` means create only when absent. Existing remote data is replaced only when its current revision equals the expected revision. A CAS mismatch returns the latest remote object, and `SynchronizeRunSaveCloud` reclassifies the local head against it.

This contract maps cleanly to HTTP ETags, object-store generations, database CAS, platform cloud tokens, and provider revision APIs without adding a network dependency to the engine.

Synchronization outcomes are `NoChange`, `Uploaded`, `Downloaded`, `ConflictPreserved`, `Rejected`, `InvalidLocal`, `TransportFailure`, and `CompareExchangeConflict`.

## Conflict branches

`PreserveRunSaveCloudBranch` writes exact validated envelope bytes using:

```text
<base>--branch--<normalized-label>--<revision>.branch.sav
```

Labels are bounded and path-safe. Repeating the same request is idempotent. Branch preservation does not rewrite revision metadata or payload bytes.

## Inventory and retention

`InspectRunSaveStorage` reports bytes, counts, validity, errors, sequence, Tick, revision, and base slot for primary, recovery, branch, and temporary files.

`RunSaveStoragePolicy` supports total-byte, branch-byte, and per-slot branch-count budgets. Cleanup order is deterministic:

1. temporary files
2. invalid conflict branches
3. per-slot excess branches
4. oldest remaining branches until budgets are met

Primary and recovery slots are never automatically removed. If protected files alone exceed the configured total budget, the plan reports the budget as unsatisfied.

Cleanup executes through `IRunSaveDurability` and reports removed bytes plus per-file failures.

## Acceptance tests

`rts_cloud_transport_tests` covers create-only upload, descendant upload/download, stale revision rejection, fetch-to-upload race injection, divergent preserve-both, explicit local preference, invalid input, named/idempotent branch preservation, inventory, invalid/excess branch cleanup, byte budgets, and protection of primary/recovery slots.

The final implementation head passed Ubuntu/Windows Debug and Release in GitHub Actions run `#277`.

## Remaining product work

Production adapters still own authentication, retries, cancellation, offline queues, transport encryption, provider quotas, server storage, and user-facing conflict UX. Future slices may add an asynchronous sync queue, branch promotion/merge workflows, localized actions, scheduled cleanup, telemetry sinks, fault-injection testing, and authenticated encryption/signatures.
