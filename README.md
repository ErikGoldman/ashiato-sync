# Kage Sync

Kage Sync is a C++17 library for fixed-tick, predictive networking on top of the
kagesoko ECS. The project is separate from the ECS implementation in
`../main`; sync code, tests, and benchmarks live here.

The current implementation provides the server-side replication scheduling
foundation:

- sync component registration for `SyncSettings`, `Replicated`, and
  `NetworkOwner`
- server/client registry configuration helpers
- named sync archetypes with per-component replication audience metadata
- replicated entity and network owner markers
- a bandwidth-limited `ReplicationServer` that rotates sends by per-client
  priority

Client-side interpolation, prediction, rollback, serialization, and transport
integration are planned surface area rather than complete behavior in this
snapshot.

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
    server.add_replicated(registry, entity, actor);

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
- Use `mark_replicated` and `set_owner` when working directly with ECS markers.
  Use `ReplicationServer::add_replicated` when the server scheduler should also
  track the entity.
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
server ticks, replicated entity add/remove churn, and adding clients after
entities are already replicated.

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
