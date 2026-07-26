# Target Architecture

## 1. Architectural goal

RTSEngine is a specialized runtime for deterministic 2D/2.5D RTS, tower-defense, base-building and roguelite games. The design optimizes for predictable simulation, many simple entities, dynamic map occupancy, reproducible runs and fast iteration on game content.

The engine is divided into four responsibility zones:

```text
Application composition
        ↓
Game and specialized frameworks
        ↓
Authoritative simulation kernel
        ↓
Foundation and platform services

Presentation reads simulation output beside this chain; it does not sit inside it.
```

## 2. Dependency rule

Dependencies always point downward or toward immutable contracts.

```text
apps
 ├── framework/rts
 ├── framework/tower_defense
 ├── framework/roguelite
 └── engine/presentation

framework/*
 ├── engine/sim
 ├── engine/ecs
 ├── engine/navigation
 └── engine/foundation

engine/presentation
 ├── engine/assets
 ├── engine/render
 ├── engine/audio
 └── engine/foundation

engine/sim
 ├── engine/ecs
 └── engine/foundation
```

Forbidden dependencies:

- simulation to render, audio, UI, platform windowing or wall-clock time
- framework/rts to a concrete game
- assets to a concrete renderer device
- UI directly to mutable simulation state
- animation events to authoritative damage, resource or spawn logic

These rules will be enforced by CMake target dependencies and source-boundary tests.

## 3. Authoritative simulation

The simulation runs at a fixed default rate of 30 Hz. It owns all gameplay time and advances only through explicit ticks.

```cpp
struct TickContext
{
    Tick tick;
    CommandView commands;
    DomainEventWriter events;
    RandomService& random;
    ScratchArena& scratch;
};
```

The simulation host owns:

- current tick
- command ingestion
- ordered system execution
- structural-change barriers
- deterministic random streams
- domain-event publication
- world hashing
- snapshot extraction
- save and replay state

Variable frame delta is never passed into gameplay systems.

## 4. Commands, events and snapshots

### GameCommand

A command is a validated request to mutate future authoritative state.

```text
Move, Attack, Stop, Hold, Patrol
Gather, Build, Repair
Train, CancelProduction, SetRally
StartWave, ChooseReward, CastAbility
```

Each command carries a target tick, issuer, monotonically increasing sequence and typed payload. Commands are sorted deterministically and rejected explicitly when invalid.

### DomainEvent

A domain event is a fact that has already occurred:

```text
ConstructionStarted
ProductionCompleted
WeaponFired
DamageApplied
EntityDied
WaveCompleted
RewardOffered
RewardChosen
```

Events are immutable after publication. Presentation consumes them through stable event IDs so replay and future rollback do not duplicate audiovisual effects.

### WorldSnapshot

A snapshot is a compact immutable view for presentation. It contains only renderable or UI-relevant state, never mutable component addresses.

Snapshots are double or triple buffered and can be interpolated independently of simulation rate.

## 5. ECS model

The ECS is deliberately limited:

- generational `EntityId`
- typed sparse-set component pools
- deterministic multi-component views
- explicit stage scheduler
- typed structural command buffer
- component schemas for save, hash and migration

It will not initially include archetype migration, runtime reflection-heavy queries, nested object ownership or script-defined component layouts.

Stable simulation stages:

```text
Command
Reservation
ConstructionAndProduction
OrdersAndAI
NavigationRequest
NavigationCommit
Movement
SpatialIndex
Targeting
Combat
DamageResolve
DeathCleanup
WaveAndRunProgression
Snapshot
```

Structural changes are committed only at declared barriers. A system cannot directly create or destroy entities while iterating.

## 6. RTS framework

The RTS framework owns reusable domain mechanics:

- selection-independent unit commands
- order queues and interrupt policy
- teams and diplomacy
- resource ledger and reservation transactions
- workers, gathering and drop-off
- building placement and construction
- production, research, population and rally points
- movement agents and formations
- spatial queries and targeting
- weapons, projectiles, damage and status effects
- vision and fog of war

Game-specific units, factions, weapons and balance values remain content definitions outside the framework.

