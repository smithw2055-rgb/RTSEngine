# Cloud Save Revision and Conflict Contract

## Scope

Cloud synchronization operates on validated `RunSaveEnvelope` records. The simulation archive remains unchanged: cloud metadata is envelope-level product state used to compare two save heads before deciding whether to upload, download, preserve both, or reject them.

The local storage and cloud comparison chain is:

```text
RunSaveSlotStore
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

Envelope v2 appends the cloud revision fields to the existing manifest and protects them with the envelope checksum. The cloud flag must agree with whether revision metadata is present. The decoder rejects:

- malformed or noncanonical vector clocks
- more than two parents or more than 32 devices
- zero IDs/counters in tracked records
- revision IDs that do not match the canonical payload-derived value
- cloud flags without metadata or metadata without the cloud flag
- checksum, payload, or manifest mismatches

Version-1 checksum and field order remain unchanged, so old local save files continue to decode.

`RunSaveManifestIdentity` carries optional cloud revision metadata when a save is written. Existing callers that initialize only product, content, and build IDs remain source compatible; the cloud fields default to untracked.

## Causal comparison

For revisions in the same lineage, vector clocks provide deterministic ancestry classification:

- local clock strictly dominates cloud: local is a descendant and should upload
- cloud clock strictly dominates local: cloud is a descendant and should download
- equal revision ID: both heads are identical
- neither clock dominates: the heads diverged

A newly introduced device produces a new vector-clock component. A merge revision dominates both parents because it takes the component-wise maximum and then increments the merging device.

## Conflict classification

`ResolveRunSaveCloudConflict` classifies:

```text
both missing
local only / cloud only
identical revision
equivalent untracked payloads
local descendant / cloud descendant
diverged
ancestry unavailable
unrelated lineage
incompatible product/content manifest
local invalid / cloud invalid / both invalid
```

The default divergence policy is `PreserveBoth`. The engine never silently overwrites two valid incomparable branches.

Optional policies are:

- `PreferLocal`
- `PreferCloud`
- `PreferDeterministicLatest`

The deterministic-latest policy compares, in order:

1. total vector-clock progress
2. save Tick
3. slot sequence
4. authoring device logical clock
5. revision ID
6. device ID

This tie-break is deterministic but is a product policy, not a semantic merge. Products should normally preserve both branches and ask the user when divergent progress is valuable.

## Compatibility and missing metadata

Product ID and content-manifest ID must match before ancestry is considered. Different content manifests are rejected instead of compared by Tick or revision.

When one or both valid envelopes lack cloud metadata:

- byte-identical payloads are equivalent
- different payloads are classified as `AncestryUnavailable`
- the safe default is to preserve both

This allows pre-cloud and version-1 saves to coexist without inventing false ancestry.

## Local slot integration

`RunSaveSlotStore` already passes `RunSaveManifestIdentity` into the envelope codec. Supplying a root/child/merge revision in `identity.cloud` therefore writes cloud metadata directly into the primary and recovery envelopes; no sidecar file is required.

A caller normally:

1. loads and decodes the current local/cloud heads
2. creates a child revision from the selected parent, or a two-parent merge revision
3. writes the new local slot with a higher local slot sequence
4. uploads the exact validated envelope bytes

The local slot sequence still prevents stale asynchronous local writes. Cloud vector clocks and revision IDs handle cross-device causality; they do not replace the local sequence contract.

## Acceptance tests

`rts_cloud_save_tests` covers:

- Envelope v1 backward compatibility
- root, child, divergent, and two-parent merge revisions
- exact vector-clock dominance in both directions
- identical and unrelated revisions
- default preserve-both behavior
- deterministic divergence tie-breaking
- incompatible content manifests
- untracked equivalent and untracked conflicting payloads
- invalid and missing candidates
- cloud metadata round-trip through `RunSaveSlotStore`

## Remaining product work

Cloud transport, authentication, account ownership, server-side compare-and-swap, retention, and user-facing branch naming remain product/platform responsibilities. A production upload API should condition writes on the expected remote revision ID; otherwise two clients can still race between classification and upload.
