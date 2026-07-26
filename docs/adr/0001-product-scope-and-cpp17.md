# ADR-0001: Product scope and C++17 baseline

- Status: Accepted
- Date: 2026-07-21

## Context

The project originates from experiments around a StarCraft-like runtime, but its intended product is a reusable lightweight engine for RTS, tower-defense, base-building and roguelite games. Expanding toward a general-purpose engine would dilute development effort and create abstractions that do not improve the target game.

The intended platforms include Windows, Linux, mobile devices and potentially consoles. The codebase must remain compatible with mature platform toolchains and third-party libraries.

## Decision

RTSEngine will:

- target C++17 as the required language level
- remain specialized for RTS, tower defense, base building and roguelite gameplay
- prioritize 2D and 2.5D GPU presentation
- build single-player deterministic simulation before multiplayer
- avoid importing legacy StarCraft code wholesale
- migrate legacy concepts only through newly defined module boundaries

C++20-only language and library features are not part of public contracts. Implementations may not silently require them.

## Consequences

Positive:

- broader compiler and platform compatibility
- simpler third-party integration
- lower risk when targeting mobile and console toolchains
- architectural effort remains focused on gameplay-scale problems

Negative:

- some conveniences such as concepts, ranges and standard `std::span` are unavailable
- small C++17 equivalents or engine-owned views may be required
- the engine cannot market itself as a general-purpose replacement for Unity or Godot

## Non-goals

- general 3D scene authoring
- full rigid-body physics
- arbitrary user-defined runtime component types
- a general node-based scripting editor
- multiplayer before replay determinism is proven
