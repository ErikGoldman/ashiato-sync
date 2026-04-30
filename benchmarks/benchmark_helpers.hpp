#pragma once

#include "kage/sync/sync.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace kage::sync::benchmarks {

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
    std::vector<BitBuffer> initial;
    std::vector<BitBuffer> destroys;
};

struct TaggedSchema {
    SyncArchetypeId archetype;
    std::vector<ecs::Entity> tags;
};

}  // namespace kage::sync::benchmarks

namespace kage::sync {

template <>
struct SyncComponentTraits<benchmarks::DeltaPosition> {
    using Quantized = benchmarks::DeltaPosition;

    static Quantized quantize(const benchmarks::DeltaPosition& value) {
        return value;
    }

    static benchmarks::DeltaPosition dequantize(const Quantized& value) {
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

    static Quantized quantize(const benchmarks::LargePayload& value) {
        return value;
    }

    static benchmarks::LargePayload dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized* previous, const Quantized& current, BitBuffer& out) {
        if (previous == nullptr) {
            out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
            return;
        }

        for (int index = 0; index < 8; ++index) {
            out.push_bits(current.values[index] - previous->values[index], 8U);
        }
    }

    static bool deserialize(BitBuffer& in, const Quantized* previous, Quantized& out) {
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

    static Quantized quantize(const benchmarks::TinyFlags& value) {
        return value;
    }

    static benchmarks::TinyFlags dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized* previous, const Quantized& current, BitBuffer& out) {
        const std::uint8_t base = previous == nullptr ? 0U : previous->bits;
        out.push_bits(static_cast<std::int32_t>(current.bits ^ base), 4U);
    }

    static bool deserialize(BitBuffer& in, const Quantized* previous, Quantized& out) {
        const std::uint8_t base = previous == nullptr ? 0U : previous->bits;
        out.bits = static_cast<std::uint8_t>(base ^ static_cast<std::uint8_t>(in.read_bits(4U)));
        return true;
    }
};

}  // namespace kage::sync

namespace kage::sync::benchmarks {

inline SyncArchetypeId define_archetype(ecs::Registry& registry) {
    const ecs::Entity position = register_sync_component<Position>(registry, "Position");
    const ecs::Entity health = register_sync_component<Health>(registry, "Health");
    return ::kage::sync::define_archetype(
        registry,
        "Actor",
        {
            {position, ReplicationAudience::All},
            {health, ReplicationAudience::Owner},
        });
}

inline SyncArchetypeId define_delta_archetype(ecs::Registry& registry, bool interpolate = false) {
    const ecs::Entity position = register_sync_component<DeltaPosition>(registry, "DeltaPosition");
    return ::kage::sync::define_archetype(
        registry,
        "DeltaActor",
        {{position,
          ReplicationAudience::All,
          interpolate ? ComponentInterpolation::Interpolate : ComponentInterpolation::Step}});
}

inline TaggedSchema define_tagged_archetype(ecs::Registry& registry, int tag_count) {
    const ecs::Entity position = register_sync_component<Position>(registry, "Position");
    std::vector<SyncTagReplication> tags;
    tags.reserve(static_cast<std::size_t>(tag_count));
    std::vector<ecs::Entity> tag_entities;
    tag_entities.reserve(static_cast<std::size_t>(tag_count));
    for (int index = 0; index < tag_count; ++index) {
        const ecs::Entity tag = registry.register_tag("BenchTag" + std::to_string(index));
        tag_entities.push_back(tag);
        tags.push_back(SyncTagReplication{
            tag,
            (index % 4) == 0 ? ReplicationAudience::Owner : ReplicationAudience::All});
    }
    return TaggedSchema{
        ::kage::sync::define_archetype(
            registry,
            SyncArchetypeDesc{
                "TaggedActor",
                std::move(tags),
                {{position, ReplicationAudience::All}},
            }),
        std::move(tag_entities)};
}

inline std::vector<SyncArchetypeId> define_diverse_archetypes(ecs::Registry& registry) {
    const ecs::Entity position = register_sync_component<Position>(registry, "Position");
    const ecs::Entity health = register_sync_component<Health>(registry, "Health");
    const ecs::Entity delta = register_sync_component<DeltaPosition>(registry, "DeltaPosition");
    const ecs::Entity large = register_sync_component<LargePayload>(registry, "LargePayload");
    const ecs::Entity tiny = register_sync_component<TinyFlags>(registry, "TinyFlags");

    return {
        ::kage::sync::define_archetype(
            registry,
            "DiversePosition",
            {{position, ReplicationAudience::All}}),
        ::kage::sync::define_archetype(
            registry,
            "DiverseDeltaTiny",
            {
                {delta, ReplicationAudience::All},
                {tiny, ReplicationAudience::All},
            }),
        ::kage::sync::define_archetype(
            registry,
            "DiverseLargeOwner",
            {
                {large, ReplicationAudience::All},
                {health, ReplicationAudience::Owner},
            }),
    };
}

inline std::vector<ecs::Entity> create_entities(ecs::Registry& registry, int count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        entities.push_back(registry.create());
    }
    return entities;
}

inline std::vector<ecs::Entity> create_position_entities(ecs::Registry& registry, int count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ecs::Entity entity = registry.create();
        registry.add<Position>(entity, Position{static_cast<float>(i), static_cast<float>(i + 1)});
        entities.push_back(entity);
    }
    return entities;
}

inline std::vector<ecs::Entity> create_tagged_entities(
    ecs::Registry& registry,
    int count,
    int client_count,
    const std::vector<ecs::Entity>& tags) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ecs::Entity entity = registry.create();
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

