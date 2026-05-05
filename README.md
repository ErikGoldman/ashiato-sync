# Kage Sync

[![CI](https://github.com/ErikGoldman/kage-sync/actions/workflows/ci.yml/badge.svg)](https://github.com/ErikGoldman/kage-sync/actions/workflows/ci.yml)
[![Coverage](https://img.shields.io/endpoint?url=https://erikgoldman.github.io/kage-sync/coverage.json)](https://erikgoldman.github.io/kage-sync/)

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
- a `ReplicationClient` with snap mode, client-side buffered interpolation, and
  predicted mode with rollback/resimulation
- client ACK packets packed up to a configured MTU
- transport callbacks for serialized server-to-client and client-to-server
  packets

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
and renders replicated moving balls with raylib. Use `--client-mode snap`,
`--client-mode buffered-interpolation`, or `--client-mode predict` to choose the
client application mode.
Buffered interpolation auto-sizes its buffer from measured receive latency and
jitter by default; pass
`--auto-interpolation-buffer off` to keep the configured buffer fixed. Use
`--latency-ms N`, `--jitter-ms N` to start the local link simulator with fixed
latency and uniform `+/-N ms` jitter, and `--loss-percent N` to start with packet
loss. Use `--entities N` to set the initial ball target. In the example,
Up/Down adjust the target by 8, Shift+Up/Down by 1, PageUp/PageDown by 32, and
Home/End jump to the maximum/minimum target.
Use `--time-dilation-min`, `--time-dilation-max`, and
`--time-dilation-gain` to control how quickly the client playback accumulator
converges when the desired buffer changes.

The FPS example uses the same examples option and runs as separate UDP server
and client processes:

```sh
cmake --build build-examples --target kage_sync_fps_example
./build-examples/kage_sync_fps_example --server --port 37043 --bots 4
./build-examples/kage_sync_fps_example --client --host 127.0.0.1 --port 37043
```

For a quick local multi-client run, launch one headless server plus several
client windows from one command:

```sh
./build-examples/kage_sync_fps_example --clients 2 --port 37043 --bots 4
```

`kage_sync_fps_example` renders replicated capsule characters inside a box
arena. The local character is predicted, other characters are buffered and
interpolated, and shooting uses replicated cues for muzzle flashes and hit
feedback. Controls are WASD, mouse look, Space to jump, left click to shoot,
and R to reload.

## Tests

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Trace Viewer

Build the Dear ImGui trace viewer with tracing enabled:

```sh
cmake -S . -B build-viewer \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKAGE_SYNC_BUILD_TRACE_VIEWER=ON \
  -DKAGE_SYNC_ENABLE_TRACING=ON
cmake --build build-viewer --target kage_sync_trace_viewer
```

For live automation, start the viewer with a local control socket:

```sh
./build-viewer/kage_sync_trace_viewer \
  --trace-dir /tmp/kage-balls-trace \
  --control-socket /tmp/kage_sync_trace_viewer.sock
```

The socket accepts newline-delimited commands and returns `OK` or `ERR`:

```sh
printf 'click 120 240\nscreenshot /tmp/kage_sync_trace_viewer.png\nclose\n' | nc -U /tmp/kage_sync_trace_viewer.sock
```

Supported commands are `load path`, `move x y`, `click x y [left|right|middle]`,
`mouse_down x y [button]`, `mouse_up x y [button]`, `scroll x y`,
`screenshot [path]`, `status`, and `close`.
Screenshot paths are forced to a `.png` extension, and missing parent
directories are created before writing.

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

    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.transport =
        [](kage::sync::ClientId client, const ecs::BitBuffer& packet) {
            // Enqueue `packet` for `client` on your UDP/socket transport here.
        };
    kage::sync::ReplicationServer server(server_options);

    server.add_client(1);
    registry.add<kage::sync::Replicated>(entity, {actor});

    server.tick(registry);
}
```

`ReplicationServer::tick` advances every tracked client by one scheduling epoch,
serializes due entity updates into server update packets, and sends those packets
through `ReplicationServerOptions::transport`. Entities sent in the current tick
are moved behind unsent entities for that client, so bandwidth-limited clients
naturally receive older unsent state first.

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
  mode for newly received entities. Set `entity_mode_selector` to choose snap,
  buffered interpolation, or prediction on the first upsert for each entity from
  a decoded `ReplicatedEntityUpdateView`. The view exposes
  `client_entity_network_id`, `local_entity`, `archetype`, `frame`, and typed
  `try_get<T>` accessors for the received component data.
- Components can serialize entity references by storing
  `kage::sync::EntityReference` and defining context-aware
  `SyncComponentTraits<T>::serialize(..., EntityReferenceContext&)` and
  `deserialize(..., EntityReferenceContext&)` overloads. Use
  `write_entity_reference` on the server and `read_entity_reference` on the
  client; references are encoded as the receiving client's compact network id.
  If a referenced entity arrives later, the decoded reference keeps
  `client_entity_network_id` stable and `ReplicationClient::local_entity(id)`
  can resolve it after the entity is created.
- Call `ReplicationClient::set_entity_mode(registry, client_entity_network_id, mode)`
  to switch an already-known replicated entity immediately. It returns `false`
  for unknown replicated entities and does not create future overrides.
- Set `ReplicationClientOptions::fixed_dt_seconds`, call
  `ReplicationClient::tick(registry, dt_seconds)` once per app frame, and pass
  server packets to the normal `receive(registry, packet)` overload. The client
  owns receive/playback frame counters, records continuous receive delay from
  server update frames, applies fixed buffered frames, runs prediction
  rollback/resimulation and ECS jobs for predicted entities, and adjusts
  playback with `timing_stats().time_dilation`. Disable
  `auto_interpolation_buffer_frames` for a fixed manual buffer. Fast auto timing
  recovery is enabled by default; tune it with
  `auto_timing_fast_recovery_min_frame_gap` or disable it with
  `auto_timing_fast_recovery`. Gaps at or above that threshold snap prediction
  and interpolation to the measured targets; smaller changes keep using time
  dilation to nudge toward the target. Adaptive ping sampling is also enabled by
  default, using `adaptive_ping_interval_seconds` until latency stabilizes and
  again after latency jumps. Explicit-frame
  `receive`, `apply_frame`, and `predict_tick` overloads remain available for
  tests and advanced integrations.
- Predicted replicated components must define
  `SyncComponentTraits<T>::should_roll_back(const Quantized&, const Quantized&)`.
  The client throws if a predicted archetype includes a replicated component
  without this hook.
- Mark client-side `ComponentReplication::interpolation` as `Interpolate` for
  components that should be filled between received frames. The corresponding
  `SyncComponentTraits<T>` must provide `static Quantized interpolate(...)`;
  otherwise buffered receive rejects the update without ACKing it. Components
  left as `Step` hold the previous value until the received frame.
- Mark component entities with `set_display_interpolated` when render code
  should sample them at fractional playback frames without mutating the ECS, then
  render `client.display_interpolation_frame(registry).entities`. The display
  frame contains snap and buffered entities in one list. Predicted entities lag
  display-interpolated components one fixed tick behind simulation and sample
  between predicted history frames. Typed `try_get<T>` reads sampled
  display-interpolated values first and falls back to live ECS values for
  untagged components such as visuals. If auto-buffering changes depth or target
  data is missing, the client keeps returning the previous valid display frame
  instead of rewinding or exposing partial live transform state.
- `ReplicationAudience::All` and `ReplicationAudience::Owner` are applied by
  server packet serialization. Full and delta records include only the
  components visible to the receiving client.
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

### Prediction Stress Harness

`kage_sync_prediction_stress` is a deterministic headless prediction scenario
with multiple replicated components, predicted client ECS jobs, configurable
server-to-client frame latency, and configurable authoritative mispredictions.
The default latency is 10 frames.

```sh
cmake --build build-bench --target kage_sync_prediction_stress
./build-bench/kage_sync_prediction_stress \
  --entities 2048 \
  --ticks 1800 \
  --latency-frames 10 \
  --misprediction-percent 5 \
  --rollback-policy all \
  --report json
```

Use `--rollback-policy only-affected` to measure targeted resimulation. The
report includes server/client packet counts and bytes, client receive and tick
timings, server simulation and replication timings, delivered update packets,
and the number of injected misprediction events. The
`run_kage_sync_prediction_stress` target uses
`KAGE_SYNC_PREDICTION_STRESS_RUN_ARGS`.

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
benchmarks/prediction_stress.cpp
                             Prediction rollback/resimulation stress harness
benchmarks/benchmark_helpers.hpp
                             Shared benchmark fixtures and packet helpers
CMakeLists.txt               Library, test, and benchmark targets
```
