# Phase 7 Playable Desktop Runtime

## Status

Phase 7 now has a complete playable desktop composition. The deterministic
simulation remains backend-independent, while the optional Sokol application
connects native window events, fixed-Tick simulation, presentation extraction,
minimal HUD rendering, cooked assets and a GPU swapchain.

The default build still has no external graphics dependency. Sokol targets are
created only when both `RTSENGINE_ENABLE_SOKOL_RENDERER` and
`RTSENGINE_ENABLE_SOKOL_APP` are enabled with a pinned Sokol include directory.

## Runtime chain

```text
sokol_app event callback
    -> SokolAppPlatform
    -> InputState
    -> DesktopController
    -> validated TickCommand values
    -> RunSimulation / TowerDefense / RTS
    -> immutable WorldSnapshot and DomainEvent values
    -> RtsPresentationRuntime
    -> snapshot interpolation and playback cues
    -> RenderPacket + UiDrawList
    -> Fixed2DRenderer
    -> SokolRenderDevice
    -> Sokol swapchain
```

No platform event, presentation clock, GPU handle, animation state or UI state
enters authoritative simulation state or world hashes.

## Modules

### `engine/platform`

The engine-owned platform event contract now covers:

- key press/release and repeat state
- UTF-32 text input
- pointer movement, buttons and wheel input
- touch begin/move/end
- window resize, DPI, focus and quit events
- dropped-file paths

`InputState` converts an ordered event stream into frame-local pressed/released
edges plus persistent keyboard, pointer and touch state. `NullPlatform` can
inject the same events for tests. `SokolAppPlatform` is an optional adapter and
keeps all `sokol_app.h` types outside the core platform API.

### `engine/runtime`

`DesktopFrameLoop` owns the presentation frame sequence:

1. poll platform events and update `InputState`
2. calculate bounded frame delta
3. invoke application input/HUD preparation
4. advance zero or more fixed simulation ticks
5. render once with the fixed-step interpolation alpha
6. clear frame-local input edges

The loop supports authoritative Tick resynchronization after loading a saved
run. Variable presentation delta is never passed to gameplay systems.

### `engine/presentation`

Phase 7 adds:

- an embedded ASCII bitmap font atlas with no runtime font dependency
- pixel-coordinate `UiDrawList` primitives
- panels, labels, buttons, progress bars and outlines
- `ScreenUi` batching through the existing fixed 2D renderer
- world overlay quads for selection, placement and drag rectangles
- per-kind fallback visual bindings for data-defined entities
- a Sokol fixed 2D shader-description module compiled separately from the
  application entry point

The minimal UI is intentionally a HUD toolkit rather than a general desktop UI
framework. Complex menus and editor tooling can integrate a dedicated UI layer
without changing the simulation boundary.

### `framework/rts_presentation`

The former header-only presentation adapter is now a compiled library. It owns:

- RTS snapshot-to-scene extraction
- stable RTS event-to-presentation-event conversion
- scene buffering and interpolation
- visual/event catalogs
- `DesktopController`

`DesktopController` implements click and box selection, camera pan/zoom,
visibility-aware right-click move/attack commands, attack-move, stop,
hold-position and tower placement mode. It only emits normal deterministic RTS
commands; it cannot mutate the world directly.

### `framework/rts_desktop`

`PlayableDesktopRuntime` composes a self-contained three-wave RTS/tower-defense
Roguelite session with:

- a player core and defenders
- an enemy lane and deterministic waves
- tower construction
- click/drag selection and right-click commands
- reward selection
- procedural cooked texture and sprite assets
- fixed 2D world rendering and a minimal HUD
- in-memory authoritative save and restore
- presentation reset and Tick-clock resynchronization after restore

The same runtime is exercised with Null platform/render/audio backends in CI,
so the complete application state machine remains testable without a display.

## Building the default runtime

```bash
cmake -S . -B build \
  -DRTSENGINE_BUILD_TESTS=ON \
  -DRTSENGINE_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Building the Sokol desktop application

Use the pinned Sokol revision documented in CI and provide a directory
containing:

```text
sokol_app.h
sokol_gfx.h
sokol_glue.h
sokol_log.h
```

Then configure:

```bash
cmake -S . -B build-sokol \
  -DCMAKE_BUILD_TYPE=Release \
  -DRTSENGINE_BUILD_TESTS=ON \
  -DRTSENGINE_BUILD_EXAMPLES=ON \
  -DRTSENGINE_ENABLE_SOKOL_RENDERER=ON \
  -DRTSENGINE_ENABLE_SOKOL_APP=ON \
  -DRTSENGINE_SOKOL_INCLUDE_DIR=/path/to/sokol
cmake --build build-sokol --target rts_desktop_demo --parallel
```

Linux requires X11, Xi, Xcursor and OpenGL development packages. The desktop
CI job installs them before compiling the executable.

## Controls

| Input | Action |
|---|---|
| Left click | Select one friendly unit/building |
| Left drag | Box-select friendly units/buildings |
| Right click ground | Move selected units |
| Right click visible enemy | Attack selected target |
| Middle drag / arrow or WASD keys | Pan camera |
| Mouse wheel | Zoom camera |
| `B` | Enter tower-placement mode |
| `A` | Enter attack-move mode |
| `S` | Stop selected units |
| `H` | Hold position |
| `Space` | Start a run/wave when available |
| `1`–`3` | Choose a pending Roguelite reward |
| `F5` | Save the authoritative run in memory |
| `F9` | Restore the in-memory save |
| `Escape` | Cancel the current interaction mode |

## Validation

The cross-platform test suite includes dedicated coverage for:

- input edge/state transitions and Null-platform event injection
- HUD/font geometry and Screen UI batching
- click/box selection and deterministic command generation
- fixed-step desktop frame ordering and backlog limits
- playable runtime rendering, commands, save/restore and resumed frames
- optional Sokol shader descriptor contracts

The normal Linux/Windows Debug/Release matrix remains dependency-free. A
separate Ubuntu Sokol smoke build compiles the platform adapter, renderer,
shader module, application composition and final desktop executable.

## Phase boundary

Phase 7 is complete at the engine-runtime level. The following concerns remain
outside this phase:

- source asset conversion and generalized hot-reload tooling (Phase 8)
- replay inspection and content validation tools (Phase 8)
- a production audio backend and product-specific mixing policy
- long-run GPU, soak, crash and memory-budget validation (Phase 9)
- product-specific UI/editor integration
