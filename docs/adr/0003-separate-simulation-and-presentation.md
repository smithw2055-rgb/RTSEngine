# ADR-0003: Separate simulation and presentation

- Status: Accepted
- Date: 2026-07-21

## Context

Legacy game code commonly lets UI, animation, rendering and audio objects directly change gameplay state. That makes headless execution difficult, introduces frame-rate-dependent behavior and prevents clean replay or future rollback.

## Decision

Authoritative simulation and presentation are separate runtimes.

Simulation publishes:

- immutable world snapshots for continuous visible state
- immutable domain events for discrete facts

Presentation may interpolate snapshots and consume events for animation, particles, audio and UI. It cannot obtain a mutable simulation world or component pointer. UI actions become validated game commands rather than direct method calls on units or managers.

Logical content assets refer to stable asset IDs, never GPU or audio-device handles.

## Consequences

Positive:

- headless simulation and automated tests remain first-class
- render stalls and animation changes cannot alter gameplay
- replay and future rollback can safely rebuild presentation
- render and audio backends remain replaceable

Negative:

- snapshot extraction and view binding require additional code
- event IDs and duplicate-consumption policy must be designed
- some apparently simple UI interactions must pass through command validation

## Enforcement

- simulation CMake targets cannot link render, audio, UI or platform-window targets
- architecture tests scan forbidden include paths
- presentation receives read-only contracts only
- authoritative damage, spawning, economy and wave logic cannot be triggered by animation callbacks
