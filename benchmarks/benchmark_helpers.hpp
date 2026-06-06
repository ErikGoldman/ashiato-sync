#pragma once

#include "ashiato/sync/sync.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ashiato::sync::benchmarks {

struct DeltaPosition {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

struct Health {
    std::int32_t value = 100;
};

struct LargePayload {
    std::int32_t values[8] = {};
};

struct TinyFlags {
    std::uint8_t bits = 0;
};

struct DestroyPackets {
    std::vector<ashiato::BitBuffer> initial;
    std::vector<ashiato::BitBuffer> destroys;
};

struct ChurnPackets {
    std::vector<ashiato::BitBuffer> initial;
    std::vector<ashiato::BitBuffer> destroys;
    std::vector<ashiato::BitBuffer> respawns;
};

struct TaggedSchema {
    SyncArchetypeId archetype;
    std::vector<ashiato::Entity> tags;
};

}  // namespace ashiato::sync::benchmarks

namespace ashiato::sync {

template <>
struct SyncComponentTraits<benchmarks::DeltaPosition> {
    using Quantized = benchmarks::DeltaPosition;

    static void quantize(const benchmarks::DeltaPosition& value, Quantized& out) {
        out = value;
    }

    static benchmarks::DeltaPosition dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized* previous, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        if (previous == nullptr) {
            out.write_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
            return;
        }

        out.write_bits(current.x - previous->x, 8U);
        out.write_bits(current.y - previous->y, 8U);
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized* previous, Quantized& out, ashiato::ComponentSerializationContext&) {
        if (previous == nullptr) {
            in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
            return true;
        }

        out = Quantized{
            previous->x + static_cast<std::int32_t>(in.read_bits(8U)),
            previous->y + static_cast<std::int32_t>(in.read_bits(8U)),
        };
        return true;
    }

    static Quantized interpolate(const Quantized& from, const Quantized& to, float alpha) {
        return Quantized{
            from.x + static_cast<std::int32_t>(static_cast<float>(to.x - from.x) * alpha),
            from.y + static_cast<std::int32_t>(static_cast<float>(to.y - from.y) * alpha),
        };
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        return predicted.x != authoritative.x || predicted.y != authoritative.y;
    }
};

template <>
struct SyncComponentTraits<benchmarks::LargePayload> {
    using Quantized = benchmarks::LargePayload;

    static void quantize(const benchmarks::LargePayload& value, Quantized& out) {
        out = value;
    }

    static benchmarks::LargePayload dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized* previous, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        if (previous == nullptr) {
            out.write_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
            return;
        }

        for (int index = 0; index < 8; ++index) {
            out.write_bits(current.values[index] - previous->values[index], 8U);
        }
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized* previous, Quantized& out, ashiato::ComponentSerializationContext&) {
        if (previous == nullptr) {
            in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
            return true;
        }

        out = *previous;
        for (int index = 0; index < 8; ++index) {
            out.values[index] += static_cast<std::int32_t>(in.read_bits(8U));
        }
        return true;
    }
};

template <>
struct SyncComponentTraits<benchmarks::TinyFlags> {
    using Quantized = benchmarks::TinyFlags;

    static void quantize(const benchmarks::TinyFlags& value, Quantized& out) {
        out = value;
    }

    static benchmarks::TinyFlags dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized* previous, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        const std::uint8_t base = previous == nullptr ? 0U : previous->bits;
        out.write_bits(static_cast<std::int32_t>(current.bits ^ base), 4U);
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized* previous, Quantized& out, ashiato::ComponentSerializationContext&) {
        const std::uint8_t base = previous == nullptr ? 0U : previous->bits;
        out.bits = static_cast<std::uint8_t>(base ^ static_cast<std::uint8_t>(in.read_bits(4U)));
        return true;
    }
};

}  // namespace ashiato::sync

