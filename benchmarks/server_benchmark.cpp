#include "kage/sync/sync.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

namespace {

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

struct Health {
    std::int32_t value = 100;
};

kage::sync::SyncArchetypeId define_archetype(ecs::Registry& registry) {
    const ecs::Entity position = registry.register_component<Position>("Position");
    const ecs::Entity health = registry.register_component<Health>("Health");
    return kage::sync::define_archetype(
        registry,
        "Actor",
        {
            {position, kage::sync::ReplicationAudience::All},
            {health, kage::sync::ReplicationAudience::Owner},
        });
}

std::vector<ecs::Entity> create_entities(ecs::Registry& registry, int count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        entities.push_back(registry.create());
    }
    return entities;
}

void add_clients(kage::sync::ReplicationServer& server, int count) {
    for (int i = 0; i < count; ++i) {
        server.add_client(static_cast<kage::sync::ClientId>(i + 1));
    }
}

void add_replicated_entities(
    kage::sync::ReplicationServer& server,
    ecs::Registry& registry,
    const std::vector<ecs::Entity>& entities,
    kage::sync::SyncArchetypeId archetype) {
    for (const ecs::Entity entity : entities) {
        server.add_replicated(registry, entity, archetype);
    }
}

void BM_ServerTickFullBudget(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int client_count = static_cast<int>(state.range(1));

    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_archetype(registry);
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{
        static_cast<std::size_t>(entity_count) * 128U,
        128U,
    });
    add_clients(server, client_count);
    add_replicated_entities(server, registry, entities, archetype);

    std::uint64_t sent = 0;
    for (auto _ : state) {
        server.tick(registry, [&](kage::sync::ClientId client, ecs::Entity entity) {
            sent += client ^ entity.value;
            benchmark::DoNotOptimize(sent);
        });
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(entity_count) * static_cast<std::int64_t>(client_count));
}

void BM_ServerTickBudgetLimited(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int client_count = static_cast<int>(state.range(1));
    const int sends_per_client = static_cast<int>(state.range(2));

    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_archetype(registry);
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{
        static_cast<std::size_t>(sends_per_client) * 128U,
        128U,
    });
    add_clients(server, client_count);
    add_replicated_entities(server, registry, entities, archetype);

    std::uint64_t sent = 0;
    for (auto _ : state) {
        server.tick(registry, [&](kage::sync::ClientId client, ecs::Entity entity) {
            sent += client + entity.value;
            benchmark::DoNotOptimize(sent);
        });
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(sends_per_client) * static_cast<std::int64_t>(client_count));
}

void BM_ServerAddRemoveReplicated(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));

    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_archetype(registry);
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);
    kage::sync::ReplicationServer server;
    add_clients(server, 4);

    for (auto _ : state) {
        for (const ecs::Entity entity : entities) {
            benchmark::DoNotOptimize(server.add_replicated(registry, entity, archetype));
        }
        for (const ecs::Entity entity : entities) {
            benchmark::DoNotOptimize(server.remove_replicated(registry, entity));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count) * 2);
}

void BM_ServerAddClientsAfterReplicated(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int client_count = static_cast<int>(state.range(1));

    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_archetype(registry);
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);

    for (auto _ : state) {
        kage::sync::ReplicationServer server;
        add_replicated_entities(server, registry, entities, archetype);
        for (int i = 0; i < client_count; ++i) {
            benchmark::DoNotOptimize(server.add_client(static_cast<kage::sync::ClientId>(i + 1)));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count) * client_count);
}

void TickArgs(benchmark::internal::Benchmark* benchmark) {
    benchmark->Args({1024, 1})->Args({16384, 1})->Args({16384, 8})->Args({65536, 8});
}

void LimitedTickArgs(benchmark::internal::Benchmark* benchmark) {
    benchmark->Args({1024, 1, 64})->Args({16384, 1, 64})->Args({16384, 8, 64})->Args({65536, 8, 64});
}

void ChurnArgs(benchmark::internal::Benchmark* benchmark) {
    benchmark->Arg(1024)->Arg(16384)->Arg(65536);
}

void AddClientArgs(benchmark::internal::Benchmark* benchmark) {
    benchmark->Args({1024, 8})->Args({16384, 8})->Args({65536, 8});
}

BENCHMARK(BM_ServerTickFullBudget)->Apply(TickArgs);
BENCHMARK(BM_ServerTickBudgetLimited)->Apply(LimitedTickArgs);
BENCHMARK(BM_ServerAddRemoveReplicated)->Apply(ChurnArgs);
BENCHMARK(BM_ServerAddClientsAfterReplicated)->Apply(AddClientArgs);

}  // namespace
