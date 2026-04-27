#include "kage/sync/sync.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

struct DeltaPosition {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

namespace kage::sync {

template <>
struct SyncComponentTraits<DeltaPosition> {
    using Quantized = DeltaPosition;

    static Quantized quantize(const DeltaPosition& value) {
        return value;
    }

    static DeltaPosition dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized* previous, const Quantized& current, BitBuffer& out) {
        if (previous == nullptr) {
            out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
            return;
        }

        out.push_bits(current.x - previous->x, 8U);
        out.push_bits(current.y - previous->y, 8U);
    }

    static bool deserialize(BitBuffer& in, const Quantized* previous, Quantized& out) {
        if (in.remaining_bits() == sizeof(Quantized) * 8U) {
            in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
            return true;
        }
        if (in.remaining_bits() == 16U && previous != nullptr) {
            out = Quantized{
                previous->x + static_cast<std::int32_t>(in.read_bits(8U)),
                previous->y + static_cast<std::int32_t>(in.read_bits(8U)),
            };
            return true;
        }
        return false;
    }
};

}  // namespace kage::sync

namespace {

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

struct Health {
    std::int32_t value = 100;
};

kage::sync::SyncArchetypeId define_archetype(ecs::Registry& registry) {
    const ecs::Entity position = kage::sync::register_sync_component<Position>(registry, "Position");
    const ecs::Entity health = kage::sync::register_sync_component<Health>(registry, "Health");
    return kage::sync::define_archetype(
        registry,
        "Actor",
        {
            {position, kage::sync::ReplicationAudience::All},
            {health, kage::sync::ReplicationAudience::Owner},
        });
}

kage::sync::SyncArchetypeId define_delta_archetype(ecs::Registry& registry) {
    const ecs::Entity position = kage::sync::register_sync_component<DeltaPosition>(registry, "DeltaPosition");
    return kage::sync::define_archetype(
        registry,
        "DeltaActor",
        {{position, kage::sync::ReplicationAudience::All}});
}

std::vector<ecs::Entity> create_entities(ecs::Registry& registry, int count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        entities.push_back(registry.create());
    }
    return entities;
}

std::vector<ecs::Entity> create_position_entities(ecs::Registry& registry, int count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ecs::Entity entity = registry.create();
        registry.add<Position>(entity, Position{static_cast<float>(i), static_cast<float>(i + 1)});
        entities.push_back(entity);
    }
    return entities;
}

std::vector<ecs::Entity> create_delta_entities(ecs::Registry& registry, int count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ecs::Entity entity = registry.create();
        registry.add<DeltaPosition>(entity, DeltaPosition{i, i + 1});
        entities.push_back(entity);
    }
    return entities;
}

void add_clients(kage::sync::ReplicationServer& server, int count) {
    for (int i = 0; i < count; ++i) {
        server.add_client(static_cast<kage::sync::ClientId>(i + 1));
    }
}

void add_replication_configs(
    ecs::Registry& registry,
    const std::vector<ecs::Entity>& entities,
    kage::sync::SyncArchetypeId archetype) {
    for (const ecs::Entity entity : entities) {
        registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{archetype});
    }
}

void ack_packets(
    kage::sync::ReplicationServer& server,
    const std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>>& packets) {
    for (const auto& sent : packets) {
        kage::sync::BitBuffer packet = sent.second;
        benchmark::DoNotOptimize(packet.read_bits(8U));
        const auto frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
        const auto entity_count = static_cast<std::uint16_t>(packet.read_bits(16U));
        for (std::uint16_t entity_index = 0; entity_index < entity_count; ++entity_index) {
            const ecs::Entity entity{packet.read_unsigned_bits(64U)};
            const bool full = packet.read_bool();
            if (full) {
                benchmark::DoNotOptimize(packet.read_bits(32U));
            }
            const auto component_count = static_cast<std::uint16_t>(packet.read_bits(16U));
            for (std::uint16_t component = 0; component < component_count; ++component) {
                benchmark::DoNotOptimize(packet.read_bits(16U));
                const auto payload_bits = static_cast<std::uint32_t>(packet.read_bits(32U));
                for (std::uint32_t bit = 0; bit < payload_bits; ++bit) {
                    benchmark::DoNotOptimize(packet.read_bool());
                }
            }
            benchmark::DoNotOptimize(server.acknowledge_entity(sent.first, entity, frame));
        }
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
    add_replication_configs(registry, entities, archetype);
    server.refresh_replicated(registry);

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
    add_replication_configs(registry, entities, archetype);
    server.refresh_replicated(registry);

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

void BM_ServerRefreshReplicatedChanges(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));

    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_archetype(registry);
    const std::vector<ecs::Entity> entities = create_entities(registry, entity_count);
    kage::sync::ReplicationServer server;
    add_clients(server, 4);

    for (auto _ : state) {
        for (const ecs::Entity entity : entities) {
            benchmark::DoNotOptimize(registry.add<kage::sync::Replicated>(
                entity,
                kage::sync::Replicated{archetype}));
        }
        server.refresh_replicated(registry);
        for (const ecs::Entity entity : entities) {
            benchmark::DoNotOptimize(registry.remove<kage::sync::Replicated>(entity));
        }
        server.refresh_replicated(registry);
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
        add_replication_configs(registry, entities, archetype);
        server.refresh_replicated(registry);
        for (int i = 0; i < client_count; ++i) {
            benchmark::DoNotOptimize(server.add_client(static_cast<kage::sync::ClientId>(i + 1)));
        }
        for (const ecs::Entity entity : entities) {
            registry.remove<kage::sync::Replicated>(entity);
        }
        server.refresh_replicated(registry);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entity_count) * client_count);
}