namespace ashiato::sync::benchmarks {

inline SyncArchetypeId define_archetype(ashiato::Registry& registry) {
    const ashiato::Entity position = register_sync_component<Position>(registry, "Position");
    const ashiato::Entity health = register_sync_component<Health>(registry, "Health");
    return ::ashiato::sync::define_archetype(
        registry,
        "Actor",
        {
            {position, ReplicationAudience::All},
            {health, ReplicationAudience::Owner},
        });
}

inline SyncArchetypeId define_delta_archetype(ashiato::Registry& registry, bool interpolate = false) {
    const ashiato::Entity position = register_sync_component<DeltaPosition>(registry, "DeltaPosition");
    return ::ashiato::sync::define_archetype(
        registry,
        "DeltaActor",
        {{position,
          ReplicationAudience::All,
          interpolate ? ComponentInterpolation::Interpolate : ComponentInterpolation::Step}});
}

inline SyncArchetypeId define_large_payload_archetype(ashiato::Registry& registry) {
    const ashiato::Entity large = register_sync_component<LargePayload>(registry, "LargePayload");
    return ::ashiato::sync::define_archetype(
        registry,
        "LargePayloadActor",
        {{large, ReplicationAudience::All}});
}

inline TaggedSchema define_tagged_archetype(ashiato::Registry& registry, int tag_count) {
    const ashiato::Entity position = register_sync_component<Position>(registry, "Position");
    std::vector<SyncTagReplication> tags;
    tags.reserve(static_cast<std::size_t>(tag_count));
    std::vector<ashiato::Entity> tag_entities;
    tag_entities.reserve(static_cast<std::size_t>(tag_count));
    for (int index = 0; index < tag_count; ++index) {
        const ashiato::Entity tag = registry.register_tag("BenchTag" + std::to_string(index));
        tag_entities.push_back(tag);
        tags.push_back(SyncTagReplication{
            tag,
            (index % 4) == 0 ? ReplicationAudience::Owner : ReplicationAudience::All});
    }
    return TaggedSchema{
        ::ashiato::sync::define_archetype(
            registry,
            SyncArchetypeDesc{
                "TaggedActor",
                std::move(tags),
                {{position, ReplicationAudience::All}},
            }),
        std::move(tag_entities)};
}

inline std::vector<SyncArchetypeId> define_diverse_archetypes(ashiato::Registry& registry) {
    const ashiato::Entity position = register_sync_component<Position>(registry, "Position");
    const ashiato::Entity health = register_sync_component<Health>(registry, "Health");
    const ashiato::Entity delta = register_sync_component<DeltaPosition>(registry, "DeltaPosition");
    const ashiato::Entity large = register_sync_component<LargePayload>(registry, "LargePayload");
    const ashiato::Entity tiny = register_sync_component<TinyFlags>(registry, "TinyFlags");

    return {
        ::ashiato::sync::define_archetype(
            registry,
            "DiversePosition",
            {{position, ReplicationAudience::All}}),
        ::ashiato::sync::define_archetype(
            registry,
            "DiverseDeltaTiny",
            {
                {delta, ReplicationAudience::All},
                {tiny, ReplicationAudience::All},
            }),
        ::ashiato::sync::define_archetype(
            registry,
            "DiverseLargeOwner",
            {
                {large, ReplicationAudience::All},
                {health, ReplicationAudience::Owner},
            }),
    };
}

inline std::vector<ashiato::Entity> create_entities(ashiato::Registry& registry, int count) {
    std::vector<ashiato::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        entities.push_back(registry.create());
    }
    return entities;
}

inline std::vector<ashiato::Entity> create_position_entities(ashiato::Registry& registry, int count) {
    std::vector<ashiato::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ashiato::Entity entity = registry.create();
        registry.add<Position>(entity, Position{static_cast<float>(i), static_cast<float>(i + 1)});
        entities.push_back(entity);
    }
    return entities;
}

