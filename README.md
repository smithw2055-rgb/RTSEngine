# RTSEngine

RTSEngine is a C++17 engine specialized for deterministic RTS, tower-defense, base-building and lightweight roguelite games.

## Current slice

The repository now contains the first executable architecture slice:

- C++17 CMake target graph
- generational handles
- canonical field-by-field hashing
- deterministic named random streams
- fixed 30 Hz simulation clock
- centralized `SimulationHost`
- headless example and regression tests
- Linux and Windows CI matrix

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/apps/headless_demo/rts_headless_demo
```

On multi-config generators, pass `--config Debug` to the build and test commands.

## Architecture

See [docs/architecture.md](docs/architecture.md) and [docs/roadmap.md](docs/roadmap.md).
