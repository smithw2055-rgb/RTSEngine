# Phase 7 Presentation Foundations

This slice establishes the first three Phase 7 deliverables without introducing a concrete graphics backend.

## Module boundaries

```text
engine/platform
    engine-owned window, event and presentation-clock contracts

engine/render
    backend-neutral device/resource/draw contracts
    NullRenderDevice for CI and validation

engine/presentation
    immutable PresentationScene
    double-buffered snapshot interpolation
    backend-neutral RenderPacket

framework/rts_presentation
    WorldSnapshot -> PresentationScene adapter
    composed RtsPresentationRuntime
```

The authoritative RTS, tower-defense and roguelite targets do not depend on any of these targets. The bridge target depends on both `RTSEngine::Rts` and `RTSEngine::Presentation`, so the core simulation framework remains independently buildable.

## PresentationScene

`PresentationScene` is a value-only contract containing:

- source simulation tick and world hash
- stable 64-bit view IDs derived from generational entity IDs
- logical sprite and animation asset IDs
- render layer and stable sort bias
- position, team, health and construction progress
- observed-team visibility and exploration masks

It never contains ECS component pointers, mutable world references, platform handles or GPU handles.

`RtsPresentationExtractor` maps `gameplay::WorldSnapshot` into a canonical scene sorted by `ViewId`. Enemy visibility is filtered using the observed team's current visibility mask.

## Interpolation

`PresentationSceneBuffer` retains the previous and current immutable scenes and rejects non-canonical or non-increasing publications.

Sampling produces an `InterpolatedScene` with explicit lifecycle states:

- `Stable`: entity exists in both snapshots
- `Spawned`: entity only exists in the current snapshot
- `Despawned`: entity only exists in the previous snapshot

Stable entities interpolate position. Spawned and despawned entities receive alpha-based opacity. Movement above the configured teleport distance snaps to the current position instead of crossing the map visually.

Interpolation uses presentation frame time only. It cannot mutate simulation state or alter the source world hash.

## RenderPacket

`RenderPacketBuilder` converts an interpolated scene into logical draw data:

- sprite instances sorted by render layer, world Y, sort bias and ViewId
- health bars
- construction progress bars
- visibility masks for the future fog pass

Packets still contain logical asset IDs rather than device textures or pipelines. A future renderer resolves those IDs through device-owned resource caches.

## Platform abstraction

`Platform` defines engine-owned contracts for:

- window creation and destruction
- logical and framebuffer sizes
- DPI scale
- focus, resize and quit events
- presentation-only monotonic time

`NullPlatform` provides generational window handles, queued synthetic events and a manually advanced clock for CI. The monotonic clock is explicitly forbidden in authoritative simulation.

## RenderDevice and NullRenderDevice

`RenderDevice` defines engine-owned generational handles for buffers, textures and pipelines, plus frame and draw submission contracts.

`NullRenderDevice`:

- validates descriptors and draw handles
- records completed frames
- invalidates stale handles after destruction
- invalidates every handle on device reset
- increments the device generation on reset
- requires no graphics API or native window handle

No Sokol, Direct3D, OpenGL or other backend object appears in the public contracts.

## Current application flow

```cpp
rts::rts_presentation::RtsPresentationRuntime runtime;
runtime.registerVisual(binding);

runtime.publishSnapshot(simulation.snapshot());
auto packet = runtime.buildRenderPacket(interpolationAlpha);
```

A later desktop application will translate `RenderPacket` into `RenderDevice` draw commands. That translation is intentionally outside this slice.

## Tests

The slice includes dedicated cross-platform tests for:

- RTS snapshot extraction and fog visibility filtering
- canonical entity ordering and logical asset binding
- double-buffer publication and stale snapshot rejection
- stable, spawned, despawned and teleported views
- render packet sorting and world UI extraction
- null platform events, DPI and window generations
- null render resource generations, frame validation and device reset

## Next Phase 7 boundary

The next slice can implement the fixed 2D pass executor and sprite batch compiler that translate `RenderPacket` into backend-neutral `DrawCommand` groups. A Sokol device implementation should follow only after that command boundary is stable.
