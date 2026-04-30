#include "test_components.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using kage_sync_tests::Health;
using kage_sync_tests::BandwidthProbe;
using kage_sync_tests::NetworkedPayload;
using kage_sync_tests::NetworkedPosition;
using kage_sync_tests::Secret;
using kage_sync_tests::Visible;
using kage_sync_tests::define_position_archetype;
using kage_sync_tests::read_networked_payload;

namespace {

struct ComponentRecord {
    std::uint16_t component_index = 0;
    kage::sync::BitBuffer payload;
};

struct EntityRecord {
    ecs::Entity entity;
    std::uint32_t network_id = 0;
    bool destroy = false;
    bool full = false;
    kage::sync::SyncFrame baseline_frame = 0;
    kage::sync::SyncArchetypeId archetype;
    std::vector<ComponentRecord> components;
};

struct ServerUpdatePacket {
    std::uint8_t message = 0;
    kage::sync::SyncFrame frame = 0;
    std::uint32_t packet_id = 0;
    std::vector<EntityRecord> entities;
};

bool start_sync(ecs::Registry& registry, ecs::Entity entity, kage::sync::SyncArchetypeId archetype) {
    return registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{archetype}) != nullptr;
}

kage::sync::BitBuffer make_connect_request(const std::string& token) {
    kage::sync::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_connect_request_message, 8U);
    kage::sync::protocol::write_string(packet, token);
    return packet;
}