## 7. Tower-defense framework

Tower-defense capabilities build on RTS systems:

- lane and spawn graph
- deterministic budget-based wave director
- base-core objectives
- threat and influence maps
- path-blocking placement validation
- wave modifiers, bosses and inter-wave phases

A tower is normally an RTS building with targeting and weapon components; it is not a separate engine object hierarchy.

## 8. Roguelite framework

State is divided into:

```text
MatchState  - current map and entities
RunState    - seed, wave, run currency, chosen modifiers and history
MetaState   - long-term unlocks and profile data
```

The first release focuses on `RunState`; `MetaState` remains application-owned until product requirements stabilize.

Rewards are data-driven and use selectors, prerequisites, exclusions, rarity, stack limits, pity rules and deterministic offer generation.

## 9. Navigation

Navigation uses a layered grid:

```text
terrain walkability and cost
static blockers
dynamic building footprints
temporary reservations
clearance
threat influence
vision data
```

Algorithms are selected by workload:

- A* for individual requests
- cached paths for repeated routes
- flow fields for groups sharing a destination
- lane graphs for stable enemy routes
- hierarchical regions only when map scale requires them

Asynchronous pathfinding is allowed, but results are committed at tick boundaries in request-ID order. Worker completion timing must never change simulation results.

## 10. Presentation

Presentation owns:

- snapshot interpolation
- entity-to-view binding
- sprite and skeletal animation sampling
- visual projectiles and particles
- audio event playback
- selection highlights, health bars and world UI
- camera, screen UI and debug overlays

Rendering begins with fixed lightweight 2D passes:

```text
Terrain
WorldShadow
WorldEntity
ProjectileAndEffect
FogOfWar
SelectionAndDecal
WorldUI
ScreenUI
Debug
```

The rendering API remains backend-neutral. Sokol may be used as an implementation aid, but no Sokol handle crosses the public engine boundary.

## 11. Asset architecture

One asset manager owns all content lifecycle:

- virtual file system
- canonical asset key `(type, id)`
- cooked formats and schema versions
- dependency graph
- CPU and GPU resource budgets
- async loading and cancellation
- hot reload in development builds

Logical assets reference image or material IDs, not GPU handles. GPU resources are resolved by presentation caches tied to a render device generation.

Initial optional third-party dependencies:

- FreeType for font rasterization
- HarfBuzz for shaping
- Expat or another small XML parser only if XML content is retained
- libtess2 for vector tessellation if needed
- ryu for deterministic numeric text conversion
- Sokol for platform/render backend simplification

Every dependency must be replaceable behind an engine-owned interface.

## 12. Determinism contract

Authoritative state follows these rules:

- fixed-width integer or fixed-point values for gameplay-critical math
- explicit iteration order
- no pointer values in save or hash data
- no unordered-container iteration in authoritative logic
- named RNG streams derived from root seed and stream ID
- canonical little-endian field-by-field serialization
- versioned component schemas
- no system time, locale or device-dependent input during a tick

The same seed and command stream must produce the same world hash across Debug/Release and supported compilers.

## 13. Threading policy

The initial simulation is single-threaded. Parallelism is introduced only for isolated jobs with deterministic commit points:

- path search
- visibility computation
- immutable spatial queries
- asset decoding
- snapshot preparation

Job-system complexity is not allowed inside gameplay rules until profiling proves it necessary.

## 14. Performance baseline

Initial product budget at 30 Hz:

```text
1000 active units
200 buildings
1500 authoritative projectiles
4 concurrent enemy lanes
simulation tick p95 below 10 ms on the target desktop baseline
no heap allocation in warmed core tick paths
```

Budgets are verified with reproducible headless benchmark scenes rather than isolated microbenchmarks alone.

## 15. Public API policy

Public APIs use engine-owned C++17 types. Standard-library types may be used internally, but ownership and ABI boundaries remain explicit.

A small set of native containers and handles may be introduced where determinism, allocation control, serialization or ABI stability justifies them. The engine will not recreate the entire standard library merely to imitate another commercial engine.
