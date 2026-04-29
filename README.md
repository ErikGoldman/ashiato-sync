# Kage Sync

Kage Sync is a C++17 library for fixed-tick, predictive networking on top of the
kagesoko ECS. The project is separate from the ECS implementation in
`../main`; sync code, tests, and benchmarks live here.

The current implementation provides the v0 replication foundation:

- sync component registration for `SyncSettings`, `Replicated`, and
  `NetworkOwner`
- server/client registry configuration helpers
- named sync archetypes with per-component replication audience metadata
- replicated entity and network owner markers
- a bandwidth-limited `ReplicationServer` that rotates sends by per-client
  priority
- serialized server update packets with full, delta, and destroy records
- a `ReplicationClient` with snap mode and client-side buffered interpolation,
  including delayed create/remove/destroy application and ACK packets packed up
  to a configured MTU

Prediction, rollback, and production transport integration are planned surface
area rather than complete behavior in this revision.

## Requirements

- CMake 3.16 or newer
- A C++17 compiler
- The kagesoko ECS checkout at `../main`

Tests fetch Catch2 with CMake `FetchContent`. Benchmarks fetch Google Benchmark
when enabled.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The library target is `kage_sync`, with the alias target `kage::sync`.

## Examples

The optional examples target fetches raylib and is disabled by default:

```sh
cmake -S . -B build-examples \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=OFF \
  -DKAGE_SYNC_BUILD_EXAMPLES=ON
cmake --build build-examples --target kage_sync_balls_example
./build-examples/kage_sync_balls_example
```

`kage_sync_balls_example` runs a UDP localhost server and client in one process
and renders replicated moving balls with raylib. Use `--client-mode snap` or
`--client-mode buffered-interpolation` to choose the client application mode.
Buffered interpolation auto-sizes its buffer from measured receive latency and
jitter by default; pass
`--auto-interpolation-buffer off` to keep the configured buffer fixed. Use
`--jitter-ms N` to start the local link simulator with uniform `+/-N ms` jitter,
and `--entities N` to set the initial ball target. In the example, Up/Down
adjust the target by 8, Shift+Up/Down by 1, PageUp/PageDown by 32, and Home/End
jump to the maximum/minimum target.
Use `--time-dilation-min`, `--time-dilation-max`, and
`--time-dilation-gain` to control how quickly the client playback accumulator
converges when the desired buffer changes.

## Tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Basic Usage

```cpp
#include "kage/sync/sync.hpp"

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

int main() {
    ecs::Registry registry;

    kage::sync::configure_server(registry);

    const ecs::Entity position_component =
        registry.register_component<Position>("Position");

    const kage::sync::SyncArchetypeId actor =
        kage::sync::define_archetype(
            registry,
            "Actor",
            {{position_component, kage::sync::ReplicationAudience::All}});

    ecs::Entity entity = registry.create();

    kage::sync::ReplicationServer server({
        1024, // bandwidth_limit_bytes_per_tick
        128,  // fixed_entity_replication_cost_bytes
    });

    server.add_client(1);
    registry.add<kage::sync::Replicated>(entity, {actor});

    server.tick(registry, [&](kage::sync::ClientId client, ecs::Entity replicated) {
        // Serialize and enqueue `replicated` for `client` here.
    });
}
```

`ReplicationServer::tick` advances every tracked client by one scheduling epoch.
Entities sent in the current tick are moved behind unsent entities for that
client, so bandwidth-limited clients naturally receive older unsent state first.

## API Notes

- Call `kage::sync::register_components`, `configure_server`, or
  `configure_client` before using sync components directly.
- Define archetypes with registered ECS component ids. Unregistered component ids
  throw `std::invalid_argument`.
- Add `kage::sync::Replicated` directly to an ECS entity to start server
  replication, and remove that component to stop replication. The server
  monitors `Replicated` component additions, replacements, removals, and entity
  destruction at the start of each tick. Call `ReplicationServer::refresh_replicated`
  before querying `is_replicated` or `replicated_count` if marker changes happened
  since the last tick.
- Use `set_owner` or add `NetworkOwner` directly when assigning owner-only
  replicated state.
- `ReplicationClientOptions::default_entity_mode` selects the fallback client
  mode for newly received entities. Set `entity_mode_selector` to choose snap
  or buffered interpolation on the first upsert for each entity from a decoded
  `ReplicatedEntityUpdateView`. The view exposes `server_entity`,
  `local_entity`, `archetype`, `frame`, and typed `try_get<T>` accessors for
  the received component data.
- Call `ReplicationClient::set_entity_mode(registry, server_entity, mode)` to
  switch an already-known entity immediately. It returns `false` for unknown
  server entities and does not create future overrides.
- Set `ReplicationClientOptions::fixed_dt_seconds`, call
  `ReplicationClient::tick(registry, dt_seconds)` once per app frame, and pass
  server packets to the normal `receive(registry, packet)` overload. The client
  owns receive/playback frame counters, records continuous receive delay from
  server update frames, applies fixed buffered frames, and adjusts playback with
  `timing_stats().time_dilation`. Disable `auto_interpolation_buffer_frames` for
  a fixed manual buffer. Explicit-frame `receive` and `apply_frame` overloads
  remain available for tests and advanced integrations.
- Mark client-side `ComponentReplication::interpolation` as `Interpolate` for
  components that should be filled between received frames. The corresponding
  `SyncComponentTraits<T>` must provide `static Quantized interpolate(...)`;
  otherwise buffered receive rejects the update without ACKing it. Components
  left as `Step` hold the previous value until the received frame.