ServerUpdatePacket read_server_update(
    kage::sync::BitBuffer packet,
    std::size_t sync_slot_count = 2U,
    std::size_t component_one_payload_bits = 17U,
    std::size_t network_entity_id_tier0_bits =
        kage::sync::protocol::default_network_entity_id_tier0_bits) {
    ServerUpdatePacket update;
    update.message = static_cast<std::uint8_t>(packet.read_bits(8U));
    update.frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
    update.packet_id = static_cast<std::uint32_t>(packet.read_bits(kage::sync::protocol::server_packet_id_bits));
    const auto entity_count = static_cast<std::uint16_t>(packet.read_bits(16U));
    update.entities.reserve(entity_count);
    for (std::uint16_t entity_index = 0; entity_index < entity_count; ++entity_index) {
        EntityRecord entity;
        entity.destroy = packet.read_bool();
        REQUIRE(kage::sync::protocol::read_network_entity_id(
            packet,
            entity.network_id,
            network_entity_id_tier0_bits));
        if (entity.destroy) {
            update.entities.push_back(std::move(entity));
            continue;
        }
        entity.full = packet.read_bool();
        if (entity.full) {
            entity.archetype =
                kage::sync::SyncArchetypeId{static_cast<std::uint32_t>(packet.read_bits(32U))};
            const bool uses_presence_mask = packet.read_bool();
            const std::size_t sync_slot_bits = kage::sync::protocol::bits_for_range(sync_slot_count);
            std::uint16_t component_count = 0;
            std::uint64_t presence_mask = 0;
            if (uses_presence_mask) {
                presence_mask = packet.read_unsigned_bits(sync_slot_count);
                for (std::size_t slot = 0; slot < sync_slot_count; ++slot) {
                    if ((presence_mask & (std::uint64_t{1} << slot)) != 0U) {
                        ++component_count;
                    }
                }
            } else {
                component_count = static_cast<std::uint16_t>(packet.read_bits(16U));
            }
            entity.components.reserve(component_count);
            for (std::uint16_t component = 0; component < component_count; ++component) {
                ComponentRecord record;
                if (uses_presence_mask) {
                    while ((presence_mask & (std::uint64_t{1} << record.component_index)) == 0U) {
                        ++record.component_index;
                    }
                    presence_mask &= ~(std::uint64_t{1} << record.component_index);
                } else {
                    record.component_index = static_cast<std::uint16_t>(packet.read_bits(sync_slot_bits));
                }
                const std::size_t payload_bits =
                    record.component_index == 1 ? component_one_payload_bits : sizeof(Health) * 8U;
                for (std::size_t bit = 0; bit < payload_bits; ++bit) {
                    record.payload.push_bool(packet.read_bool());
                }
                entity.components.push_back(std::move(record));
            }
            const bool has_cues = packet.read_bool();
            REQUIRE_FALSE(has_cues);
        } else {
            REQUIRE(kage::sync::protocol::read_baseline_frame(packet, update.frame, entity.baseline_frame));
            const bool tag_changed = packet.read_bool();
            REQUIRE_FALSE(tag_changed);
            std::vector<std::uint16_t> changed_components;
            for (std::uint16_t component_index = 1; component_index < sync_slot_count; ++component_index) {
                if (packet.read_bool()) {
                    changed_components.push_back(component_index);
                }
            }
            for (std::size_t changed_index = 0; changed_index < changed_components.size(); ++changed_index) {
                ComponentRecord record;
                record.component_index = changed_components[changed_index];
                const std::size_t payload_bits =
                    record.component_index == 1 ? component_one_payload_bits : sizeof(Health) * 8U;
                for (std::size_t bit = 0; bit < payload_bits; ++bit) {
                    record.payload.push_bool(packet.read_bool());
                }
                entity.components.push_back(std::move(record));
            }
            const bool has_cues = packet.read_bool();
            REQUIRE_FALSE(has_cues);
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

kage::sync::BitBuffer write_ack_packet(std::uint32_t packet_id) {
    kage::sync::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_ack_message, 8U);
    packet.push_bits(1, 16U);
    packet.push_bits(packet_id, kage::sync::protocol::server_packet_id_bits);
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

TEST_CASE("replication server disconnects clients after configured idle timeout") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 2.0;
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    server.tick(registry);
    REQUIRE(server.has_client(1));
    server.tick(registry);
    REQUIRE_FALSE(server.has_client(1));
}

TEST_CASE("replication server resets idle timeout when a client sends packets") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 2.0;
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    server.tick(registry);
    REQUIRE(server.has_client(1));

    kage::sync::BitBuffer ack;
    ack.push_bits(kage::sync::protocol::client_ack_message, 8U);
    ack.push_bits(0, 16U);
    REQUIRE(server.process_packet(1, ack));

    server.tick(registry);
    REQUIRE(server.has_client(1));
    server.tick(registry);
    REQUIRE_FALSE(server.has_client(1));
}

TEST_CASE("server connect response resends until client id is ACKed") {
    ecs::Registry registry;
    define_position_archetype(registry);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> sent;
    kage::sync::ReplicationServerOptions options;
    options.connect_handler = [](const std::string& token, kage::sync::ClientId& client, std::string& error) {
        if (token != "token") {
            error = "bad token";
            return false;
        }
        client = 7;
        return true;
    };
    options.transport = [&](kage::sync::ClientId peer, const kage::sync::BitBuffer& packet) {
        sent.push_back({peer, packet});
    };
    kage::sync::ReplicationServer server(options);

    REQUIRE(server.process_packet(99, make_connect_request("token")));
    REQUIRE(server.has_client(7));
    REQUIRE(sent.size() == 1);
    REQUIRE(sent.back().first == 99);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);
    REQUIRE(sent.back().second.read_bool());
    REQUIRE(sent.back().second.read_unsigned_bits(64U) == 7);

    sent.clear();
    for (int tick = 0; tick < 16; ++tick) {
        server.tick(registry);
    }
    REQUIRE(sent.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);

    kage::sync::BitBuffer ack;
    ack.push_bits(kage::sync::protocol::client_connect_ack_message, 8U);
    ack.push_unsigned_bits(7, 64U);
    REQUIRE(server.process_packet(99, ack));

    sent.clear();
    server.tick(registry);
    REQUIRE(sent.empty());

    REQUIRE(server.process_packet(100, make_connect_request("no")));
    REQUIRE(sent.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);
    REQUIRE_FALSE(sent.back().second.read_bool());
    std::string error;
    REQUIRE(kage::sync::protocol::read_string(sent.back().second, error));
    REQUIRE(error == "bad token");
}

