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

The authoritative simulation layer does not depend on rendering, audio, UI, scripting, networking, or platform services. See `docs/architecture.md`, `docs/roadmap.md`, and `docs/adr/` for the design constraints and phased plan.
