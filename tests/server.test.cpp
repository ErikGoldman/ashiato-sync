#include "test_components.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using kage_sync_tests::Health;
using kage_sync_tests::BandwidthProbe;
using kage_sync_tests::NetworkedPayload;
using kage_sync_tests::NetworkedPosition;
using kage_sync_tests::define_position_archetype;
using kage_sync_tests::read_networked_payload;

namespace {

struct ComponentRecord {
    std::uint16_t component_index = 0;
    kage::sync::BitBuffer payload;
};

struct EntityRecord {
    ecs::Entity entity;
    bool destroy = false;
    bool full = false;
    kage::sync::SyncFrame baseline_frame = 0;
    kage::sync::SyncArchetypeId archetype;
    std::vector<ComponentRecord> components;
};

struct ServerUpdatePacket {
    std::uint8_t message = 0;
    kage::sync::SyncFrame frame = 0;
    std::vector<EntityRecord> entities;
};

bool start_sync(ecs::Registry& registry, ecs::Entity entity, kage::sync::SyncArchetypeId archetype) {
    return registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{archetype}) != nullptr;
}

ServerUpdatePacket read_server_update(kage::sync::BitBuffer packet) {
    ServerUpdatePacket update;
    update.message = static_cast<std::uint8_t>(packet.read_bits(8U));
    update.frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
    const auto entity_count = static_cast<std::uint16_t>(packet.read_bits(16U));
    update.entities.reserve(entity_count);
    for (std::uint16_t entity_index = 0; entity_index < entity_count; ++entity_index) {
        EntityRecord entity;
        entity.destroy = packet.read_bool();
        entity.entity = ecs::Entity{packet.read_unsigned_bits(64U)};
        if (entity.destroy) {
            update.entities.push_back(std::move(entity));
            continue;
        }
        entity.full = packet.read_bool();
        if (entity.full) {
            entity.archetype =
                kage::sync::SyncArchetypeId{static_cast<std::uint32_t>(packet.read_bits(32U))};
            const auto component_count = static_cast<std::uint16_t>(packet.read_bits(16U));
            entity.components.reserve(component_count);
            for (std::uint16_t component = 0; component < component_count; ++component) {
                ComponentRecord record;
                record.component_index = static_cast<std::uint16_t>(packet.read_bits(16U));
                std::size_t payload_bits = packet.remaining_bits();
                if (entity_index + 1U < entity_count || component + 1U < component_count) {
                    payload_bits = record.component_index == 0 ? 17U : sizeof(Health) * 8U;
                }
                for (std::size_t bit = 0; bit < payload_bits; ++bit) {
                    record.payload.push_bool(packet.read_bool());
                }
                entity.components.push_back(std::move(record));
            }
        } else {
            REQUIRE(kage::sync::protocol::read_baseline_frame(packet, update.frame, entity.baseline_frame));
            const bool component_changed = packet.read_bool();
            if (component_changed) {
                ComponentRecord record;
                record.component_index = 0;
                const std::size_t payload_bits = packet.remaining_bits();
                for (std::size_t bit = 0; bit < payload_bits; ++bit) {
                    record.payload.push_bool(packet.read_bool());
                }
                entity.components.push_back(std::move(record));
            }
        }
        update.entities.push_back(std::move(entity));
    }
    return update;
}

NetworkedPayload read_first_networked_payload(const kage::sync::BitBuffer& packet) {
    const ServerUpdatePacket update = read_server_update(packet);
    REQUIRE(update.message == 1);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].components.size() == 1);
    return read_networked_payload(update.entities[0].components[0].payload);
}

kage::sync::BitBuffer write_ack_packet(ecs::Entity entity, kage::sync::SyncFrame frame, bool destroy = false) {
    kage::sync::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_ack_message, 8U);
    packet.push_bits(1, 16U);
    packet.push_bool(destroy);
    packet.push_bits(frame, 32U);
    packet.push_unsigned_bits(entity.value, 64U);
    return packet;
}

}  // namespace