TEST_CASE("replication client and server templates configure network id tier width") {
    kage::sync::ReplicationClientT<8> client;
    REQUIRE(client.options().network_entity_id_tier0_bits == 8U);

    kage::sync::ReplicationServerT<8> server;
    REQUIRE(server.options().network_entity_id_tier0_bits == 8U);
}

TEST_CASE("replication client and server reject invalid network id tier widths") {
    kage::sync::ReplicationClientOptions client_options;
    client_options.network_entity_id_tier0_bits = 0U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClient(client_options), std::invalid_argument);
    client_options.network_entity_id_tier0_bits = 23U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClient(client_options), std::invalid_argument);

    kage::sync::ReplicationServerOptions server_options;
    server_options.network_entity_id_tier0_bits = 0U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationServer(server_options), std::invalid_argument);
    server_options.network_entity_id_tier0_bits = 23U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationServer(server_options), std::invalid_argument);
}

TEST_CASE("replication client and server interoperate with custom network id tier width") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    const ecs::Entity entity = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(
                entity,
                kage_sync_tests::Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, entity, server_archetype));

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        REQUIRE(client == 1);
        payloads.push_back(payload);
    };
    kage::sync::ReplicationServerT<8> server(server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry);
    REQUIRE(payloads.size() == 1);

    const ServerUpdatePacket update = read_server_update(
        payloads[0],
        2U,
        sizeof(kage_sync_tests::Position) * 8U,
        kage::sync::ReplicationServerT<8>::network_entity_id_tier0_bits);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].network_id == 1U);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == server_archetype.value);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClientT<8> client;
    REQUIRE(client.receive(client_registry, payloads[0]));

    const kage::sync::ClientEntityNetworkId client_network_id =
        kage::sync::make_client_entity_network_id(1, update.entities[0].network_id, 1U);
    const ecs::Entity local = client.local_entity(client_network_id);
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client_registry.get<kage_sync_tests::Position>(local).x == 1.0f);
    REQUIRE(client_registry.get<kage_sync_tests::Position>(local).y == 2.0f);
    REQUIRE(client.drain_ack_packets().size() == 1);
}

TEST_CASE("replication rejects client ids that cannot fit in client entity network ids") {
    kage::sync::ReplicationServer server;
    REQUIRE_FALSE(server.add_client(kage::sync::max_client_entity_network_id_client + 1U));

    ecs::Registry registry;
    REQUIRE_THROWS_AS(
        kage::sync::configure_client(registry, kage::sync::max_client_entity_network_id_client + 1U),
        std::invalid_argument);

    std::vector<kage::sync::BitBuffer> responses;
    kage::sync::ReplicationServerOptions options;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        responses.push_back(payload);
    };
    options.connect_handler = [](const std::string&, kage::sync::ClientId& accepted, std::string&) {
        accepted = kage::sync::max_client_entity_network_id_client + 1U;
        return true;
    };
    kage::sync::ReplicationServer connect_server(options);
    REQUIRE(connect_server.process_packet(99, make_connect_request("token")));
    REQUIRE_FALSE(connect_server.has_client(kage::sync::max_client_entity_network_id_client + 1U));
    REQUIRE(responses.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(responses[0].read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);
    REQUIRE_FALSE(responses[0].read_bool());
    std::string error;
    REQUIRE(kage::sync::protocol::read_string(responses[0], error));
    REQUIRE(error == "client id out of range");
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

    REQUIRE_FALSE(server.process_packet(99, write_ack_packet(42)));
    REQUIRE_FALSE(server.process_packet(1, write_ack_packet(42)));
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

    update = read_server_update(payloads.back(), 3U);
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
    options.bandwidth_limit_bytes_per_tick = 17;
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
    REQUIRE(sends[0] == std::pair<kage::sync::ClientId, std::size_t>{1, 17});
    REQUIRE(sends[1] == std::pair<kage::sync::ClientId, std::size_t>{2, 21});
}