void BM_ServerTickSerializedFullBudget(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int client_count = static_cast<int>(state.range(1));

    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_archetype(registry);
    const std::vector<ecs::Entity> entities = create_position_entities(registry, entity_count);

    std::uint64_t sent = 0;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * sizeof(Position);
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        sent += client + payload.byte_size();
        benchmark::DoNotOptimize(sent);
    };

    kage::sync::ReplicationServer server(options);
    add_clients(server, client_count);
    add_replication_configs(registry, entities, archetype);
    server.refresh_replicated(registry);

    for (auto _ : state) {
        server.tick(registry);
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(entity_count) * static_cast<std::int64_t>(client_count));
}

void BM_ServerTickSerializedDelta(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int client_count = static_cast<int>(state.range(1));

    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_delta_archetype(registry);
    const std::vector<ecs::Entity> entities = create_delta_entities(registry, entity_count);

    std::uint64_t sent = 0;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * sizeof(DeltaPosition);
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        sent += client + payload.byte_size();
        benchmark::DoNotOptimize(sent);
    };

    kage::sync::ReplicationServer server(options);
    add_clients(server, client_count);
    add_replication_configs(registry, entities, archetype);
    server.tick(registry);

    for (auto _ : state) {
        server.tick(registry);
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(entity_count) * static_cast<std::int64_t>(client_count));
}

void BM_ServerTickSerializedBudgetLimited(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int client_count = static_cast<int>(state.range(1));
    const int sends_per_client = static_cast<int>(state.range(2));

    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_archetype(registry);
    const std::vector<ecs::Entity> entities = create_position_entities(registry, entity_count);

    std::uint64_t sent = 0;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(sends_per_client) * sizeof(Position);
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        sent += client + payload.byte_size();
        benchmark::DoNotOptimize(sent);
    };

    kage::sync::ReplicationServer server(options);
    add_clients(server, client_count);
    add_replication_configs(registry, entities, archetype);
    server.refresh_replicated(registry);

    for (auto _ : state) {
        server.tick(registry);
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(sends_per_client) * static_cast<std::int64_t>(client_count));
}

void BM_ServerTickPackedFullBudget(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int client_count = static_cast<int>(state.range(1));

    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_archetype(registry);
    const std::vector<ecs::Entity> entities = create_position_entities(registry, entity_count);

    std::uint64_t sent = 0;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * sizeof(Position) * 4U;
    options.mtu_bytes = 1200;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        sent += client + payload.byte_size();
        benchmark::DoNotOptimize(sent);
    };

    kage::sync::ReplicationServer server(options);
    add_clients(server, client_count);
    add_replication_configs(registry, entities, archetype);
    server.refresh_replicated(registry);

    for (auto _ : state) {
        server.tick(registry);
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(entity_count) * static_cast<std::int64_t>(client_count));
}

void BM_ServerTickPackedAckedDeltaShared(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int client_count = static_cast<int>(state.range(1));

    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_delta_archetype(registry);
    const std::vector<ecs::Entity> entities = create_delta_entities(registry, entity_count);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> packets;
    packets.reserve(static_cast<std::size_t>(entity_count) * static_cast<std::size_t>(client_count));

    std::uint64_t sent = 0;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * sizeof(DeltaPosition) * 4U;
    options.mtu_bytes = 1200;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        packets.push_back({client, payload});
        sent += client + payload.byte_size();
        benchmark::DoNotOptimize(sent);
    };

    kage::sync::ReplicationServer server(options);
    add_clients(server, client_count);
    add_replication_configs(registry, entities, archetype);
    server.tick(registry);
    ack_packets(server, packets);
    packets.clear();

    for (auto _ : state) {
        server.tick(registry);
        packets.clear();
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(entity_count) * static_cast<std::int64_t>(client_count));
}

void BM_ServerTickPackedMtuLimited(benchmark::State& state) {
    const int entity_count = static_cast<int>(state.range(0));
    const int client_count = static_cast<int>(state.range(1));

    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_delta_archetype(registry);
    const std::vector<ecs::Entity> entities = create_delta_entities(registry, entity_count);

    std::uint64_t sent = 0;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * sizeof(DeltaPosition) * 4U;
    options.mtu_bytes = 256;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        sent += client + payload.byte_size();
        benchmark::DoNotOptimize(sent);
    };

    kage::sync::ReplicationServer server(options);
    add_clients(server, client_count);
    add_replication_configs(registry, entities, archetype);
    server.refresh_replicated(registry);

    for (auto _ : state) {
        server.tick(registry);
    }

    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(entity_count) * static_cast<std::int64_t>(client_count));
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
BENCHMARK(BM_ServerRefreshReplicatedChanges)->Apply(ChurnArgs);
BENCHMARK(BM_ServerAddClientsAfterReplicated)->Apply(AddClientArgs);
BENCHMARK(BM_ServerTickSerializedFullBudget)->Apply(TickArgs);
BENCHMARK(BM_ServerTickSerializedDelta)->Apply(TickArgs);
BENCHMARK(BM_ServerTickSerializedBudgetLimited)->Apply(LimitedTickArgs);
BENCHMARK(BM_ServerTickPackedFullBudget)->Apply(TickArgs);
BENCHMARK(BM_ServerTickPackedAckedDeltaShared)->Apply(TickArgs);
BENCHMARK(BM_ServerTickPackedMtuLimited)->Apply(TickArgs);

}  // namespace