TEST_CASE("replication server tracks clients and replicated component changes") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();

    kage::sync::ReplicationServer server;

    REQUIRE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(kage::sync::invalid_client_id));
    REQUIRE(server.has_client(7));
    REQUIRE(server.client_count() == 1);

    REQUIRE_FALSE(start_sync(registry, ecs::Entity{}, archetype));
    REQUIRE(start_sync(registry, entity, kage::sync::SyncArchetypeId{999}));
    server.refresh_replicated(registry);
    REQUIRE_FALSE(server.is_replicated(entity));
    REQUIRE(start_sync(registry, entity, archetype));
    server.refresh_replicated(registry);
    REQUIRE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 1);
    REQUIRE(registry.contains<kage::sync::Replicated>(entity));

    REQUIRE(registry.remove<kage::sync::Replicated>(entity));
    server.refresh_replicated(registry);
    REQUIRE_FALSE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 0);
    REQUIRE_FALSE(registry.contains<kage::sync::Replicated>(entity));
    REQUIRE_FALSE(registry.remove<kage::sync::Replicated>(entity));

    REQUIRE(server.remove_client(7));
    REQUIRE_FALSE(server.has_client(7));
}

TEST_CASE("replication server rejects malformed ACK packets") {
    kage::sync::ReplicationServer server;
    REQUIRE(server.add_client(1));

    kage::sync::BitBuffer empty;
    REQUIRE_FALSE(server.process_packet(1, empty));

    kage::sync::BitBuffer wrong_message;
    wrong_message.push_bits(kage::sync::protocol::server_update_message, 8U);
    wrong_message.push_bits(0, 16U);
    REQUIRE_FALSE(server.process_packet(1, wrong_message));

    kage::sync::BitBuffer truncated_record;
    truncated_record.push_bits(kage::sync::protocol::client_ack_message, 8U);
    truncated_record.push_bits(1, 16U);
    truncated_record.push_bool(false);
    REQUIRE_FALSE(server.process_packet(1, truncated_record));

    REQUIRE_FALSE(server.process_packet(99, write_ack_packet(ecs::Entity{42}, 1)));
    REQUIRE_FALSE(server.process_packet(1, write_ack_packet(ecs::Entity{42}, 1)));
}

TEST_CASE("replication server respects per-client bandwidth limits") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    std::vector<ecs::Entity> entities;
    for (int i = 0; i < 3; ++i) {
        entities.push_back(registry.create());
    }

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{256, 128});
    REQUIRE(server.add_client(1));
    for (const ecs::Entity entity : entities) {
        REQUIRE(start_sync(registry, entity, archetype));
    }

    std::vector<ecs::Entity> sent;
    server.tick(registry, [&](kage::sync::ClientId client, ecs::Entity entity) {
        REQUIRE(client == 1);
        sent.push_back(entity);
    });

    REQUIRE(sent.size() == 2);
    for (const ecs::Entity entity : sent) {
        REQUIRE(server.priority(1, entity) == 0);
    }

    auto unsent = std::find_if(entities.begin(), entities.end(), [&](ecs::Entity entity) {
        return std::find(sent.begin(), sent.end(), entity) == sent.end();
    });
    REQUIRE(unsent != entities.end());
    REQUIRE(server.priority(1, *unsent) == 1);
}

TEST_CASE("replication server rotates budget-limited sends by priority") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    std::vector<ecs::Entity> entities;
    for (int i = 0; i < 3; ++i) {
        entities.push_back(registry.create());
    }

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{128, 128});
    REQUIRE(server.add_client(1));
    for (const ecs::Entity entity : entities) {
        REQUIRE(start_sync(registry, entity, archetype));
    }

    std::vector<ecs::Entity> sent;
    for (int i = 0; i < 3; ++i) {
        server.tick(registry, [&](kage::sync::ClientId, ecs::Entity entity) {
            sent.push_back(entity);
        });
    }

    REQUIRE(sent.size() == 3);
    for (const ecs::Entity entity : entities) {
        REQUIRE(std::find(sent.begin(), sent.end(), entity) != sent.end());
        REQUIRE(server.priority(1, entity) <= 2);
    }
}

