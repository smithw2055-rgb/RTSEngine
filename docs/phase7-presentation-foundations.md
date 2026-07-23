# Phase 7 Presentation and GPU Runtime

This phase connects the deterministic simulation to replaceable presentation backends without allowing frame timing, GPU resources, animation or audio to enter authoritative state.

## Module boundaries

```text
engine/platform
    engine-owned window, event and presentation-clock contracts

engine/assets
    VFS, versioned cooked content, dependency loading, CPU budgets and hot reload

engine/audio
    backend-neutral playback commands and NullAudioDevice

engine/render
    backend-neutral device/resource/upload/draw contracts
    NullRenderDevice for CI and validation
    optional RenderSokol implementation

engine/presentation
    immutable PresentationScene and interpolation
    stable audiovisual event consumption
    logical-to-GPU presentation asset cache
    fixed 2D pass and sprite batch runtime

framework/rts_presentation
    WorldSnapshot -> PresentationScene adapter
    RTS DomainEvent -> stable PresentationEvent adapter
    composed RtsPresentationRuntime
```

The authoritative RTS, tower-defense and roguelite targets do not depend on platform, assets, audio, render or presentation. GPU handles and presentation wall-clock time are therefore excluded from commands, saves, snapshots and world hashes.

## PresentationScene and interpolation

`PresentationScene` is a value-only contract containing source tick/hash, stable view IDs, logical sprite and animation IDs, positions, team data, health, construction progress and visibility masks. It never contains mutable ECS references or GPU resources.

`PresentationSceneBuffer` retains previous/current canonical scenes and produces explicit `Stable`, `Spawned` and `Despawned` views. Stable positions interpolate, spawned/despawned views fade, and movement above the configured teleport distance snaps instead of crossing the map visually.

## RenderDevice and backends

`RenderDevice` owns generational buffer, texture and pipeline handles. The public contract now includes:

- dynamic buffer and texture upload
- explicit vertex layouts and index types
- fixed render-pass classification
- blend, filter and address modes
- frame clear state and ordered draw submission
- device generation reset

`NullRenderDevice` stores uploaded bytes, validates descriptors, buffer ranges and resource generations, records submitted frames, and sorts recorded commands by fixed pass and sort key.

### Optional Sokol backend

`RTSEngine::RenderSokol` is built only when explicitly enabled:

```bash
cmake -S . -B build \
  -DRTSENGINE_ENABLE_SOKOL_RENDERER=ON \
  -DRTSENGINE_SOKOL_INCLUDE_DIR=/path/to/sokol
```

The application supplies callbacks for `sg_environment`, the `sg_swapchain` associated with an engine `WindowHandle`, and shader descriptors generated for known engine `ShaderKey` values. Sokol types remain confined to the optional backend target; no Sokol handle crosses the engine render interface.

The default CI matrix intentionally builds the backend-neutral and Null implementations without downloading a graphics dependency.

## Fixed 2D passes and sprite batching

The initial pass order is fixed:

```text
Terrain
WorldShadow
WorldEntity
ProjectileAndEffect
FogOfWar
SelectionAndDecal
WorldUi
ScreenUi
Debug
```

`SpriteBatchCompiler` resolves logical sprite IDs, converts world-space quads to camera-relative NDC vertices, creates indexed quads, emits health/construction bars, and combines adjacent sprites sharing pass, blend mode and texture.

`Fixed2DRenderer` uploads one vertex stream and one index stream per frame, then submits backend-neutral `DrawCommand` batches through the render device. It maintains four sprite pipelines for opaque, alpha, additive and multiply blending, rebuilds resources after device-generation changes, and reports draw/quad/upload statistics.

## Presentation asset cache

`PresentationAssetCache` is the only bridge from logical assets to GPU textures:

```text
Logical Sprite ID
    -> Cooked Sprite
    -> Cooked Texture dependency
    -> RenderDevice TextureHandle
```

The cache tracks both AssetManager generation and RenderDevice generation. Content hot reload rebuilds only the affected texture; device reset invalidates handles and lazily recreates them. Logical cooked assets never store device handles.

## Stable animation, effect and audio events

Authoritative domain events are converted into `PresentationEvent` values with stable IDs derived from domain, tick, event ordinal and payload. A bounded consumer remembers processed IDs so replaying the same event does not duplicate audiovisual output.

Bindings can map an event to:

- an entity animation clip
- a world effect
- an audio clip

`PresentationPlaybackRuntime` samples cooked animation frames, inserts effect sprites into the projectile/effect pass, applies additive blending when requested, expires effects by presentation time, and submits audio commands to an `AudioDevice`. None of these operations can produce damage, resources, spawning or other authoritative changes.

`NullAudioDevice` records validated playback commands and provides generational voice handles for cross-platform tests.

## VFS, AssetManager and cooked content

The asset runtime uses canonical `(AssetType, id)` keys and path-normalized virtual filesystems:

- `MemoryVfs` for tests and generated content
- `MountedVfs` for layered roots such as base content and DLC
- `DirectoryVfs` for guarded filesystem roots

The `RTA1` cooked container stores its asset key, schema version, canonical dependency list, payload hash and payload bytes. Initial typed payload schemas cover textures, sprites, animation clips, effects and PCM audio.

`AssetManager` provides:

- request handles, cancellation and bounded processing
- recursive dependency loading and cycle detection
- key, schema and payload-hash validation
- retain/release and dependency pinning
- deterministic least-recently-used CPU-budget eviction
- transactional hot reload with generation increments
- runtime statistics

The API is synchronous at the execution point today, but requests and `process(maximumRequests)` establish the cancellation and scheduling boundary needed for a future worker-backed decoder. Worker completion timing must not change publication order.

## Current application flow

```cpp
rts::rts_presentation::RtsPresentationRuntime scenes;
rts::presentation::PresentationPlaybackRuntime playback(assets, audio);
rts::presentation::PresentationAssetCache assetCache(assets, renderDevice);
rts::presentation::Fixed2DRenderer renderer(renderDevice, assetCache);

scenes.publishSnapshot(simulation.snapshot(), simulation.events());
auto cues = scenes.consumeCues();
playback.consume(cues, presentationMilliseconds);

auto packet = scenes.buildRenderPacket(interpolationAlpha);
playback.apply(packet, presentationMilliseconds);
renderer.render(frameDescription, packet, camera);
```

## Tests

Dedicated cross-platform tests cover:

- path traversal rejection, VFS mounts and cooked container validation
- recursive dependency loading, cancellation and dependency cycles
- deterministic budget eviction and transactional hot reload
- cooked sprite upload, texture rebuild and device reset
- fixed pass ordering, adjacent sprite batching and world UI batches
- stable event IDs, exact/wildcard bindings and replay deduplication
- animation frame sampling, additive effects, expiry and NullAudio playback
- all earlier scene extraction, fog, interpolation and platform/render contracts

## Remaining Phase 7 work

The main remaining deliverable is font and UI foundations, followed by a desktop application composition that supplies a native platform backend, a Sokol swapchain and cooked content files. Asset conversion tooling and generalized development hot reload remain Phase 8 concerns.