inline std::vector<ecs::Entity> create_delta_entities(ecs::Registry& registry, int count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ecs::Entity entity = registry.create();
        registry.add<DeltaPosition>(entity, DeltaPosition{i, i + 1});
        entities.push_back(entity);
    }
    return entities;
}

inline std::vector<ecs::Entity> create_owned_entities(ecs::Registry& registry, int count, int client_count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ecs::Entity entity = registry.create();
        registry.add<Position>(entity, Position{static_cast<float>(i), static_cast<float>(i + 1)});
        registry.add<Health>(entity, Health{100 + i});
        registry.add<NetworkOwner>(
            entity,
            NetworkOwner{static_cast<ClientId>((i % client_count) + 1)});
        entities.push_back(entity);
    }
    return entities;
}

inline std::vector<ecs::Entity> create_diverse_entities(
    ecs::Registry& registry,
    int count,
    const std::vector<SyncArchetypeId>& archetypes,
    int client_count) {
    std::vector<ecs::Entity> entities;
    entities.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const ecs::Entity entity = registry.create();
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

inline void add_replication_configs(
    ecs::Registry& registry,
    const std::vector<ecs::Entity>& entities,
    SyncArchetypeId archetype) {
    for (const ecs::Entity entity : entities) {
        registry.add<Replicated>(entity, Replicated{archetype});
    }
}

inline void add_diverse_replication_configs(
    ecs::Registry& registry,
    const std::vector<ecs::Entity>& entities,
    const std::vector<SyncArchetypeId>& archetypes) {
    for (std::size_t i = 0; i < entities.size(); ++i) {
        registry.add<Replicated>(
            entities[i],
            Replicated{archetypes[i % archetypes.size()]});
    }
}

inline void ack_packets(
    ReplicationServer& server,
    const std::vector<std::pair<ClientId, BitBuffer>>& packets) {
    for (const auto& sent : packets) {
        BitBuffer packet = sent.second;
        benchmark::DoNotOptimize(packet.read_bits(8U));
        benchmark::DoNotOptimize(packet.read_bits(32U));
        const auto packet_id = static_cast<std::uint32_t>(packet.read_bits(protocol::server_packet_id_bits));
        BitBuffer ack;
        ack.push_bits(protocol::client_ack_message, 8U);
        ack.push_bits(1, 16U);
        ack.push_bits(packet_id, protocol::server_packet_id_bits);
        benchmark::DoNotOptimize(server.process_packet(sent.first, ack));
    }
}

inline std::uint64_t consume_packets(const std::vector<std::pair<ClientId, BitBuffer>>& packets) {
    std::uint64_t consumed = 0;
    for (const auto& packet : packets) {
        consumed += packet.first + packet.second.byte_size();
    }
    return consumed;
}

inline std::vector<BitBuffer> make_client_receive_packets(int entity_count, int frame_count) {
    ecs::Registry registry;
    const SyncArchetypeId archetype = define_delta_archetype(registry);
    const std::vector<ecs::Entity> entities = create_delta_entities(registry, entity_count);

    std::vector<std::pair<ClientId, BitBuffer>> sent;
    sent.reserve(static_cast<std::size_t>(frame_count) * static_cast<std::size_t>(entity_count));

    ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * sizeof(DeltaPosition) * 8U;
    options.mtu_bytes = 1200;
    options.transport = [&](ClientId client, const BitBuffer& packet) {
        sent.push_back({client, packet});
    };

    ReplicationServer server(options);
    server.add_client(1);
    add_replication_configs(registry, entities, archetype);
    for (int frame = 0; frame < frame_count; ++frame) {
        for (int index = 0; index < entity_count; ++index) {
            registry.write<DeltaPosition>(entities[static_cast<std::size_t>(index)]) =
                DeltaPosition{index + frame, index + frame + 1};
        }
        server.tick(registry);
        ack_packets(server, sent);
    }

    std::vector<BitBuffer> packets;
    packets.reserve(sent.size());
    for (const auto& packet : sent) {
        packets.push_back(packet.second);
    }
    return packets;
}

inline DestroyPackets make_destroy_packets(int entity_count) {
    ecs::Registry registry;
    const SyncArchetypeId archetype = define_delta_archetype(registry);
    const std::vector<ecs::Entity> entities = create_delta_entities(registry, entity_count);

    std::vector<std::pair<ClientId, BitBuffer>> sent;
    ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = static_cast<std::size_t>(entity_count) * sizeof(DeltaPosition) * 16U;
    options.mtu_bytes = 1200;
    options.transport = [&](ClientId client, const BitBuffer& packet) {
        sent.push_back({client, packet});
    };

    ReplicationServer server(options);
    server.add_client(1);
    add_replication_configs(registry, entities, archetype);
    server.tick(registry);

    DestroyPackets packets;
    packets.initial.reserve(sent.size());
    for (const auto& packet : sent) {
        packets.initial.push_back(packet.second);
    }

    ack_packets(server, sent);
    sent.clear();
    for (const ecs::Entity entity : entities) {
        registry.destroy(entity);
    }
    server.tick(registry);
    packets.destroys.reserve(sent.size());
    for (const auto& packet : sent) {
        packets.destroys.push_back(packet.second);
    }
    return packets;
}

inline void define_client_delta_schema(ecs::Registry& registry, bool interpolate) {
    configure_client(registry, 1);
    define_delta_archetype(registry, interpolate);
}

inline void ClientArgs(benchmark::internal::Benchmark* benchmark) {
    benchmark->Args({128, 16})->Args({1024, 16})->Args({4096, 16});
}

inline void DestroyArgs(benchmark::internal::Benchmark* benchmark) {
    benchmark->Arg(128)->Arg(1024)->Arg(4096);
}

}  // namespace kage::sync::benchmarks