inline std::vector<ashiato::Entity> create_tagged_entities(
    ashiato::Registry& registry,
    int count,
    int client_count,
    const std::vector<ashiato::Entity>& tags) {
    std::vector<ashiato::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ashiato::Entity entity = registry.create();
        registry.add<Position>(entity, Position{static_cast<float>(i), static_cast<float>(i + 1)});
        registry.add<NetworkOwner>(
            entity,
            NetworkOwner{static_cast<ClientId>((i % client_count) + 1)});
        for (std::size_t tag_index = 0; tag_index < tags.size(); ++tag_index) {
            if (((static_cast<std::size_t>(i) + tag_index) & 1U) == 0U) {
                registry.add_tag(entity, tags[tag_index]);
            }
        }
        entities.push_back(entity);
    }
    return entities;
}

inline std::vector<ashiato::Entity> create_delta_entities(ashiato::Registry& registry, int count) {
    std::vector<ashiato::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ashiato::Entity entity = registry.create();
        registry.add<DeltaPosition>(entity, DeltaPosition{i, i + 1});
        entities.push_back(entity);
    }
    return entities;
}

inline std::vector<ashiato::Entity> create_large_payload_entities(ashiato::Registry& registry, int count) {
    std::vector<ashiato::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ashiato::Entity entity = registry.create();
        LargePayload payload;
        for (int value = 0; value < 8; ++value) {
            payload.values[value] = i + value;
        }
        registry.add<LargePayload>(entity, payload);
        entities.push_back(entity);
    }
    return entities;
}

inline std::vector<ashiato::Entity> create_owned_entities(ashiato::Registry& registry, int count, int client_count) {
    std::vector<ashiato::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ashiato::Entity entity = registry.create();
        registry.add<Position>(entity, Position{static_cast<float>(i), static_cast<float>(i + 1)});
        registry.add<Health>(entity, Health{100 + i});
        registry.add<NetworkOwner>(
            entity,
            NetworkOwner{static_cast<ClientId>((i % client_count) + 1)});
        entities.push_back(entity);
    }
    return entities;
}

inline std::vector<ashiato::Entity> create_diverse_entities(
    ashiato::Registry& registry,
    int count,
    const std::vector<SyncArchetypeId>& archetypes,
    int client_count) {
    std::vector<ashiato::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ashiato::Entity entity = registry.create();
        const int kind = i % static_cast<int>(archetypes.size());
        if (kind == 0) {
            registry.add<Position>(entity, Position{static_cast<float>(i), static_cast<float>(i + 1)});
        } else if (kind == 1) {
            registry.add<DeltaPosition>(entity, DeltaPosition{i, i + 1});
            registry.add<TinyFlags>(entity, TinyFlags{static_cast<std::uint8_t>(i & 0x0F)});
        } else {
            LargePayload payload;
            for (int value = 0; value < 8; ++value) {
                payload.values[value] = i + value;
            }
            registry.add<LargePayload>(entity, payload);
            registry.add<Health>(entity, Health{100 + i});
            registry.add<NetworkOwner>(
                entity,
                NetworkOwner{static_cast<ClientId>((i % client_count) + 1)});
        }
        entities.push_back(entity);
    }
    return entities;
}

inline void add_clients(ReplicationServer& server, int count) {
    for (int i = 0; i < count; ++i) {
        server.add_client(static_cast<ClientId>(i + 1));
    }
}

inline ReplicationClientOptions make_client_options(ReplicationClientMode mode, SyncFrame buffered_frame_lag = 2) {
    ReplicationClientOptions options;
    options.network.mtu_bytes = 1200;
    options.entities.default_mode = mode;
    options.buffered.buffered_frame_lag = buffered_frame_lag;
    options.buffered.auto_buffered_frame_lag = false;
    options.session.local_client = 1;
    return options;
}

inline void add_replication_configs(
    ashiato::Registry& registry,
    const std::vector<ashiato::Entity>& entities,
    SyncArchetypeId archetype) {
    for (const ashiato::Entity entity : entities) {
        registry.add<Replicated>(entity, Replicated{archetype});
    }
}

inline void add_diverse_replication_configs(
    ashiato::Registry& registry,
    const std::vector<ashiato::Entity>& entities,
    const std::vector<SyncArchetypeId>& archetypes) {
    for (std::size_t i = 0; i < entities.size(); ++i) {
        registry.add<Replicated>(
            entities[i],
            Replicated{archetypes[i % archetypes.size()]});
    }
}

