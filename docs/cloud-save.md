# Cloud Save Revision, Transport, and Retention Contract

## Scope

Cloud synchronization operates on validated `RunSaveEnvelope` records. The authoritative simulation archive remains provider-independent: cloud metadata, transport, conflict preservation, and retention are product-layer services around the canonical save bytes.

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

## Revision metadata

`RunSaveCloudRevision` stores lineage ID, revision ID, authoring device ID, logical clock, zero to two direct parents, and a bounded vector clock ordered by device ID. A lineage identifies one logical save history. A normal commit has one parent, a merge may have two, and a root has none.

The vector clock supports up to 32 devices. Entries are canonical, unique, strictly ordered, and nonzero. `MakeRunSaveCloudRevision` validates parents, merges their clocks component-wise, increments the current device, and records sorted unique parent IDs.

The envelope codec finalizes the revision ID from product/content identity, lineage, device, logical clock, save Tick, parents, vector clock, and payload checksum. Slot sequence and build ID are excluded so rewrapping the same causal state does not create a different cloud revision.

## Envelope version 2

Envelope v2 protects cloud revision fields with the envelope checksum. The decoder rejects malformed clocks, excessive parents/devices, zero IDs/counters, mismatched canonical revision IDs, invalid flags, truncation, checksum failure, and trailing data.

Version-1 field order and checksum remain unchanged, so old local saves continue to decode. Existing callers that initialize only product, content, and build IDs remain source compatible; cloud fields default to untracked.

## Causal comparison

For revisions in the same lineage:

- local clock strictly dominates cloud: local is a descendant
- cloud clock strictly dominates local: cloud is a descendant
- equal revision ID: both heads are identical
- neither clock dominates: the heads diverged

A merge revision dominates both parents because it takes the component-wise maximum and increments the merging device.

`ResolveRunSaveCloudConflict` classifies missing, invalid, identical, descendant, divergent, unrelated-lineage, incompatible-manifest, and untracked records. The default divergence policy is `PreserveBoth`; valid incomparable branches are never silently overwritten.

Optional policies are `PreferLocal`, `PreferCloud`, and `PreferDeterministicLatest`. Deterministic-latest is a stable product tie-break, not a semantic merge.

## Cloud transport contract

`IRunSaveCloudTransport` exposes:

```text
fetch(key)
compareExchange(key, expectedRevisionId, envelopeBytes)
```

`expectedRevisionId == 0` means create only when the remote key is absent. Existing objects are replaced only when the remote head still equals the expected revision.

A failed compare-and-swap returns the latest remote object. `SynchronizeRunSaveCloud` reclassifies the local head against that current remote head, closing the race between fetch and upload without coupling the engine to a provider-specific ETag, generation number, transaction API, or HTTP library.

Provider adapters may map the contract to HTTP `If-Match`, object-store generations, database CAS, Steam/Xbox/PlayStation tokens, PlayFab revisions, or proprietary account storage.

The in-memory reference transport validates keys, checksummed envelopes, tracked revisions, expected revision equality, product/content identity, and lineage compatibility. The transport does not decide whether divergent progress should overwrite another branch.

## Synchronization outcomes

`SynchronizeRunSaveCloud` returns:

```text
NoChange
Uploaded
Downloaded
ConflictPreserved
Rejected
InvalidLocal
TransportFailure
CompareExchangeConflict
```

Typical behavior is:

- local only or local descendant: CAS upload
- cloud only or cloud descendant: return exact cloud envelope for local import
- identical: no action
- divergent with default policy: return both exact envelopes
- manifest incompatibility: reject
- CAS race: return latest remote head and updated conflict classification

No provider operation mutates `RunSimulation`, `TowerDefenseSimulation`, or `RtsSimulation` directly.

## Conflict branch preservation and naming

`PreserveRunSaveCloudBranch` stores the original validated envelope under:

```text
<base>--branch--<normalized-label>--<revision>.branch.sav
```

Labels are normalized to bounded lowercase ASCII alphanumeric/hyphen text. Empty labels fall back to `local` or `cloud`. Tracked records use the revision ID; untracked records use the envelope checksum.

Exact bytes are preserved, so branch creation does not change revision, vector clock, payload, build metadata, or checksum. Repeating the same preservation request is idempotent.

Conflict branches remain separate from `<slot>.sav` and `<slot>.recovery.sav`. Selecting one for active play should explicitly create a child or merge revision rather than rewriting history.

## Storage inventory and observability

`InspectRunSaveStorage` reports total bytes, bytes and counts by file kind, invalid-envelope counts, and per-file validity, envelope error, sequence, save Tick, revision ID, and base slot.

The inventory supports settings UI, telemetry, support diagnostics, and capacity preflight without coupling disk management to authoritative simulation state.

## Retention and capacity budgets

`RunSaveStoragePolicy` supports maximum total bytes, maximum conflict-branch bytes, maximum branches per base slot, temporary-file cleanup, and invalid-branch cleanup.

Retention is deterministic and independent from wall-clock timestamps. Cleanup order is:

1. temporary files
2. invalid conflict branches
3. branches beyond the per-slot count, oldest canonical metadata first
4. oldest remaining branches until branch and total byte budgets are satisfied

Primary and recovery files are never automatically selected. If protected files alone exceed the total budget, the plan reports the budget as unsatisfied instead of deleting recoverable gameplay state.

`ApplyRunSaveStorageRetentionPlan` removes planned files through `IRunSaveDurability` and reports planned count, removed count, removed bytes, and per-file durability failures.

## Local sequence versus cloud revision

```text
slot sequence -> local write ordering
revision ID + vector clock -> cross-device ancestry
provider CAS -> race-free remote head update
```

All three contracts are required; none replaces the others.

## Acceptance tests

`rts_cloud_save_tests` covers Envelope compatibility and revision/conflict semantics.

`rts_cloud_transport_tests` covers:

- create-only upload
- descendant upload and download
- stale expected-revision rejection
- fetch-to-upload race injection and reclassification
- divergent preserve-both and explicit prefer-local behavior
- invalid key and missing-revision rejection
- deterministic user-labelled branch preservation
- idempotent branch writes
- primary/recovery/branch/temporary inventory
- invalid and excess branch cleanup
- branch-byte budget enforcement
- proof that primary and recovery survive cleanup

The code-bearing and final documentation heads are validated in Ubuntu/Windows Debug and Release through the standard CI matrix.

## Remaining product work

A production transport still owns authentication, account authorization, retries, cancellation, offline queues, encryption in transit, provider quotas, and server-side storage. Upload APIs should always condition writes on the expected remote revision.

Future slices may add an asynchronous retry/offline queue, localized conflict actions, branch promotion/merge workflows, scheduled cleanup, telemetry sinks, process-kill and suspend fault injection, and optional authenticated encryption or signatures.
