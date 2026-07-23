# RTSEngine

RTSEngine is a lightweight C++17 engine specialized for deterministic RTS, tower-defense, base-building, and roguelite games.

The repository is being built as small, testable vertical slices rather than importing a legacy general-purpose engine wholesale.

## Current capabilities

- fixed 30 Hz authoritative simulation with bounded catch-up
- canonical state hashing and named deterministic random streams
- generational entities, sparse-set components, stable queries, and staged structural changes
- tick command stream, domain events, immutable snapshots, and replay-ready ordering
- queued move, stop, attack, attack-move, and hold-position commands
- deterministic grid A* with dynamic blockers and stable tie-breaking
- deterministic tower-defense lane graphs with registration-order-independent route selection
- deterministic multi-wave tower-defense loops with preparation countdowns, early starts, bosses, affixes, victory/failure states, and mid-preparation persistence
- authoritative roguelite run history with wave results, boss/affix outcomes, resources, core health, reward choices, and legacy save migration
- deterministic reward rarity budgets, minimum-rarity guarantees, and persisted pity counters
- immutable presentation scenes extracted from RTS snapshots with logical asset bindings and fog visibility filtering
- double-buffered snapshot interpolation with spawn, despawn and teleport policies
- backend-neutral platform, render and audio contracts with generational handles and Null backends
- fixed 2D passes, cooked sprite batching, world UI quads and device-generation recovery
- stable animation, effect and audio event consumption with replay-safe deduplication
- virtual file systems, versioned cooked assets, dependency loading, cancellation, CPU budgets and transactional hot reload
- optional Sokol render-device implementation behind engine-owned interfaces
- transactional resources, construction, cancellation, production queues, and rally points
- teams, health, armor, weapons, fixed-tick cooldowns, spatial targeting, buffered damage, and deterministic death cleanup
- kill bounty rewards and automatic navigation blocker release when buildings or construction sites are destroyed
- Linux and Windows Debug/Release CI coverage

## Build

```bash
cmake -S . -B build -DRTSENGINE_BUILD_TESTS=ON -DRTSENGINE_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The optional Sokol backend is enabled by providing a Sokol include directory:

```bash
cmake -S . -B build \
  -DRTSENGINE_ENABLE_SOKOL_RENDERER=ON \
  -DRTSENGINE_SOKOL_INCLUDE_DIR=/path/to/sokol
```

The authoritative simulation layer does not depend on rendering, audio, UI, scripting, networking, or platform services. See `docs/architecture.md`, `docs/roadmap.md`, `docs/phase7-presentation-foundations.md`, and `docs/adr/` for the design constraints and phased plan.
