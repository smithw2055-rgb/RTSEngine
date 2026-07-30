# Engine G1 and G2 Foundation

This change moves RTSEngine from a programmatic RTS sample toward a data-driven world and presentation runtime while preserving the deterministic simulation boundary.

## Engine G1 — data-driven world and Navigation 2.0

### World map content

`WorldMapDefinition` is a bounded, versioned, canonical map format containing:

- terrain and static navigation cells;
- unit, building and prop spawn records;
- resource nodes;
- required routes and tower-defense lanes;
- trigger zones and an optional script bundle identity.

`WorldMapCodec` encodes and decodes the format in little-endian form. Validation rejects duplicate IDs, invalid dimensions, out-of-bounds records, invalid movement masks and unsafe payload sizes. Canonical sorting and `CanonicalWorldMapHash()` make map content suitable for multiplayer content identity and replay validation.

`WorldMapAssetCodec` integrates the map with the cooked asset system through `AssetType::WorldMap`. `BuildWorldBootstrapPlan()` converts a decoded map into deterministic startup records without giving the asset layer direct access to the authoritative ECS.

### Layered navigation

`NavigationWorld` separates navigation state into:

- terrain type and static blockers;
- dynamic blockers;
- reservations;
- tactical and congestion costs.

`NavProfile` defines movement domain, footprint, clearance, terrain costs and the dynamic cost policy. Ground, air, naval and hover domains are represented explicitly.

`WeightedGridPathfinder` provides deterministic weighted A* with stable tie-breaking, expansion budgets and chunk dependency reporting. `NavigationRequestQueue` provides a request/solve/commit boundary: results are sorted by request ID and are committed only while the revisions they were solved against remain valid.

`FixedPosition2D` and `FixedMover` provide Q16 continuous presentation-independent movement primitives, while `FormationPlanner` assigns stable line, column, box and wedge slots by entity ID.

## Engine G2 — animation, fog, VFX and audio runtime

### Animation

`AnimationController2D` adds action and eight-direction animation selection, transition priority, one-shot interruption policy and stable per-view phase offsets.

`PresentationPlaybackRuntime` now caches decoded animation clips by asset generation. Frame sampling no longer decodes every clip for every sprite on every frame. Event-driven non-looping animations are released and removed when complete, allowing the entity to return to its default animation.

### Fog of war

`FogOfWarSurface` maintains Unexplored, Explored and Visible states, preserves exploration history, supports deterministic circular reveals and exposes a dirty rectangle plus an R8 alpha texture for renderer upload.

### VFX

`VfxRuntime` consumes stable event IDs and creates deterministic, presentation-only burst particles. It provides bounded particle counts, lifetime, drag, gravity, fade and render-view extraction without feeding visual state back into simulation.

### Audio

`AudioMixer` layers bus volume and mute state over `AudioDevice`, supports listener-relative attenuation and pan, priority-aware voice stealing, non-stealable voices, lifecycle retirement and device-generation recovery.

## Validation targets

The focused `rts_engine_g1_g2_tests` target covers world-map round trips and canonical hashes, layered weighted navigation, movement domains, request ordering, formations, fixed movement, animation selection, fog persistence, VFX determinism and audio mixing. The existing `rts_presentation_events_tests` target remains part of the playback regression boundary.

## Deliberate boundaries

This stage establishes production-facing foundations, but it intentionally does not claim the following as complete:

- worker-thread execution for path requests; the deterministic submission and commit boundary is provided, while the host chooses its worker system;
- an operating-system audio backend; the mixer is backend-independent and continues to work with the existing null/test device;
- GPU fog and particle shaders; the runtime produces upload/render data for the renderer backend;
- automatic authoritative instantiation of every preplaced building; the bootstrap plan keeps map loading separate from ECS ownership and lets each game framework choose its creation policy;
- a visual map editor. The canonical format and validation APIs are the foundation for `rtsmap` and editor tooling.