inline void ack_packets(
    ReplicationServer& server,
    ashiato::Registry& registry,
    const std::vector<std::pair<ClientId, ashiato::BitBuffer>>& packets) {
    for (const auto& sent : packets) {
        ashiato::BitBuffer packet = sent.second;
        benchmark::DoNotOptimize(packet.read_bits(ashiato::sync::protocol::message_bits));
        benchmark::DoNotOptimize(packet.read_bits(32U));
        const auto packet_id = static_cast<std::uint32_t>(packet.read_bits(protocol::server_packet_id_bits));
        ashiato::BitBuffer ack;
        ack.write_bits(protocol::client_ack_message, ashiato::sync::protocol::message_bits);
        ack.write_bits(1, protocol::ack_count_bits);
        ack.write_bits(packet_id, protocol::server_packet_id_bits);
        benchmark::DoNotOptimize(server.process_packet(registry, sent.first, ack));
    }
}

inline std::uint64_t consume_packets(const std::vector<std::pair<ClientId, ashiato::BitBuffer>>& packets) {
    std::uint64_t consumed = 0;
    for (const auto& packet : packets) {
        consumed += packet.first + packet.second.byte_size();
    }
    return consumed;
}

inline std::vector<ashiato::BitBuffer> make_client_receive_packets(int entity_count, int frame_count) {
    ashiato::Registry registry;
    const SyncArchetypeId archetype = define_delta_archetype(registry);
    const std::vector<ashiato::Entity> entities = create_delta_entities(registry, entity_count);

    std::vector<std::pair<ClientId, ashiato::BitBuffer>> sent;
    sent.reserve(static_cast<std::size_t>(frame_count) * static_cast<std::size_t>(entity_count));

    ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * sizeof(DeltaPosition) * 8U;
    options.mtu_bytes = 1200;
    options.transport = [&](ClientId client, const ashiato::BitBuffer& packet) {
        sent.push_back({client, packet});
    };

    ReplicationServer server(registry, options);
    server.add_client(1);
    add_replication_configs(registry, entities, archetype);
    for (int frame = 0; frame < frame_count; ++frame) {
        for (int index = 0; index < entity_count; ++index) {
            registry.write<DeltaPosition>(entities[static_cast<std::size_t>(index)]) =
                DeltaPosition{index + frame, index + frame + 1};
        }
        server.tick(registry, server.options().fixed_dt_seconds);
        ack_packets(server, registry, sent);
    }

    std::vector<ashiato::BitBuffer> packets;
    packets.reserve(sent.size());
    for (const auto& packet : sent) {
        packets.push_back(packet.second);
    }
    return packets;
}

inline std::vector<ashiato::BitBuffer> make_large_payload_client_receive_packets(int entity_count, int frame_count) {
    ashiato::Registry registry;
    const SyncArchetypeId archetype = define_large_payload_archetype(registry);
    const std::vector<ashiato::Entity> entities = create_large_payload_entities(registry, entity_count);

    std::vector<std::pair<ClientId, ashiato::BitBuffer>> sent;
    sent.reserve(static_cast<std::size_t>(frame_count) * static_cast<std::size_t>(entity_count));

    ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * sizeof(LargePayload) * 8U;
    options.mtu_bytes = 1200;
    options.transport = [&](ClientId client, const ashiato::BitBuffer& packet) {
        sent.push_back({client, packet});
    };

    ReplicationServer server(registry, options);
    server.add_client(1);
    add_replication_configs(registry, entities, archetype);
    for (int frame = 0; frame < frame_count; ++frame) {
        for (int entity_index = 0; entity_index < entity_count; ++entity_index) {
            LargePayload& payload = registry.write<LargePayload>(entities[static_cast<std::size_t>(entity_index)]);
            for (int value = 0; value < 8; ++value) {
                payload.values[value] = entity_index + frame + value;
            }
        }
        server.tick(registry, server.options().fixed_dt_seconds);
        ack_packets(server, registry, sent);
    }

    std::vector<ashiato::BitBuffer> packets;
    packets.reserve(sent.size());
    for (const auto& packet : sent) {
        packets.push_back(packet.second);
    }
    return packets;
}