TEST_CASE("replication server serializes compact synced tag masks per client") {
    ecs::Registry registry;
    const ecs::Entity visible = registry.register_component<Visible>("Visible");
    const ecs::Entity secret = registry.register_component<Secret>("Secret");
    const ecs::Entity position =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        kage::sync::SyncArchetypeDesc{
            "TaggedActor",
            {{visible, kage::sync::ReplicationAudience::All},
             {secret, kage::sync::ReplicationAudience::Owner}},
            {{position, kage::sync::ReplicationAudience::All}},
        });
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add_tag(entity, visible));
    REQUIRE(registry.add_tag(entity, secret));
    REQUIRE(kage::sync::set_owner(registry, entity, 1));

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
    for (auto sent : payloads) {
        kage::sync::BitBuffer packet = sent.second;
        REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::server_update_message);
        const auto frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
        packet.read_bits(kage::sync::protocol::server_packet_id_bits);
        REQUIRE(static_cast<std::uint16_t>(packet.read_bits(16U)) == 1);
        REQUIRE_FALSE(packet.read_bool());
        std::uint32_t network_id = 0;
        REQUIRE(kage::sync::protocol::read_network_entity_id(packet, network_id));
        REQUIRE(network_id != 0);
        REQUIRE(packet.read_bool());
        REQUIRE(kage::sync::SyncArchetypeId{static_cast<std::uint32_t>(packet.read_bits(32U))} == archetype);
        REQUIRE(packet.read_bool());
        REQUIRE(packet.read_unsigned_bits(2U) == 3U);
        REQUIRE(packet.read_unsigned_bits(2U) == (sent.first == 1 ? 3U : 1U));
        NetworkedPayload fields = read_networked_payload(packet);
        REQUIRE_FALSE(fields.delta);
        REQUIRE(fields.x == 10);
        REQUIRE(fields.y == 20);
        REQUIRE_FALSE(packet.read_bool());
        REQUIRE(server.acknowledge_entity(sent.first, entity, frame));
    }

    REQUIRE(registry.remove_tag(entity, visible));
    payloads.clear();
    server.tick(registry);
    REQUIRE(payloads.size() == 2);
    for (auto sent : payloads) {
        kage::sync::BitBuffer packet = sent.second;
        packet.read_bits(8U);
        const auto frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
        packet.read_bits(kage::sync::protocol::server_packet_id_bits);
        kage::sync::SyncFrame baseline_frame = 0;
        REQUIRE(static_cast<std::uint16_t>(packet.read_bits(16U)) == 1);
        REQUIRE_FALSE(packet.read_bool());
        std::uint32_t network_id = 0;
        REQUIRE(kage::sync::protocol::read_network_entity_id(packet, network_id));
        REQUIRE(network_id != 0);
        REQUIRE_FALSE(packet.read_bool());
        REQUIRE(kage::sync::protocol::read_baseline_frame(packet, frame, baseline_frame));
        REQUIRE(packet.read_bool());
        REQUIRE_FALSE(packet.read_bool());
        REQUIRE(packet.read_unsigned_bits(2U) == (sent.first == 1 ? 2U : 0U));
        REQUIRE_FALSE(packet.read_bool());
    }
}

TEST_CASE("replication server batches sphere filtering and component LOD masks") {
    ecs::Registry registry;
    const ecs::Entity health_component = kage::sync::register_sync_component<Health>(registry, "Health");
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "SphereFilteredActor",
        {{health_component, kage::sync::ReplicationAudience::All},
         {position_component, kage::sync::ReplicationAudience::All}});

    const ecs::Entity near = registry.create();
    REQUIRE(registry.add<Health>(near, Health{75}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(near, NetworkedPosition{1.0f, 0.0f}) != nullptr);
    REQUIRE(start_sync(registry, near, archetype));

    const ecs::Entity far = registry.create();
    REQUIRE(registry.add<Health>(far, Health{25}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(far, NetworkedPosition{10.0f, 0.0f}) != nullptr);
    REQUIRE(start_sync(registry, far, archetype));

    std::vector<kage::sync::BitBuffer> payloads;
    std::size_t prioritizer_calls = 0;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId client,
                              const std::vector<kage::sync::ReplicationPriorityObject>& objects,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        REQUIRE(client == 1);
        ++prioritizer_calls;
        REQUIRE(decisions.size() == objects.size());
        for (std::size_t index = 0; index < objects.size(); ++index) {
            const NetworkedPosition& position = registry.get<NetworkedPosition>(objects[index].entity);
            decisions[index].replicate = position.x <= 2.0f;
            decisions[index].priority = decisions[index].replicate ? 100U : 0U;
            decisions[index].component_mask = std::uint64_t{1} << 0U;
        }
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    server.tick(registry);

    REQUIRE(prioritizer_calls == 1);
    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket update = read_server_update(payloads[0], 2U);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].network_id != 0);
    REQUIRE(update.entities[0].components.size() == 1);
    REQUIRE(update.entities[0].components[0].component_index == 1);
}

