# Implementation Roadmap

The roadmap is organized around compileable, testable slices. Each phase must leave the repository in a usable state and must not depend on future phases to become coherent.

## Phase 0 — Architecture baseline

Deliverables:

- product scope and non-goals
- target module graph
- architecture decision records
- repository and CMake target layout
- coding, testing and dependency rules
- empty headless application target

Exit criteria:

- dependency directions are documented
- old code is not imported wholesale
- all future modules have named owners and responsibilities

## Phase 1 — Foundation and deterministic simulation

Deliverables:

- fixed-width types, `Tick`, generational handles and result/error model
- fixed-step clock with explicit backlog policy
- fixed-point scalar/vector types
- canonical binary writer and hash writer
- named deterministic random streams
- typed command stream and domain-event stream
- `SimulationHost` and headless loop

Tests:

- interpolation remains in `[0, 1]`
- identical RNG streams restore from saved state
- different stream IDs produce independent sequences
- canonical serialization is padding- and endianness-independent
- same seed and command stream reproduce the same hash

## Phase 2 — ECS and scheduling

Deliverables:

- entity registry with generation protection
- typed sparse-set component storage
- deterministic multi-component views
- component schema registry
- stage scheduler with execution ordinals
- typed entity command buffer and deferred entities
- structural barriers

Tests:

- stale entity IDs cannot access recycled slots
- structural operations preserve system execution order
- future-stage commands are not committed early
- ordered views are stable after creation and deletion
- component save/load/hash functions are schema-driven

## Phase 3 — First RTS vertical slice

Scope:

- one worker
- one resource type and resource node
- one base building
- one production unit
- one tower
- one enemy
- one map

Deliverables:

- command validation
- order queue
- gather/drop-off cycle
- transactional resource reservation
- building placement and construction
- production queue and rally point
- movement, targeting, weapons, damage and death
- immutable presentation snapshot

Exit criteria:

A headless scenario gathers resources, constructs a tower, trains a unit, defeats enemies and produces an identical final hash in repeated runs.

## Phase 4 — Navigation and scale

Deliverables:

- layered navigation grid
- deterministic A*
- reusable path-search scratch memory
- dynamic building footprints
- path cache and invalidation
- flow fields for shared destinations
- lane graph for enemy waves
- simple local separation and occupancy reservation
- fixed-grid spatial index

Tests and benchmarks:

- placing and removing buildings updates reachability
- illegal full path blocking is rejected where required
- asynchronous results commit in request-ID order
- 1000-agent movement benchmark has stable allocation and timing

## Phase 5 — Tower defense and wave loop

Deliverables:

- spawn lanes and base core
- wave budget and enemy pool
- deterministic wave composition
- inter-wave preparation state
- bosses and wave affixes
- threat/influence data
- wave completion and failure conditions

Exit criteria:

The vertical slice runs several waves, including a boss, without game-specific logic entering engine foundations.

## Phase 6 — Roguelite run system

Deliverables:

- `RunState`
- reward catalog
- deterministic reward offers
- prerequisites, exclusions, rarity and stack limits
- stat and rule modifiers
- run history and reward events
- run save/load

Exit criteria:

A saved run resumes with the same command outcome, RNG state, wave plan and reward sequence.

## Phase 7 — Presentation and GPU runtime

Status: complete for the playable desktop runtime. See `phase7-playable-desktop-runtime.md`.

Deliverables:

- platform abstraction
- render-device abstraction
- Sokol-backed desktop implementation where practical
- 2D render passes and sprite batching
- snapshot interpolation
- animation/effect/audio event consumption
- asset manager, VFS and cooked content
- font and UI foundations

Rules:

- simulation remains buildable and testable without presentation backends
- no GPU handle enters simulation or logical asset formats
- visual frame rate cannot change authoritative state

## Phase 8 — Save, replay, tools and content workflow

Deliverables:

- versioned save container
- replay recording and playback
- world-hash diagnostics
- content validation tool
- headless balance runner
- map and definition conversion pipeline
- development hot reload

Exit criteria:

A reported gameplay bug can be reproduced from seed, content version and replay command stream.

## Phase 9 — Product hardening

Deliverables:

- benchmark scenes and performance telemetry
- memory budgets
- long-run soak tests
- fuzz tests for cooked files and saves
- Windows, Linux and selected mobile backend validation
- crash diagnostics and user-safe save recovery

Networking remains out of scope until the complete single-player replay path is deterministic at product scale.

## Commit strategy

Prefer small vertical commits rather than large subsystem dumps. Typical sequence:

```text
build: establish C++17 target graph
feat(foundation): add generational handles
feat(sim): add fixed-tick host
feat(sim): add named random streams
feat(ecs): add component pools and ordered views
feat(rts): add transactional resource ledger
feat(rts): add construction order
...
```

Each feature commit must include relevant tests or explain why no executable behavior changed.