inline DestroyPackets make_destroy_packets(int entity_count) {
    ashiato::Registry registry;
    const SyncArchetypeId archetype = define_delta_archetype(registry);
    const std::vector<ashiato::Entity> entities = create_delta_entities(registry, entity_count);

    std::vector<std::pair<ClientId, ashiato::BitBuffer>> sent;
    ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * sizeof(DeltaPosition) * 16U;
    options.mtu_bytes = 1200;
    options.transport = [&](ClientId client, const ashiato::BitBuffer& packet) {
        sent.push_back({client, packet});
    };

    ReplicationServer server(registry, options);
    server.add_client(1);
    add_replication_configs(registry, entities, archetype);
    server.tick(registry, server.options().fixed_dt_seconds);

    DestroyPackets packets;
    packets.initial.reserve(sent.size());
    for (const auto& packet : sent) {
        packets.initial.push_back(packet.second);
    }

    ack_packets(server, registry, sent);
    sent.clear();
    for (const ashiato::Entity entity : entities) {
        registry.destroy(entity);
    }
    server.tick(registry, server.options().fixed_dt_seconds);
    packets.destroys.reserve(sent.size());
    for (const auto& packet : sent) {
        packets.destroys.push_back(packet.second);
    }
    return packets;
}

inline ChurnPackets make_churn_packets(int entity_count) {
    ashiato::Registry registry;
    const SyncArchetypeId archetype = define_delta_archetype(registry);
    std::vector<ashiato::Entity> entities = create_delta_entities(registry, entity_count);

    std::vector<std::pair<ClientId, ashiato::BitBuffer>> sent;
    ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * sizeof(DeltaPosition) * 16U;
    options.mtu_bytes = 1200;
    options.transport = [&](ClientId client, const ashiato::BitBuffer& packet) {
        sent.push_back({client, packet});
    };

    ReplicationServer server(registry, options);
    server.add_client(1);
    add_replication_configs(registry, entities, archetype);
    server.tick(registry, server.options().fixed_dt_seconds);

    ChurnPackets packets;
    packets.initial.reserve(sent.size());
    for (const auto& packet : sent) {
        packets.initial.push_back(packet.second);
    }

    ack_packets(server, registry, sent);
    sent.clear();
    for (const ashiato::Entity entity : entities) {
        registry.destroy(entity);
    }
    server.tick(registry, server.options().fixed_dt_seconds);
    packets.destroys.reserve(sent.size());
    for (const auto& packet : sent) {
        packets.destroys.push_back(packet.second);
    }

    ack_packets(server, registry, sent);
    sent.clear();
    entities = create_delta_entities(registry, entity_count);
    add_replication_configs(registry, entities, archetype);
    server.tick(registry, server.options().fixed_dt_seconds);
    packets.respawns.reserve(sent.size());
    for (const auto& packet : sent) {
        packets.respawns.push_back(packet.second);
    }
    return packets;
}

inline void define_client_delta_schema(ashiato::Registry& registry, bool interpolate) {
    ReplicationClient client(registry, make_client_options(ReplicationClientMode::Snap));
    define_delta_archetype(registry, interpolate);
}

inline void define_client_large_payload_schema(ashiato::Registry& registry) {
    ReplicationClient client(registry, make_client_options(ReplicationClientMode::Snap));
    define_large_payload_archetype(registry);
}

inline void ClientArgs(benchmark::internal::Benchmark* benchmark) {
    benchmark->Args({128, 16})->Args({1024, 16})->Args({4096, 16});
}

inline void DestroyArgs(benchmark::internal::Benchmark* benchmark) {
    benchmark->Arg(128)->Arg(1024)->Arg(4096);
}

}  // namespace ashiato::sync::benchmarks