TEST_CASE("replication prioritizer reuses cached decisions until the configured interval expires") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{1.0f, 1.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    bool allow_replication = false;
    std::size_t prioritizer_calls = 0;
    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.prioritizer_interval_frames = 3;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>& objects,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        ++prioritizer_calls;
        REQUIRE(objects.size() == 1);
        decisions[0].replicate = allow_replication;
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    server.tick(registry);
    REQUIRE(prioritizer_calls == 1);
    REQUIRE(payloads.empty());

    allow_replication = true;
    server.tick(registry);
    server.tick(registry);
    REQUIRE(prioritizer_calls == 1);
    REQUIRE(payloads.empty());

    server.tick(registry);
    REQUIRE(prioritizer_calls == 2);
    REQUIRE(payloads.size() == 1);
    REQUIRE(read_server_update(payloads.back(), 2U, sizeof(kage_sync_tests::Position) * 8U).entities.size() == 1);
}

TEST_CASE("replication prioritizer filtering destroys an already visible entity for that client") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "FilterDestroyActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    bool allow_replication = true;
    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>&,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        decisions[0].replicate = allow_replication;
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE_FALSE(update.entities[0].destroy);
    const std::uint32_t visible_network_id = update.entities[0].network_id;
    REQUIRE(server.acknowledge_entity(1, entity, update.frame));

    allow_replication = false;
    payloads.clear();
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].destroy);
    REQUIRE(update.entities[0].network_id == visible_network_id);

    REQUIRE(server.process_packet(1, write_ack_packet(update.packet_id)));
    payloads.clear();
    server.tick(registry);
    REQUIRE(payloads.empty());

    allow_replication = true;
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE_FALSE(update.entities[0].destroy);
    REQUIRE(update.entities[0].full);
}

TEST_CASE("replication prioritizer priority affects serialized send order") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity low = registry.create();
    const ecs::Entity middle = registry.create();
    const ecs::Entity high = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(low, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(middle, NetworkedPosition{2.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(high, NetworkedPosition{3.0f, 3.0f}) != nullptr);
    REQUIRE(start_sync(registry, low, archetype));
    REQUIRE(start_sync(registry, middle, archetype));
    REQUIRE(start_sync(registry, high, archetype));

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.mtu_bytes = 17;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>& objects,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        for (std::size_t index = 0; index < objects.size(); ++index) {
            if (objects[index].entity == high) {
                decisions[index].priority = 100U;
            } else if (objects[index].entity == middle) {
                decisions[index].priority = 50U;
            }
        }
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    server.tick(registry);

    REQUIRE(payloads.size() == 3);
    const NetworkedPayload first = read_first_networked_payload(payloads[0]);
    REQUIRE_FALSE(first.delta);
    REQUIRE(first.x == 30);
    REQUIRE(first.y == 30);
}

TEST_CASE("replication prioritizer component masks apply to delta updates") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ecs::Entity health_component = kage::sync::register_sync_component<Health>(registry, "Health");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "MaskedActor",
        {
            {position_component, kage::sync::ReplicationAudience::All},
            {health_component, kage::sync::ReplicationAudience::All},
        });
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add<Health>(entity, Health{25}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::uint64_t component_mask = std::numeric_limits<std::uint64_t>::max();
    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>&,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        for (kage::sync::ReplicationPriorityDecision& decision : decisions) {
            decision.component_mask = component_mask;
        }
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket update = read_server_update(payloads.back(), 3U);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].full);
    REQUIRE(update.entities[0].components.size() == 2);
    REQUIRE(server.acknowledge_entity(1, entity, update.frame));

    registry.write<NetworkedPosition>(entity) = NetworkedPosition{4.0f, 6.0f};
    registry.write<Health>(entity) = Health{75};
    component_mask = std::uint64_t{1} << 0U;
    payloads.clear();
    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    update = read_server_update(payloads.back(), 3U);
    REQUIRE(update.entities.size() == 1);
    REQUIRE_FALSE(update.entities[0].full);
    REQUIRE(update.entities[0].components.size() == 1);
    REQUIRE(update.entities[0].components[0].component_index == 1);
    const NetworkedPayload fields = read_networked_payload(update.entities[0].components[0].payload);
    REQUIRE(fields.delta);
    REQUIRE(fields.x == 30);
    REQUIRE(fields.y == 40);
}

