# ADR-0002: Fixed-tick authoritative simulation

- Status: Accepted
- Date: 2026-07-21

## Context

RTS and tower-defense games require reproducible combat, predictable production timing, stable wave schedules, save/load, replay and automated balance testing. Variable frame-time gameplay makes these goals unreliable and complicates future lockstep networking.

## Decision

All authoritative gameplay runs in a fixed-tick simulation, initially at 30 Hz.

The simulation host exclusively owns tick advancement. Gameplay systems receive `TickContext`, not variable frame delta or wall-clock time. Commands are assigned to ticks and sorted deterministically. Randomness is supplied only through named streams derived from a root seed.

Rendering may run at any frame rate and interpolates immutable snapshots from completed simulation ticks.

## Consequences

Positive:

- deterministic replay and seeded runs
- easier bug reproduction and headless testing
- consistent gameplay across render frame rates
- a viable foundation for future lockstep multiplayer

Negative:

- accumulator and backlog policies must be explicit
- presentation requires interpolation
- long-running work such as pathfinding needs deterministic budgets and commit points
- gameplay-critical floating-point operations must be limited or normalized

## Required invariants

- simulation tick is monotonic and advanced in one place
- interpolation alpha remains within `[0, 1]`
- no authoritative system reads system time
- no authoritative state change occurs outside a tick
- same content, seed and command stream produce the same canonical world hash