TEST_CASE("replication server keeps per-client priorities independent") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{128, 128});
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));
    REQUIRE(server.add_client(2));

    std::vector<std::pair<kage::sync::ClientId, ecs::Entity>> sent;
    server.tick(registry, [&](kage::sync::ClientId client, ecs::Entity entity) {
        sent.push_back({client, entity});
    });

    REQUIRE(sent.size() == 2);
    REQUIRE(sent[0].first == 1);
    REQUIRE(sent[1].first == 2);
    REQUIRE(server.priority(1, sent[0].second) == 0);
    REQUIRE(server.priority(2, sent[1].second) == 0);

    const ecs::Entity client_one_unsent = sent[0].second == first ? second : first;
    const ecs::Entity client_two_unsent = sent[1].second == first ? second : first;
    REQUIRE(server.priority(1, client_one_unsent) == 1);
    REQUIRE(server.priority(2, client_two_unsent) == 1);
}

TEST_CASE("replication server skips destroyed and externally unmarked entities") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity destroyed = registry.create();
    const ecs::Entity unmarked = registry.create();
    const ecs::Entity live = registry.create();

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{512, 128});
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, destroyed, archetype));
    REQUIRE(start_sync(registry, unmarked, archetype));
    REQUIRE(start_sync(registry, live, archetype));

    REQUIRE(registry.destroy(destroyed));
    REQUIRE(registry.remove<kage::sync::Replicated>(unmarked));

    std::vector<ecs::Entity> sent;
    server.tick(registry, [&](kage::sync::ClientId, ecs::Entity entity) {
        sent.push_back(entity);
    });

    REQUIRE(sent == std::vector<ecs::Entity>{live});
    REQUIRE_FALSE(server.is_replicated(destroyed));
    REQUIRE_FALSE(server.is_replicated(unmarked));
    REQUIRE(server.is_replicated(live));
    REQUIRE(server.replicated_count() == 1);
}

TEST_CASE("replication server sends nothing when bandwidth cannot cover the fixed cost") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{127, 128});
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    int sends = 0;
    server.tick(registry, [&](kage::sync::ClientId, ecs::Entity) {
        ++sends;
    });

    REQUIRE(sends == 0);
    REQUIRE(server.priority(1, entity) == 1);
}

TEST_CASE("replication server serializes full and delta component payloads through transport") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        REQUIRE(client == 1);
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket update = read_server_update(payloads[0]);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].full);
    REQUIRE(update.entities[0].archetype == archetype);
    NetworkedPayload fields = read_networked_payload(update.entities[0].components[0].payload);
    REQUIRE_FALSE(fields.delta);
    REQUIRE(fields.x == 10);
    REQUIRE(fields.y == 20);
    REQUIRE(server.acknowledge_entity(1, entity, update.frame));

    registry.write<NetworkedPosition>(entity) = NetworkedPosition{2.0f, 3.0f};
    server.tick(registry);

    REQUIRE(payloads.size() == 2);
    update = read_server_update(payloads[1]);
    REQUIRE(update.entities.size() == 1);
    REQUIRE_FALSE(update.entities[0].full);
    REQUIRE(update.entities[0].baseline_frame == read_server_update(payloads[0]).frame);
    fields = read_networked_payload(update.entities[0].components[0].payload);
    REQUIRE(fields.delta);
    REQUIRE(fields.x == 10);
    REQUIRE(fields.y == 10);
}

TEST_CASE("replication server sends a full update after archetype replacement") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ecs::Entity health_component = kage::sync::register_sync_component<Health>(registry, "Health");
    const kage::sync::SyncArchetypeId position_archetype = kage::sync::define_archetype(
        registry,
        "PositionActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const kage::sync::SyncArchetypeId actor_archetype = kage::sync::define_archetype(
        registry,
        "Actor",
        {
            {position_component, kage::sync::ReplicationAudience::All},
            {health_component, kage::sync::ReplicationAudience::All},
        });
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add<Health>(entity, Health{50}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, position_archetype));

    server.tick(registry);
    ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].full);
    REQUIRE(update.entities[0].archetype == position_archetype);
    REQUIRE(server.acknowledge_entity(1, entity, update.frame));

    REQUIRE(start_sync(registry, entity, actor_archetype));
    server.tick(registry);

    update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].full);
    REQUIRE(update.entities[0].archetype == actor_archetype);
    REQUIRE(update.entities[0].components.size() == 2);
}