TEST_CASE("replication prioritizer can emit an entity record with an all-zero component mask") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "ZeroMaskActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>&,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        decisions[0].component_mask = 0;
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].full);
    REQUIRE(update.entities[0].components.empty());
}

TEST_CASE("replication prioritizer rejects callbacks that return the wrong decision count") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{1.0f, 1.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    kage::sync::ReplicationServerOptions options;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [](kage::sync::ClientId,
                             const std::vector<kage::sync::ReplicationPriorityObject>&,
                             std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        decisions.clear();
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE_THROWS_AS(server.tick(registry, kage::sync::ReplicationServer::ReplicateFn{}), std::invalid_argument);
}

TEST_CASE("replication prioritizer applies independent decisions per client") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PerClientActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{2.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId client,
                              const std::vector<kage::sync::ReplicationPriorityObject>& objects,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        for (std::size_t index = 0; index < objects.size(); ++index) {
            decisions[index].replicate = client == 1 ? objects[index].entity == first : objects[index].entity == second;
        }
    };
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    server.tick(registry);

    REQUIRE(payloads.size() == 2);
    for (const auto& sent : payloads) {
        const NetworkedPayload fields = read_first_networked_payload(sent.second);
        REQUIRE_FALSE(fields.delta);
        if (sent.first == 1) {
            REQUIRE(fields.x == 10);
        } else {
            REQUIRE(sent.first == 2);
            REQUIRE(fields.x == 20);
        }
    }
}

TEST_CASE("replication prioritizer decisions are honored by the parallel serialized path") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "ParallelPrioritizedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{2.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.serialized_worker_threads = 2;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId client,
                              const std::vector<kage::sync::ReplicationPriorityObject>& objects,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        for (std::size_t index = 0; index < objects.size(); ++index) {
            decisions[index].replicate = client == 1 ? objects[index].entity == first : objects[index].entity == second;
        }
    };
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    server.tick(registry);

    REQUIRE(payloads.size() == 2);
    for (const auto& sent : payloads) {
        const NetworkedPayload fields = read_first_networked_payload(sent.second);
        REQUIRE_FALSE(fields.delta);
        if (sent.first == 1) {
            REQUIRE(fields.x == 10);
        } else {
            REQUIRE(sent.first == 2);
            REQUIRE(fields.x == 20);
        }
    }
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
    options.mtu_bytes = 56;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    REQUIRE(payloads[0].byte_size() == 25);
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
    options.mtu_bytes = 17;
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
        REQUIRE(payload.byte_size() == 17);
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
    options.bandwidth_limit_bytes_per_tick = 17;
    options.mtu_bytes = 17;
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
    payloads.clear();

    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].destroy);
    REQUIRE(update.entities[0].network_id != 0);
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
    REQUIRE(resent_destroy.entities[0].network_id == first_destroy.entities[0].network_id);
    REQUIRE(resent_destroy.frame > first_destroy.frame);

    REQUIRE(server.process_packet(1, write_ack_packet(resent_destroy.packet_id)));
    payloads.clear();
    server.tick(registry);
    REQUIRE(payloads.empty());
}

