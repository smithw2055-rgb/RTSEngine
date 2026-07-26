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

`RunSaveCloudRevision` stores lineage, canonical revision, authoring device, logical clock, zero to two direct parents, and a bounded vector clock. Vector-clock dominance identifies descendants; incomparable clocks identify split branches. The default conflict action is `PreserveBoth`.

`IRunSaveCloudTransport` exposes `fetch(key)` and `compareExchange(key, expectedRevisionId, envelopeBytes)`. An expected revision of zero means create only when absent. A mismatch returns the latest remote object, and `SynchronizeRunSaveCloud` reclassifies the local head against it.

`PreserveRunSaveCloudBranch` stores exact validated envelope bytes using deterministic path-safe branch names. `InspectRunSaveStorage` reports bytes, counts, validity, sequence, Tick, revision, and base slot. `RunSaveStoragePolicy` removes temporary files, invalid branches, per-slot excess branches, and oldest branches until configured budgets are met. Primary and recovery slots are never automatically removed.

`rts_cloud_transport_tests` covers create-only upload, descendant upload/download, stale revision rejection, fetch-to-upload race injection, divergent preserve-both, explicit local preference, invalid input, named/idempotent branch preservation, inventory, cleanup budgets, and primary/recovery protection.

Production adapters still own authentication, retries, cancellation, offline queues, transport encryption, provider quotas, server storage, and conflict UX.