- Mark component entities with `set_display_interpolated` when render code
  should sample them at fractional playback frames without mutating the ECS, then
  render `client.display_frame(registry).entities`. The display frame contains
  snap and buffered entities in one list. Typed `try_get<T>` reads sampled
  display-interpolated values first and falls back to live ECS values for
  untagged components such as visuals. If auto-buffering changes depth or target
  data is missing, the client keeps returning the previous valid display frame
  instead of rewinding or exposing partial live transform state.
- `ReplicationAudience::All` and `ReplicationAudience::Owner` are stored as
  archetype metadata. The current scheduler callback receives every scheduled
  entity; audience filtering and serialization policy should be applied by the
  caller for now.
- Destroyed entities and entities externally unmarked as `Replicated` are
  removed from the server's schedule on the next tick.

## Benchmarks

Build benchmark binaries only when needed:

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKAGE_SYNC_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF
cmake --build build-bench --target kage_sync_benchmark
```

Run focused benchmark filters serially when collecting numbers:

```sh
./build-bench/kage_sync_benchmark --benchmark_filter=BM_ServerTick
```

Current benchmark coverage includes full-budget server ticks, budget-limited
server ticks, replicated component refresh churn, adding clients after entities
are already configured for replication, and client receive/apply paths for snap
and buffered interpolation.

### Ball Stress Harness

`kage_sync_ball_stress` is a deterministic headless stress scenario with
server-side 3D balls, poison-on-bounce, health drain, despawns, simulated
clients, in-memory latency/jitter/loss, timing counters, memory counters, and
bandwidth breakdowns.

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKAGE_SYNC_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF
cmake --build build-bench --target kage_sync_ball_stress
./build-bench/kage_sync_ball_stress \
  --duration-seconds 10 \
  --clients 4 \
  --max-balls 4096 \
  --spawn-interval-ms 5 \
  --poison-min 1 \
  --poison-max 8 \
  --health-min 20 \
  --health-max 80 \
  --latency-ms 50 \
  --jitter-ms 25 \
  --loss-percent 1 \
  --client-mode buffered-interpolation \
  --interpolation-buffer-frames 2 \
  --time-dilation-min 0.95 \
  --time-dilation-max 1.05 \
  --time-dilation-gain 0.05 \
  --report json
```

Use `--server-to-client-latency-ms`, `--client-to-server-latency-ms`,
`--server-to-client-jitter-ms`, `--client-to-server-jitter-ms`,
`--server-to-client-loss-percent`, and `--client-to-server-loss-percent` to
override the shared bidirectional link settings. The report includes total bytes
and packets split by direction, packet type, update record kind, dropped
traffic, client timing samples, and selected/auto-sized interpolation buffer
settings.

You can also configure the `run_kage_sync_ball_stress` target to run the same
scenario normally, through gprof, or through Valgrind memory tools:

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKAGE_SYNC_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF \
  -DKAGE_SYNC_BALL_STRESS_RUN_MODE=none \
  -DKAGE_SYNC_BALL_STRESS_RUN_ARGS="--duration-seconds 10 --clients 4 --max-balls 4096 --report text"
cmake --build build-bench --target run_kage_sync_ball_stress
```

Set `KAGE_SYNC_BALL_STRESS_RUN_MODE` to `none`, `gprof`, `memcheck`, or
`massif`. `memcheck` runs `valgrind --leak-check=full --track-origins=yes`, and
`massif` runs `valgrind --tool=massif`.

For gprof, enable instrumentation and select the gprof run mode:

```sh
cmake -S . -B build-bench-gprof \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKAGE_SYNC_BUILD_BENCHMARKS=ON \
  -DKAGE_SYNC_ENABLE_GPROF=ON \
  -DBUILD_TESTING=OFF \
  -DKAGE_SYNC_BALL_STRESS_RUN_MODE=gprof \
  -DKAGE_SYNC_BALL_STRESS_RUN_ARGS="--duration-seconds 30 --report text" \
  -DKAGE_SYNC_BALL_STRESS_GPROF_OUT=/tmp/kage_sync_ball_stress_gprof.txt
cmake --build build-bench-gprof --target run_kage_sync_ball_stress
```

For memory checking:

```sh
cmake -S . -B build-bench-memcheck \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKAGE_SYNC_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF \
  -DKAGE_SYNC_BALL_STRESS_RUN_MODE=memcheck \
  -DKAGE_SYNC_BALL_STRESS_RUN_ARGS="--duration-seconds 5 --report text"
cmake --build build-bench-memcheck --target run_kage_sync_ball_stress
```

For sanitizer smoke checks:

```sh
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DKAGE_SYNC_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --target kage_sync_ball_stress
./build-asan/kage_sync_ball_stress --duration-seconds 5
```

When reporting benchmark results, include the exact command, build type, and
artifact path. Debug timings are useful smoke tests, but use `RelWithDebInfo`
numbers for performance decisions. Use gprof or a comparable profiler to confirm
hot spots before making broad scheduler changes.

## Repository Layout

```text
include/kage/sync/sync.hpp   Public API
src/sync.cpp                 Implementation
tests/sync.test.cpp          Catch2 tests
benchmarks/server_benchmark.cpp
                             Google Benchmark server-side benchmarks
benchmarks/client_benchmark.cpp
                             Google Benchmark client-side benchmarks
benchmarks/benchmark_helpers.hpp
                             Shared benchmark fixtures and packet helpers
CMakeLists.txt               Library, test, and benchmark targets
```