TEST_CASE("replication server does not reuse client-local network ids before destroy ACK") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    auto spawn = [&](float x) {
        const ecs::Entity entity = registry.create();
        REQUIRE(registry.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{x, x}) != nullptr);
        REQUIRE(start_sync(registry, entity, archetype));
        return entity;
    };

    const ecs::Entity first = spawn(1.0f);
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket first_update =
        read_server_update(payloads.back(), 2U, sizeof(kage_sync_tests::Position) * 8U);
    REQUIRE(first_update.entities.size() == 1);
    const std::uint32_t first_network_id = first_update.entities[0].network_id;
    REQUIRE(first_network_id != 0);

    payloads.clear();
    REQUIRE(registry.destroy(first));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket destroy_update =
        read_server_update(payloads.back(), 2U, sizeof(kage_sync_tests::Position) * 8U);
    REQUIRE(destroy_update.entities.size() == 1);
    REQUIRE(destroy_update.entities[0].destroy);

    payloads.clear();
    spawn(2.0f);
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket second_update =
        read_server_update(payloads.back(), 2U, sizeof(kage_sync_tests::Position) * 8U);
    REQUIRE(second_update.entities.size() == 2);
    const auto upsert = std::find_if(
        second_update.entities.begin(),
        second_update.entities.end(),
        [](const EntityRecord& record) {
            return !record.destroy;
        });
    REQUIRE(upsert != second_update.entities.end());
    REQUIRE(upsert->network_id != first_network_id);
}

TEST_CASE("replication server reuses client-local network ids after each client's destroy ACK") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));

    auto spawn = [&](float x) {
        const ecs::Entity entity = registry.create();
        REQUIRE(registry.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{x, x}) != nullptr);
        REQUIRE(start_sync(registry, entity, archetype));
        return entity;
    };
    auto update_for = [&](kage::sync::ClientId client) {
        for (const auto& sent : payloads) {
            if (sent.first != client) {
                continue;
            }
            ServerUpdatePacket update = read_server_update(sent.second, 2U, sizeof(kage_sync_tests::Position) * 8U);
            const auto found = std::find_if(update.entities.begin(), update.entities.end(), [](const EntityRecord& record) {
                return !record.destroy;
            });
            if (found != update.entities.end()) {
                return update;
            }
        }
        return ServerUpdatePacket{};
    };
    auto destroy_for = [&](kage::sync::ClientId client, std::uint32_t network_id) {
        for (const auto& sent : payloads) {
            if (sent.first != client) {
                continue;
            }
            ServerUpdatePacket update = read_server_update(sent.second, 2U, sizeof(kage_sync_tests::Position) * 8U);
            const auto found = std::find_if(update.entities.begin(), update.entities.end(), [&](const EntityRecord& record) {
                return record.destroy && record.network_id == network_id;
            });
            if (found != update.entities.end()) {
                return update;
            }
        }
        return ServerUpdatePacket{};
    };
    auto packet_id = [](kage::sync::BitBuffer packet) {
        packet.read_bits(8U);
        packet.read_bits(32U);
        return static_cast<std::uint32_t>(packet.read_bits(kage::sync::protocol::server_packet_id_bits));
    };

    const ecs::Entity first = spawn(1.0f);
    server.tick(registry);
    ServerUpdatePacket first_update = update_for(1);
    REQUIRE(first_update.entities.size() == 1);
    const std::uint32_t reusable_network_id = first_update.entities[0].network_id;
    REQUIRE(reusable_network_id != 0);

    payloads.clear();
    REQUIRE(registry.destroy(first));
    server.tick(registry);
    ServerUpdatePacket client_one_destroy = destroy_for(1, reusable_network_id);
    ServerUpdatePacket client_two_destroy = destroy_for(2, reusable_network_id);
    REQUIRE(client_one_destroy.entities.size() == 1);
    REQUIRE(client_two_destroy.entities.size() == 1);
    REQUIRE(server.process_packet(1, write_ack_packet(client_one_destroy.packet_id)));

    payloads.clear();
    spawn(2.0f);
    server.tick(registry);
    ServerUpdatePacket second_update = update_for(1);
    REQUIRE(second_update.entities.size() == 1);
    REQUIRE(second_update.entities[0].network_id == reusable_network_id);
    ServerUpdatePacket client_two_second_update = update_for(2);
    REQUIRE(client_two_second_update.entities.size() == 2);
    const auto client_two_second = std::find_if(
        client_two_second_update.entities.begin(),
        client_two_second_update.entities.end(),
        [](const EntityRecord& record) {
            return !record.destroy;
        });
    REQUIRE(client_two_second != client_two_second_update.entities.end());
    REQUIRE(client_two_second->network_id != reusable_network_id);

    for (const auto& sent : payloads) {
        REQUIRE(server.process_packet(sent.first, write_ack_packet(packet_id(sent.second))));
    }
    payloads.clear();
    spawn(3.0f);
    server.tick(registry);
    ServerUpdatePacket third_update = update_for(2);
    const auto third_record = std::find_if(
        third_update.entities.begin(),
        third_update.entities.end(),
        [](const EntityRecord& record) {
            return !record.destroy;
        });
    REQUIRE(third_record != third_update.entities.end());
    const bool saw_reused_network_id = std::any_of(
        third_update.entities.begin(),
        third_update.entities.end(),
        [reusable_network_id](const EntityRecord& record) {
            return !record.destroy && record.network_id == reusable_network_id;
        });
    REQUIRE(saw_reused_network_id);
}

