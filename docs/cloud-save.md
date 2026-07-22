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

`RunSaveCloudRevision` stores:

```text
lineage ID
revision ID
authoring device ID
authoring device logical clock
zero, one, or two direct parent revision IDs
bounded vector clock entries ordered by device ID
```

A lineage identifies one logical save history. A normal commit has one parent. A merge commit may have two parents. A root revision has no parents.

The vector clock supports up to 32 devices. Entries are canonical, strictly ordered by device ID, unique, and nonzero. The authoring device entry must equal `logicalClock`.

`MakeRunSaveCloudRevision` creates a root, child, or merge candidate by:

1. validating all parent revisions
2. requiring every parent to belong to the same lineage
3. taking the component-wise maximum of the parent vector clocks
4. incrementing the current device counter
5. recording the sorted unique direct parent revision IDs

The revision ID is finalized by `RunSaveEnvelopeCodec` from the product/content identity, lineage, authoring device, logical clock, save Tick, parent IDs, vector clock, and payload checksum. Slot sequence and build ID are deliberately excluded so rewrapping the same causal save does not create a different cloud revision.

## Envelope version 2

Envelope v2 appends the cloud revision fields to the existing manifest and protects them with the envelope checksum. The cloud flag must agree with whether revision metadata is present. The decoder rejects malformed clocks, excessive parents/devices, zero IDs/counters, mismatched canonical revision IDs, invalid flags, truncation, checksum failure, and trailing data.

Version-1 checksum and field order remain unchanged, so old local save files continue to decode. Existing callers that initialize only product, content, and build IDs remain source compatible; the cloud fields default to untracked.

## Causal comparison

For revisions in the same lineage, vector clocks provide deterministic ancestry classification:

- local clock strictly dominates cloud: local is a descendant
- cloud clock strictly dominates local: cloud is a descendant
- equal revision ID: both heads are identical
- neither clock dominates: the heads diverged

A merge revision dominates both parents because it takes the component-wise maximum and then increments the merging device.

`ResolveRunSaveCloudConflict` classifies missing, invalid, identical, descendant, divergent, unrelated-lineage, incompatible-manifest, and untracked records. The default divergence policy is `PreserveBoth`; the engine never silently overwrites two valid incomparable branches.

Optional policies are `PreferLocal`, `PreferCloud`, and `PreferDeterministicLatest`. Deterministic-latest is a stable product tie-break, not a semantic merge.

## Cloud transport contract

`IRunSaveCloudTransport` intentionally exposes only two operations:

```text
fetch(key)
compareExchange(key, expectedRevisionId, envelopeBytes)
```

`expectedRevisionId == 0` means create the remote object only when it does not yet exist. For an existing object, the upload succeeds only when the current remote head still equals the expected revision.

A compare-and-swap failure returns the latest remote object and revision. `SynchronizeRunSaveCloud` then reclassifies the local head against that current remote head. This closes the race between the initial fetch and upload without requiring the engine to know a provider-specific ETag, generation number, transaction API, or HTTP implementation.

A provider adapter may map the revision contract to:

- HTTP `If-Match` / ETag
- object-store generation preconditions
- database compare-and-swap
- Steam/Xbox/PlayStation cloud version tokens
- PlayFab or proprietary account storage revisions

The in-memory reference transport enforces:

- bounded valid keys
- valid checksummed envelopes
- mandatory tracked cloud revisions
- exact expected-revision matching
- product/content and lineage compatibility for replacement

The transport does not decide whether divergent progress should overwrite another branch. That decision belongs to the conflict policy and, normally, the user.

## Synchronization outcomes

`SynchronizeRunSaveCloud` returns one of:

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
- CAS race: return the latest remote head and updated conflict classification

No provider call mutates `RunSimulation`, `TowerDefenseSimulation`, or `RtsSimulation` directly.

## Conflict branch preservation and naming

`PreserveRunSaveCloudBranch` stores the original validated Envelope bytes under a deterministic branch filename:

```text
<base>--branch--<normalized-label>--<revision>.branch.sav
```

User labels are normalized to bounded lowercase ASCII alphanumeric/hyphen text. Empty labels fall back to `local` or `cloud`. Tracked records use the cloud revision ID as the suffix; untracked legacy records use the envelope checksum.

Because the exact Envelope bytes are preserved, creating a conflict branch does not change its revision, vector clock, payload, build metadata, or checksum. Repeating the same preservation request is idempotent.

Conflict branches are separate from `<slot>.sav` and `<slot>.recovery.sav`. Selecting a branch for active play is an explicit product action that should create a new child or merge revision rather than silently renaming history.

## Storage inventory and observability

`InspectRunSaveStorage` reports:

- total bytes
- primary, recovery, conflict-branch, and temporary bytes
- counts by file kind
- invalid envelope count
- per-file validity, envelope error, sequence, save Tick, revision ID, and base slot

This inventory is suitable for settings UI, telemetry, support diagnostics, and preflight capacity checks. It does not require decoding the nested authoritative world beyond the envelope and run-save validation already performed by the codec.

## Retention and capacity budgets

`RunSaveStoragePolicy` supports:

```text
maximum total storage bytes
maximum conflict-branch bytes
maximum conflict branches per base slot
remove temporary files
remove invalid conflict branches
```

Retention is deterministic and independent from wall-clock timestamps. Cleanup order is:

1. temporary files
2. invalid conflict branches
3. branches beyond the per-slot count, oldest canonical metadata first
4. oldest remaining branches until conflict and total byte budgets are satisfied

Primary and recovery files are never automatically selected for deletion. When protected primary/recovery bytes alone exceed the total budget, the plan reports that the budget remains unsatisfied rather than deleting recoverable gameplay state.

`ApplyRunSaveStorageRetentionPlan` removes each planned file through `IRunSaveDurability` and reports planned count, removed count, removed bytes, and per-file durability failures.

## Local sequence versus cloud revision

The local slot sequence still prevents stale asynchronous writes from replacing a newer local save. Cloud vector clocks and revision IDs handle cross-device causality. Neither mechanism replaces the other:

```text
slot sequence -> local write ordering
revision ID + vector clock -> cross-device ancestry
provider CAS -> race-free remote head update
```

## Acceptance tests

`rts_cloud_save_tests` covers Envelope compatibility and revision/conflict semantics.

`rts_cloud_transport_tests` adds:

- create-only remote upload
- descendant upload and download
- stale expected-revision rejection
- fetch-to-upload race injection and reclassification
- default divergent preserve-both behavior
- explicit prefer-local CAS replacement
- invalid key and missing cloud revision rejection
- deterministic user-labelled branch preservation
- idempotent branch writes
- real primary/recovery/branch/temporary inventory
- invalid and excess branch cleanup
- branch-byte budget enforcement
- proof that primary and recovery slots survive cleanup

## Remaining product work

A production transport still owns authentication, account authorization, retries, cancellation, offline queues, encryption in transit, provider quotas, and server-side storage. Upload APIs should always condition writes on the expected remote revision.

Future slices may add localized conflict actions, branch promotion/merge workflows, process-kill and suspend fault injection, scheduled cleanup, telemetry sinks, and optional authenticated encryption or signatures.
