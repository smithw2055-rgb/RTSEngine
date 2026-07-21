# RTSEngine

RTSEngine is a lightweight C++17 engine and gameplay framework dedicated to:

- real-time strategy
- tower defense
- base building
- lightweight roguelite progression

It is not intended to become a general-purpose 3D engine. The project prioritizes deterministic simulation, large numbers of simple agents, dynamic navigation blockers, data-driven content, headless testing, replayability, and a clean separation between authoritative gameplay and presentation.

## Product direction

The initial target is a single-player 2D or 2.5D game with the following loop:

```text
prepare / explore
    -> gather and build
    -> produce and defend
    -> enemy wave
    -> reward / repair / upgrade
    -> next wave or boss
```

The engine must comfortably support a vertical slice containing:

- workers and resource collection
- building placement and construction
- production queues and rally points
- hundreds of moving units
- towers, projectiles, damage and status effects
- dynamic path blocking
- deterministic enemy waves
- roguelite reward choices
- save, load and replay

## Core principles

1. **C++17 only.** C++20 features are not required by the public engine contract.
2. **Authoritative fixed-tick simulation.** Gameplay never depends on variable frame time.
3. **Simulation and presentation are separate.** Rendering, UI, audio and particles consume snapshots and events; they never mutate authoritative state directly.
4. **Data-oriented where it matters.** ECS-style component storage is used for high-volume simulation, without building a highly abstract general-purpose ECS.
5. **Determinism is a product feature.** Replay, headless testing, seeded runs and reproducible bugs are designed in from the beginning.
6. **Specialized before generic.** RTS, tower-defense and roguelite capabilities live in explicit frameworks rather than being hidden inside generic managers.
7. **Single-player first.** Networking is deferred until replay determinism is proven at product scale.
8. **Incremental delivery.** Every milestone must compile, run and include automated tests.

## Planned repository structure

```text
RTSEngine/
├── engine/
│   ├── foundation/       # types, memory, containers, diagnostics, serialization
│   ├── sim/              # tick host, command stream, events, RNG, replay, snapshots
│   ├── ecs/              # entities, component storage, deterministic views, scheduler
│   ├── navigation/       # grid, A*, flow fields, dynamic blockers
│   ├── assets/           # VFS, cooked assets, importers, cache and hot reload
│   ├── platform/         # window, input, files, threads, time
│   ├── render/           # lightweight GPU abstraction and 2D rendering
│   ├── audio/            # audio device and event playback
│   └── presentation/     # snapshot interpolation, animation, effects and view state
├── framework/
│   ├── rts/              # orders, economy, construction, production, combat, vision
│   ├── tower_defense/    # lanes, waves, threat, base core and path validation
│   └── roguelite/        # run state, rewards, modifiers and progression rules
├── apps/
│   ├── headless_demo/
│   └── sandbox/
├── tests/
├── tools/
├── docs/
└── third_party/
```

## High-level runtime flow

```text
Platform input
    -> PlayerIntent
    -> validated GameCommand
    -> TickCommandStream
    -> SimulationHost::Step(TickContext)
    -> DomainEvents + immutable WorldSnapshot
    -> PresentationHost
    -> Render / Audio / UI
```

## Development phases

1. Architecture baseline and repository skeleton
2. Deterministic simulation kernel
3. Entity/component runtime and structural barriers
4. RTS vertical slice: gather, build, produce, move and fight
5. Tower-defense waves and dynamic navigation
6. Roguelite rewards and run state
7. Presentation, rendering, audio and tools
8. Save/load, replay, performance budgets and product hardening

See [Architecture](docs/architecture.md), [Roadmap](docs/roadmap.md), and the [Architecture Decision Records](docs/adr/README.md).

## Status

The project is currently in the architecture-baseline phase. No old StarCraft implementation is imported wholesale; useful concepts will be migrated only after their boundaries and deterministic behavior are defined.