TEST_CASE("replication server reuses network ids immediately when no clients have pending destroys") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    const ecs::Entity first = registry.create();
    REQUIRE(registry.add<kage_sync_tests::Position>(first, kage_sync_tests::Position{1.0f, 1.0f}) != nullptr);
    REQUIRE(start_sync(registry, first, archetype));
    server.tick(registry);
    REQUIRE(registry.destroy(first));
    server.tick(registry);

    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<kage_sync_tests::Position>(second, kage_sync_tests::Position{2.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, second, archetype));
    REQUIRE(server.add_client(1));
    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].network_id == 1);
}

TEST_CASE("replication server accepts delayed entity ACKs for retained quantized frames") {
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

TEST_CASE("replication server shares ACKed quantized frames across clients and frees them") {
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
    REQUIRE(server.retained_quantized_frame_count() == 1);
    REQUIRE(server.retained_quantized_frame_bytes() == sizeof(kage_sync_tests::QuantizedNetworkedPosition));

    REQUIRE(server.acknowledge_entity(1, entity, read_server_update(payloads[0].second).frame));
    REQUIRE(server.acknowledge_entity(2, entity, read_server_update(payloads[1].second).frame));
    REQUIRE(server.retained_quantized_frame_count() == 1);
    REQUIRE(server.retained_quantized_frame_bytes() == sizeof(kage_sync_tests::QuantizedNetworkedPosition));

    REQUIRE(server.remove_client(1));
    REQUIRE(server.retained_quantized_frame_count() == 1);
    REQUIRE(server.remove_client(2));
    REQUIRE(server.retained_quantized_frame_count() == 0);
    REQUIRE(server.retained_quantized_frame_bytes() == 0);
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

    std::uint32_t client_two_packet_id = 0;
    for (const auto& sent : payloads) {
        if (sent.first == 2) {
            client_two_packet_id = read_server_update(sent.second).packet_id;
        }
    }
    REQUIRE(client_two_packet_id != 0);

    REQUIRE(server.remove_client(1));
    REQUIRE(server.has_client(2));
    REQUIRE(server.process_packet(2, write_ack_packet(client_two_packet_id)));
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
    REQUIRE(payloads.back().byte_size() == 19);
    REQUIRE(server.acknowledge_entity(1, entity, read_server_update(payloads.back()).frame));

    registry.write<BandwidthProbe>(entity) = BandwidthProbe{105};
    server.tick(registry);

    const std::size_t expected_delta_bits = kage::sync::protocol::server_update_header_bits +
        1U + kage::sync::protocol::network_entity_id_encoded_bits(1U) + 1U +
        (1U + kage::sync::protocol::baseline_frame_delta_bits) + 1U + 9U;
    REQUIRE(payloads.back().byte_size() == kage::sync::protocol::bytes_for_bits(expected_delta_bits));
}