TEST_CASE("replication server applies bandwidth limits to actual serialized byte counts") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{2.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 30;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    NetworkedPayload fields = read_first_networked_payload(payloads[0]);
    REQUIRE_FALSE(fields.delta);
    REQUIRE(fields.x == 10);
    REQUIRE(fields.y == 10);
    REQUIRE(server.priority(1, first) == 0);
    REQUIRE(server.priority(1, second) == 1);

    registry.write<NetworkedPosition>(second) = NetworkedPosition{7.0f, 8.0f};
    server.tick(registry);

    REQUIRE(payloads.size() == 2);
    fields = read_first_networked_payload(payloads[1]);
    REQUIRE_FALSE(fields.delta);
    REQUIRE(fields.x == 70);
    REQUIRE(fields.y == 80);
    REQUIRE(server.priority(1, second) == 0);
}

TEST_CASE("replication server filters owner-only serialized components") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ecs::Entity health_component = kage::sync::register_sync_component<Health>(registry, "Health");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "OwnedActor",
        {
            {position_component, kage::sync::ReplicationAudience::All},
            {health_component, kage::sync::ReplicationAudience::Owner},
        });
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add<Health>(entity, Health{42}) != nullptr);
    REQUIRE(kage::sync::set_owner(registry, entity, 2));

    std::vector<std::pair<kage::sync::ClientId, std::size_t>> sends;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        sends.push_back({client, payload.byte_size()});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);

    REQUIRE(sends.size() == 2);
    REQUIRE(sends[0] == std::pair<kage::sync::ClientId, std::size_t>{1, 26});
    REQUIRE(sends[1] == std::pair<kage::sync::ClientId, std::size_t>{2, 32});
}

TEST_CASE("replication server packs entity records up to the configured mtu") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{2.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.mtu_bytes = 52;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    REQUIRE(payloads[0].byte_size() == 44);
    const ServerUpdatePacket update = read_server_update(payloads[0]);
    REQUIRE(update.entities.size() == 2);
}

TEST_CASE("replication server splits packed updates at the mtu boundary") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{2.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.mtu_bytes = 30;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    server.tick(registry);

    REQUIRE(payloads.size() == 2);
    for (const kage::sync::BitBuffer& payload : payloads) {
        REQUIRE(payload.byte_size() <= options.mtu_bytes);
        REQUIRE(payload.byte_size() == 26);
        REQUIRE(read_server_update(payload).entities.size() == 1);
    }
}

TEST_CASE("replication server interleaves pending destroys with bandwidth-limited updates") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity destroyed = registry.create();
    const ecs::Entity live = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(destroyed, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(live, NetworkedPosition{2.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 30;
    options.mtu_bytes = 30;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, destroyed, archetype));
    REQUIRE(start_sync(registry, live, archetype));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    payloads.clear();

    REQUIRE(registry.destroy(destroyed));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE_FALSE(update.entities[0].destroy);
    REQUIRE(update.entities[0].entity == live);
    payloads.clear();

    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].destroy);
    REQUIRE(update.entities[0].entity == destroyed);
}

TEST_CASE("replication server resends pending destroys until ACKed") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 30;
    options.mtu_bytes = 30;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));
    server.tick(registry);
    payloads.clear();

    REQUIRE(registry.destroy(entity));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket first_destroy = read_server_update(payloads.back());
    REQUIRE(first_destroy.entities.size() == 1);
    REQUIRE(first_destroy.entities[0].destroy);
    payloads.clear();

    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket resent_destroy = read_server_update(payloads.back());
    REQUIRE(resent_destroy.entities.size() == 1);
    REQUIRE(resent_destroy.entities[0].destroy);
    REQUIRE(resent_destroy.entities[0].entity == entity);
    REQUIRE(resent_destroy.frame > first_destroy.frame);

    REQUIRE(server.process_packet(1, write_ack_packet(entity, resent_destroy.frame, true)));
    payloads.clear();
    server.tick(registry);
    REQUIRE(payloads.empty());
}

