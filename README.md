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
area rather than complete behavior in this snapshot.

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
`--client-mode buffered-interpolation --interpolation-buffer-frames 2` to choose
the client application mode.

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
- `ReplicationClientOptions::client_mode` selects snap or buffered
  interpolation globally for now. Buffered mode is implemented with per-entity
  bookkeeping so later policy can choose modes per entity without changing the
  storage model.
- Buffered clients call `ReplicationClient::apply_frame(registry, client_frame)`
  each client tick. The ECS reflects server frame
  `client_frame - interpolation_buffer_frames`; create, component add/remove,
  and destroy records are delayed through the same buffer.
- Mark client-side `ComponentReplication::interpolation` as `Interpolate` for
  components that should be filled between received frames. The corresponding
  `SyncComponentTraits<T>` must provide `static Quantized interpolate(...)`;
  otherwise buffered receive rejects the update without ACKing it. Components
  left as `Step` hold the previous value until the received frame.
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
clients, in-memory latency/loss, timing counters, memory counters, and bandwidth
breakdowns.

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
  --loss-percent 1 \
  --client-mode buffered-interpolation \
  --interpolation-buffer-frames 2 \
  --report json
```

Use `--server-to-client-latency-ms`, `--client-to-server-latency-ms`,
`--server-to-client-loss-percent`, and `--client-to-server-loss-percent` to
override the shared bidirectional link settings. The report includes total bytes
and packets split by direction, packet type, update record kind, and dropped
traffic, plus the selected client mode and interpolation buffer settings.

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
                             Google Benchmark server scheduler benchmarks
CMakeLists.txt               Library, test, and benchmark targets
```
