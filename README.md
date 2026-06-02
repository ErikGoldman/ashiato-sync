# Ashiato Sync

[![Tests](https://img.shields.io/endpoint?url=https%3A%2F%2Ferikgoldman.github.io%2Fashiato-sync%2Fci.json)](https://github.com/ErikGoldman/ashiato-sync/actions/workflows/ci.yml)
[![Coverage](https://img.shields.io/endpoint?url=https%3A%2F%2Ferikgoldman.github.io%2Fashiato-sync%2Fcoverage.json)](https://github.com/ErikGoldman/ashiato-sync/actions/workflows/ci.yml)
[![Benchmarks](https://img.shields.io/badge/benchmarks-results-blue)](https://erikgoldman.github.io/ashiato-sync/benchmarks/)

Ashiato Sync is a C++17 library for fixed-tick, predictive networking on top of the
Ashiato ECS. The project is separate from the ECS implementation; sync code,
tests, and benchmarks live here.

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
- Git, for CMake to fetch the pinned Ashiato ECS revision

Tests fetch Catch2 with CMake `FetchContent`. Benchmarks fetch Google Benchmark
when enabled.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The library target is `ashiato_sync`, with the alias target `ashiato::sync`.
Pass `-DASHIATO_SYNC_BUILD_STATIC_LIBRARY=ON` to force `ashiato_sync` to build
as a static library even when `BUILD_SHARED_LIBS` is enabled.
CMake fetches Ashiato from GitHub at the pinned `ASHIATO_SYNC_ASHIATO_GIT_TAG`.
For local ECS development, pass
`-DASHIATO_SYNC_ASHIATO_SOURCE_DIR=/path/to/Ashiato` to use a checkout instead.

## Examples

The optional examples target fetches raylib and is disabled by default:

```sh
cmake -S . -B build-examples \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=OFF \
  -DASHIATO_SYNC_BUILD_EXAMPLES=ON
cmake --build build-examples --target ashiato_sync_balls_example
./build-examples/ashiato_sync_balls_example
```

`ashiato_sync_balls_example` runs a UDP localhost server and client in one process
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
`--time-dilation-gain` to control how quickly the client buffered accumulator
converges when the desired buffer changes.

The FPS example uses the same examples option and runs as separate UDP server
and client processes:

```sh
cmake --build build-examples --target ashiato_sync_fps_example
./build-examples/ashiato_sync_fps_example --server --port 37043 --bots 4
./build-examples/ashiato_sync_fps_example --client --host 127.0.0.1 --port 37043
```

For a quick local multi-client run, launch one headless server plus several
client windows from one command:

```sh
./build-examples/ashiato_sync_fps_example --clients 2 --port 37043 --bots 4
```

`ashiato_sync_fps_example` renders replicated capsule characters inside a box
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
  -DASHIATO_SYNC_BUILD_TRACE_VIEWER=ON \
  -DASHIATO_SYNC_ENABLE_TRACING=ON \
  -DASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION=ON
cmake --build build-viewer --target ashiato_sync_trace_viewer
```

For live automation, build with `ASHIATO_SYNC_TRACE_VIEWER_ENABLE_AUTOMATION=ON`
and start the viewer with a local control socket:

```sh
./build-viewer/ashiato_sync_trace_viewer \
  --trace-dir /tmp/ashiato-balls-trace \
  --control-socket /tmp/ashiato_sync_trace_viewer.sock
```

The socket accepts newline-delimited commands and returns `OK` or `ERR`:

```sh
printf 'click 120 240\nscreenshot /tmp/ashiato_sync_trace_viewer.png\nclose\n' | nc -U /tmp/ashiato_sync_trace_viewer.sock
```

Supported commands are `load path`, `move x y`, `click x y [left|right|middle]`,
`mouse_down x y [button]`, `mouse_up x y [button]`, `scroll x y`,
`screenshot [path]`, `status`, and `close`.
Screenshot paths are forced to a `.png` extension, and missing parent
directories are created before writing.

## Basic Usage

```cpp
#include "ashiato/sync/sync.hpp"

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

int main() {
    ashiato::Registry registry;

    const ashiato::Entity position_component =
        registry.register_component<Position>("Position");

    const ashiato::sync::SyncArchetypeId actor =
        ashiato::sync::define_archetype(
            registry,
            "Actor",
            {{position_component, ashiato::sync::ReplicationAudience::All}});

    ashiato::Entity entity = registry.create();

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.bandwidth.enabled = true;
    server_options.bandwidth.min_bytes_per_second = 8 * 1024;
    server_options.bandwidth.initial_bytes_per_second = 32 * 1024;
    server_options.bandwidth.max_bytes_per_second = 512 * 1024;
    server_options.bandwidth.max_burst_bytes = server_options.mtu_bytes * 4;
    server_options.logging.level = ashiato::sync::LogLevel::Warning;
    server_options.logging.format = ashiato::sync::LogFormat::Json;
    server_options.transport =
        [](ashiato::sync::ClientId client, const ashiato::BitBuffer& packet) {
            // Enqueue `packet` for `client` on your UDP/socket transport here.
        };
    ashiato::sync::ReplicationServer server(registry, server_options);

    server.add_client(1);
    registry.add<ashiato::sync::Replicated>(entity, {actor});

    server.tick(registry, server.options().fixed_dt_seconds);
}
```

`ReplicationServer::tick` advances every tracked client by one scheduling epoch,
serializes due entity updates into server update packets, and sends those packets
through `ReplicationServerOptions::transport`. Entities sent in the current tick
are moved behind unsent entities for that client, so bandwidth-limited clients
naturally receive older unsent state first.
Set `ReplicationServerOptions::max_fixed_steps_per_tick` to cap continuous
fixed-step catch-up work; `0` keeps the default unlimited behavior.

## API Notes

- Constructing `ReplicationServer` or `ReplicationClient` registers the sync
  components and configures the registry role. Call
  `ashiato::sync::register_components` only when you need direct component access
  before constructing a replication endpoint.
- Define archetypes with registered ECS component ids. Unregistered component ids
  throw `std::invalid_argument`.
- Add `ashiato::sync::Replicated` directly to an ECS entity to start server
  replication, and remove that component to stop replication. The server
  monitors `Replicated` component additions, replacements, removals, and entity
  destruction at the start of each tick. Call `ReplicationServer::refresh_replicated`
  before querying `is_replicated` or `replicated_count` if marker changes happened
  since the last tick.
- Use `set_owner` or add `NetworkOwner` directly when assigning owner-only
  replicated state.
- `ReplicationServerOptions::logging` configures server logs. Malformed or
  malicious client packets are warnings; server-side callback/configuration
  failures are errors. Set `logging.logger` to use an existing `spdlog` logger,
  including async or application-specific JSON loggers. Built-in JSON logs emit
  stable top-level fields such as `event`, `peer`, `client`, `frame`, and
  `reason`. Client packet warnings are capped per peer by
  `logging.max_warning_logs_per_peer`; `observability_stats()` exposes warning,
  suppression, server error, and connection lifecycle counters.
- `ReplicationClientOptions::default_entity_mode` selects the fallback client
  mode for newly received entities. Set `entity_mode_selector` to choose snap,
  buffered interpolation, or prediction on the first upsert for each entity from
  a decoded `ReplicatedEntityUpdateView`. The view exposes
  `client_entity_network_id`, `local_entity`, `archetype`, `frame`, and typed
  `try_get<T>` accessors for the received component data.
- Components can serialize entity references by storing
  `ashiato::sync::EntityReference` and defining context-aware
  `SyncComponentTraits<T>::serialize(..., EntityReferenceContext&)` and
  `deserialize(..., EntityReferenceContext&)` overloads. Use
  `write_entity_reference` on the server and `read_entity_reference` on the
  client; references are encoded as the receiving client's compact network id.
  If a referenced entity arrives later, the decoded reference keeps
  `client_entity_network_id` stable and `ReplicationClient::local_entity(id)`
  can resolve it after the entity is created.
- A component can expose compile-time serializer profiles by nesting default
  settings, fixed quantized state, and a serializer template inside
  `SyncComponentTraits<T>`. The default `register_sync_component<T>` path uses
  `Serializer<Settings>`, while projects can register another serializer
  instantiation for the same ECS component id and choose it per archetype:

  ```cpp
  struct Transform {
      float x = 0.0f;
      float y = 0.0f;
      float yaw = 0.0f;
  };

  namespace ashiato::sync {
  template <>
  struct SyncComponentTraits<Transform> {
      struct Settings {
          static constexpr bool yaw_only = false;
          static constexpr float position_resolution = 0.01f;
      };

      struct Quantized {
          std::int32_t x = 0;
          std::int32_t y = 0;
          std::int32_t yaw = 0;
      };

      template <typename SettingsT = Settings>
      struct Serializer {
          static Quantized quantize(const Transform& value);
          static Transform dequantize(const Quantized& value);

          static void serialize(
              const Quantized* previous,
              const Quantized& current,
              ashiato::BitBuffer& out,
              ashiato::ComponentSerializationContext& context);

          static bool deserialize(
              ashiato::BitBuffer& in,
              const Quantized* previous,
              Quantized& out,
              ashiato::ComponentSerializationContext& context);
      };
  };
  }  // namespace ashiato::sync

  struct ProjectileTransformSettings {
      static constexpr bool yaw_only = true;
      static constexpr float position_resolution = 0.05f;
  };

  ashiato::Registry registry;
  const ashiato::Entity transform =
      ashiato::sync::register_sync_component<Transform>(registry, "Transform");

  using TransformSync = ashiato::sync::SyncComponentTraits<Transform>;
  using ProjectileTransformSerializer =
      TransformSync::Serializer<ProjectileTransformSettings>;

  const ashiato::sync::SyncComponentSerializerId projectile_serializer =
      ashiato::sync::register_sync_component_serializer<
          Transform,
          ProjectileTransformSerializer>(
              registry,
              "Transform.Projectile");

  const ashiato::sync::SyncArchetypeId player =
      ashiato::sync::define_archetype(
          registry,
          "Player",
          {ashiato::sync::replicate<Transform>(registry)});

  const ashiato::sync::SyncArchetypeId projectile =
      ashiato::sync::define_archetype(
          registry,
          "Projectile",
          {ashiato::sync::replicate<Transform>(registry, projectile_serializer)});
  ```

  Use `if constexpr` inside the nested serializer to compile out branches for a
  settings profile. Profile settings may change precision and wire fields, but
  all profiles for a component share the same `Quantized` and optional `Error`
  layout.
- Call `ReplicationClient::set_entity_mode(registry, client_entity_network_id, mode)`
  to switch an already-known replicated entity immediately. It returns `false`
  for unknown replicated entities and does not create future overrides.
- Set `ReplicationClientOptions::fixed_dt_seconds`, call
  `ReplicationClient::tick(registry, dt_seconds)` once per app frame, and pass
  server packets to the normal `receive(registry, packet)` overload. The client
  owns receive/buffered frame counters, records continuous receive delay from
  server update frames, applies fixed buffered frames, runs prediction
  rollback/resimulation and ECS jobs for predicted entities, and adjusts
  buffered timeline with `timing_stats().time_dilation`. Set
  `ReplicationClientOptions::max_fixed_steps_per_tick` to cap and drop
  excessive receive/buffered/predicted catch-up work; `0` leaves it unlimited.
  Disable
  `auto_interpolation_buffer_frames` for a fixed manual buffer. Fast auto timing
  recovery is enabled by default; tune it with
  `auto_timing_fast_recovery_min_frame_gap` or disable it with
  `auto_timing_fast_recovery`. Gaps at or above that threshold snap prediction
  and interpolation to the measured targets; smaller changes keep using time
  dilation to nudge toward the target. Adaptive ping sampling is also enabled by
  default, using `adaptive_ping_interval_seconds` until latency stabilizes and
  again after latency jumps. Explicit-frame `receive` and `apply_frame`
  overloads remain available for tests and advanced integrations.
- Predicted replicated components must define
  `SyncComponentTraits<T>::should_roll_back(const Quantized&, const Quantized&)`.
  The client throws if a predicted archetype includes a replicated component
  without this hook.
- Mark client-side `ComponentReplication::interpolation` as `Interpolate` for
  components that should be filled between received frames. The corresponding
  `SyncComponentTraits<T>` must provide `static Quantized interpolate(...)`;
  otherwise buffered receive rejects the update without ACKing it. Components
  left as `Step` hold the previous value until the received frame.
- Mark component entities with `set_fractional_tick_sampled` when render code
  should sample them at fractional buffered frames without mutating the ECS, then
  render `client.fractional_tick_frame(registry).entities`. The fractional tick
  frame contains snap and buffered entities in one list. Predicted entities lag
  sampled components one fixed tick behind simulation and sample between
  predicted history frames. Typed `try_get_sampled_value<T>` reads sampled values
  for marked components; read all other entity data directly from ECS. If
  auto-buffering changes depth or target data is missing, the client keeps
  returning the previous valid fractional tick frame instead of rewinding or
  exposing partial live transform state.
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
  -DASHIATO_SYNC_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF
cmake --build build-bench --target ashiato_sync_benchmark
```

Run focused benchmark filters serially when collecting numbers:

```sh
./build-bench/ashiato_sync_benchmark --benchmark_filter=BM_ServerTick
```

Current benchmark coverage includes full-budget server ticks, budget-limited
server ticks, replicated component refresh churn, adding clients after entities
are already configured for replication, and client receive/apply paths for snap
and buffered interpolation.

### Ball Stress Harness

`ashiato_sync_ball_stress` is a deterministic headless stress scenario with
server-side 3D balls, poison-on-bounce, health drain, despawns, simulated
clients, in-memory latency/jitter/loss, timing counters, memory counters, and
bandwidth breakdowns.

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DASHIATO_SYNC_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF
cmake --build build-bench --target ashiato_sync_ball_stress
./build-bench/ashiato_sync_ball_stress \
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

`ashiato_sync_prediction_stress` is a deterministic headless prediction scenario
with multiple replicated components, predicted client ECS jobs, configurable
server-to-client frame latency, and configurable authoritative mispredictions.
The default latency is 10 frames.

```sh
cmake --build build-bench --target ashiato_sync_prediction_stress
./build-bench/ashiato_sync_prediction_stress \
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
`run_ashiato_sync_prediction_stress` target uses
`ASHIATO_SYNC_PREDICTION_STRESS_RUN_ARGS`.

You can also configure the `run_ashiato_sync_ball_stress` target to run the same
scenario normally, through gprof, or through Valgrind memory tools:

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DASHIATO_SYNC_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF \
  -DASHIATO_SYNC_BALL_STRESS_RUN_MODE=none \
  -DASHIATO_SYNC_BALL_STRESS_RUN_ARGS="--duration-seconds 10 --clients 4 --max-balls 4096 --report text"
cmake --build build-bench --target run_ashiato_sync_ball_stress
```

Set `ASHIATO_SYNC_BALL_STRESS_RUN_MODE` to `none`, `gprof`, `memcheck`, or
`massif`. `memcheck` runs `valgrind --leak-check=full --track-origins=yes`, and
`massif` runs `valgrind --tool=massif`.

For gprof, enable instrumentation and select the gprof run mode:

```sh
cmake -S . -B build-bench-gprof \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DASHIATO_SYNC_BUILD_BENCHMARKS=ON \
  -DASHIATO_SYNC_ENABLE_GPROF=ON \
  -DBUILD_TESTING=OFF \
  -DASHIATO_SYNC_BALL_STRESS_RUN_MODE=gprof \
  -DASHIATO_SYNC_BALL_STRESS_RUN_ARGS="--duration-seconds 30 --report text" \
  -DASHIATO_SYNC_BALL_STRESS_GPROF_OUT=/tmp/ashiato_sync_ball_stress_gprof.txt
cmake --build build-bench-gprof --target run_ashiato_sync_ball_stress
```

For memory checking:

```sh
cmake -S . -B build-bench-memcheck \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DASHIATO_SYNC_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF \
  -DASHIATO_SYNC_BALL_STRESS_RUN_MODE=memcheck \
  -DASHIATO_SYNC_BALL_STRESS_RUN_ARGS="--duration-seconds 5 --report text"
cmake --build build-bench-memcheck --target run_ashiato_sync_ball_stress
```

For sanitizer smoke checks:

```sh
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DASHIATO_SYNC_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --target ashiato_sync_ball_stress
./build-asan/ashiato_sync_ball_stress --duration-seconds 5
```

When reporting benchmark results, include the exact command, build type, and
artifact path. Debug timings are useful smoke tests, but use `RelWithDebInfo`
numbers for performance decisions. Use gprof or a comparable profiler to confirm
hot spots before making broad scheduler changes.

## Repository Layout

```text
include/ashiato/sync/sync.hpp   Public API
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