TEST_CASE("replication server accepts delayed entity ACKs for retained snapshots") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);
    const kage::sync::SyncFrame first_frame = read_server_update(payloads.back()).frame;
    registry.write<NetworkedPosition>(entity) = NetworkedPosition{2.0f, 3.0f};
    server.tick(registry);
    REQUIRE(read_first_networked_payload(payloads.back()).delta == false);
    REQUIRE(server.acknowledge_entity(1, entity, first_frame));
    REQUIRE_FALSE(server.acknowledge_entity(1, entity, first_frame));

    const kage::sync::SyncFrame second_frame = read_server_update(payloads.back()).frame;
    REQUIRE(server.acknowledge_entity(1, entity, second_frame));
    REQUIRE_FALSE(server.acknowledge_entity(1, entity, second_frame));

    registry.write<NetworkedPosition>(entity) = NetworkedPosition{3.0f, 4.0f};
    server.tick(registry);
    REQUIRE(read_first_networked_payload(payloads.back()).delta);
}

TEST_CASE("replication server shares ACKed quantized snapshots across clients and frees them") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);
    REQUIRE(payloads.size() == 2);
    REQUIRE(server.retained_snapshot_count() == 1);
    REQUIRE(server.retained_snapshot_bytes() == sizeof(kage_sync_tests::QuantizedNetworkedPosition));

    REQUIRE(server.acknowledge_entity(1, entity, read_server_update(payloads[0].second).frame));
    REQUIRE(server.acknowledge_entity(2, entity, read_server_update(payloads[1].second).frame));
    REQUIRE(server.retained_snapshot_count() == 1);
    REQUIRE(server.retained_snapshot_bytes() == sizeof(kage_sync_tests::QuantizedNetworkedPosition));

    REQUIRE(server.remove_client(1));
    REQUIRE(server.retained_snapshot_count() == 1);
    REQUIRE(server.remove_client(2));
    REQUIRE(server.retained_snapshot_count() == 0);
    REQUIRE(server.retained_snapshot_bytes() == 0);
}

TEST_CASE("replication server keeps swapped clients addressable after removal") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);
    REQUIRE(payloads.size() == 2);

    kage::sync::SyncFrame client_two_frame = 0;
    for (const auto& sent : payloads) {
        if (sent.first == 2) {
            client_two_frame = read_server_update(sent.second).frame;
        }
    }
    REQUIRE(client_two_frame != 0);

    REQUIRE(server.remove_client(1));
    REQUIRE(server.has_client(2));
    REQUIRE(server.process_packet(2, write_ack_packet(entity, client_two_frame)));
}

TEST_CASE("replication server records bandwidth savings for ACKed delta updates") {
    ecs::Registry registry;
    const ecs::Entity probe_component =
        kage::sync::register_sync_component<BandwidthProbe>(registry, "BandwidthProbe");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "BandwidthActor",
        {{probe_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<BandwidthProbe>(entity, BandwidthProbe{100}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);
    REQUIRE(payloads.back().byte_size() == 28);
    REQUIRE(server.acknowledge_entity(1, entity, read_server_update(payloads.back()).frame));

    registry.write<BandwidthProbe>(entity) = BandwidthProbe{105};
    server.tick(registry);

    const std::size_t expected_delta_bits = kage::sync::protocol::server_update_header_bits +
        1U + 64U + 1U + (1U + kage::sync::protocol::baseline_frame_delta_bits) + 1U + 9U;
    REQUIRE(payloads.back().byte_size() == kage::sync::protocol::bytes_for_bits(expected_delta_bits));
}
