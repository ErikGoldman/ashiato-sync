#include "test_protocol.hpp"

#include "kage/sync/simulated_link.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

kage::sync::SyncArchetypeId define_networked_position_archetype(ecs::Registry& registry) {
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    return kage::sync::define_archetype(
        registry,
        "NetworkedPositionActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
}

TEST_CASE("replication server rejects malformed ACK packets") {
    kage::sync::ReplicationServer server;
    REQUIRE(server.add_client(1));

    ecs::BitBuffer empty;
    REQUIRE_FALSE(server.process_packet(1, empty));

    ecs::BitBuffer wrong_message;
    wrong_message.push_bits(kage::sync::protocol::server_update_message, 8U);
    wrong_message.push_bits(0, 16U);
    REQUIRE_FALSE(server.process_packet(1, wrong_message));

    ecs::BitBuffer truncated_record;
    truncated_record.push_bits(kage::sync::protocol::client_ack_message, 8U);
    truncated_record.push_bits(1, 16U);
    truncated_record.push_bool(false);
    REQUIRE_FALSE(server.process_packet(1, truncated_record));

    REQUIRE_FALSE(server.process_packet(99, write_ack_packet(42)));
    REQUIRE_FALSE(server.process_packet(1, write_ack_packet(42)));
}

TEST_CASE("replication server rejects malformed connect ack and input packets") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServer server;

    ecs::BitBuffer malformed_connect;
    malformed_connect.push_bits(kage::sync::protocol::client_connect_request_message, 8U);
    REQUIRE_FALSE(server.process_packet(registry, 10, malformed_connect));

    REQUIRE(server.add_client(1));

    ecs::BitBuffer truncated_connect_ack;
    truncated_connect_ack.push_bits(kage::sync::protocol::client_connect_ack_message, 8U);
    truncated_connect_ack.push_bits(1, 8U);
    REQUIRE_FALSE(server.process_packet(registry, 1, truncated_connect_ack));

    ecs::BitBuffer wrong_connect_ack;
    wrong_connect_ack.push_bits(kage::sync::protocol::client_connect_ack_message, 8U);
    wrong_connect_ack.push_unsigned_bits(2, 64U);
    REQUIRE_FALSE(server.process_packet(registry, 1, wrong_connect_ack));

    ecs::BitBuffer input_without_registry;
    input_without_registry.push_bits(kage::sync::protocol::client_input_message, 8U);
    REQUIRE_FALSE(server.process_packet(1, input_without_registry));

    ecs::BitBuffer truncated_input;
    truncated_input.push_bits(kage::sync::protocol::client_input_message, 8U);
    truncated_input.push_bits(0, 16U);
    REQUIRE_FALSE(server.process_packet(registry, 1, truncated_input));
}

TEST_CASE("replication server respects per-client bandwidth limits") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    std::vector<ecs::Entity> entities;
    for (int i = 0; i < 3; ++i) {
        const ecs::Entity entity = registry.create();
        REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
        entities.push_back(entity);
    }

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& payload) {
        REQUIRE(client == 1);
        payloads.push_back(payload);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    for (const ecs::Entity entity : entities) {
        REQUIRE(start_sync(registry, entity, archetype));
    }

    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    std::vector<ecs::Entity> sent;
    for (const EntityRecord& record : update.entities) {
        sent.push_back(entities[record.network_id - 1U]);
    }
    for (const ecs::Entity entity : sent) {
        REQUIRE(server.priority(1, entity) == 0);
    }

    std::size_t unsent_count = 0;
    for (const ecs::Entity entity : entities) {
        if (std::find(sent.begin(), sent.end(), entity) == sent.end()) {
            ++unsent_count;
            REQUIRE(server.priority(1, entity) == 1);
        }
    }
    REQUIRE(unsent_count == 2);
}

TEST_CASE("replication server dynamic bandwidth charges transport overhead") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.bandwidth.enabled = true;
    options.bandwidth.min_bytes_per_second = 60;
    options.bandwidth.initial_bytes_per_second = 60;
    options.bandwidth.max_bytes_per_second = 60;
    options.bandwidth.max_burst_bytes = 21;
    options.bandwidth.transport_overhead_bytes_per_packet = 28;
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& payload) {
        payloads.push_back(payload);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    server.tick(registry);

    REQUIRE(payloads.empty());
    REQUIRE(server.priority(1, entity) == 1);
}

TEST_CASE("replication server dynamic bandwidth sends when charged packet fits") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth.enabled = true;
    options.bandwidth.min_bytes_per_second = 49;
    options.bandwidth.initial_bytes_per_second = 49;
    options.bandwidth.max_bytes_per_second = 49;
    options.bandwidth.max_burst_bytes = 49;
    options.bandwidth.transport_overhead_bytes_per_packet = 28;
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& payload) {
        payloads.push_back(payload);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    REQUIRE(read_server_update(payloads.back()).entities.size() == 1);
    REQUIRE(server.priority(1, entity) == 0);
}

TEST_CASE("replication server prioritizes pending destroys over creates when bandwidth limited") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ecs::Entity destroyed = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(destroyed, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, destroyed, archetype));

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [](
        kage::sync::ClientId,
        const std::vector<kage::sync::ReplicationPriorityObject>& objects,
        std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        decisions.resize(objects.size());
        for (kage::sync::ReplicationPriorityDecision& decision : decisions) {
            decision.replicate = true;
            decision.priority = 1000;
            decision.component_mask = std::numeric_limits<std::uint64_t>::max();
        }
    };
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& payload) {
        payloads.push_back(payload);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket initial = read_server_update(payloads.back());
    REQUIRE(initial.entities.size() == 1);
    REQUIRE(initial.entities[0].network_id == 1U);
    REQUIRE(server.acknowledge_entity(1, destroyed, initial.frame));

    REQUIRE(registry.destroy(destroyed));
    const ecs::Entity created = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(created, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, created, archetype));

    payloads.clear();
    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].destroy);
    REQUIRE(update.entities[0].network_id == 1U);
    REQUIRE(server.priority(1, created) == 1);
}

TEST_CASE("replication server parallel path prioritizes pending destroys over creates when bandwidth limited") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ecs::Entity destroyed = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(destroyed, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, destroyed, archetype));

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.serialized_worker_threads = 2;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [](
        kage::sync::ClientId,
        const std::vector<kage::sync::ReplicationPriorityObject>& objects,
        std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        decisions.resize(objects.size());
        for (kage::sync::ReplicationPriorityDecision& decision : decisions) {
            decision.replicate = true;
            decision.priority = 1000;
            decision.component_mask = std::numeric_limits<std::uint64_t>::max();
        }
    };
    options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& payload) {
        payloads.emplace_back(client, payload);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));

    server.tick(registry);
    REQUIRE(payloads.size() == 2);
    for (const kage::sync::ClientId client : {1ULL, 2ULL}) {
        const ServerUpdatePacket initial = read_server_update(packet_for(payloads, client));
        REQUIRE(initial.entities.size() == 1);
        REQUIRE(initial.entities[0].network_id == 1U);
        REQUIRE(server.acknowledge_entity(client, destroyed, initial.frame));
    }

    REQUIRE(registry.destroy(destroyed));
    const ecs::Entity created = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(created, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, created, archetype));

    payloads.clear();
    server.tick(registry);

    REQUIRE(payloads.size() == 2);
    for (const kage::sync::ClientId client : {1ULL, 2ULL}) {
        const ServerUpdatePacket update = read_server_update(packet_for(payloads, client));
        REQUIRE(update.entities.size() == 1);
        REQUIRE(update.entities[0].destroy);
        REQUIRE(update.entities[0].network_id == 1U);
        REQUIRE(server.priority(client, created) == 1);
    }
}

TEST_CASE("replication server parallel path applies bandwidth limits independently per client") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    std::vector<ecs::Entity> entities;
    for (int i = 0; i < 3; ++i) {
        const ecs::Entity entity = registry.create();
        REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
        REQUIRE(start_sync(registry, entity, archetype));
        entities.push_back(entity);
    }

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.serialized_worker_threads = 2;
    options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& payload) {
        packets.emplace_back(client, payload);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));

    server.tick(registry);

    REQUIRE(packets.size() == 2);
    for (const kage::sync::ClientId client : {1ULL, 2ULL}) {
        const ServerUpdatePacket update = read_server_update(packet_for(packets, client));
        REQUIRE(update.entities.size() == 1);

        std::size_t unsent_count = 0;
        for (const ecs::Entity entity : entities) {
            if (entity != entities[update.entities[0].network_id - 1U]) {
                ++unsent_count;
                REQUIRE(server.priority(client, entity) == 1);
            }
        }
        REQUIRE(unsent_count == 2);
    }
}

TEST_CASE("replication server parallel path batches records when budget allows") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    for (int i = 0; i < 3; ++i) {
        const ecs::Entity entity = registry.create();
        REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
        REQUIRE(start_sync(registry, entity, archetype));
    }

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.serialized_worker_threads = 2;
    options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& payload) {
        packets.emplace_back(client, payload);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));

    server.tick(registry);

    REQUIRE(packets.size() == 2);
    for (const kage::sync::ClientId client : {1ULL, 2ULL}) {
        const ServerUpdatePacket update = read_server_update(packet_for(packets, client));
        REQUIRE(update.entities.size() == 3);
    }
}

TEST_CASE("replication server rotates budget-limited sends by priority") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    std::vector<ecs::Entity> entities;
    for (int i = 0; i < 3; ++i) {
        const ecs::Entity entity = registry.create();
        REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
        entities.push_back(entity);
    }

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& payload) {
        payloads.push_back(payload);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    for (const ecs::Entity entity : entities) {
        REQUIRE(start_sync(registry, entity, archetype));
    }

    std::vector<ecs::Entity> sent;
    for (int i = 0; i < 3; ++i) {
        server.tick(registry);
        const ServerUpdatePacket update = read_server_update(payloads.back());
        REQUIRE(update.entities.size() == 1);
        sent.push_back(entities[update.entities[0].network_id - 1U]);
    }

    REQUIRE(sent.size() == 3);
    for (const ecs::Entity entity : entities) {
        REQUIRE(std::find(sent.begin(), sent.end(), entity) != sent.end());
        REQUIRE(server.priority(1, entity) <= 2);
    }
}

TEST_CASE("replication server keeps per-client priorities independent") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{}) != nullptr);

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));
    REQUIRE(server.add_client(2));

    server.tick(registry);
    for (const auto& [client, payload] : payloads) {
        const ServerUpdatePacket update = read_server_update(payload);
        REQUIRE((client == 1 || client == 2));
        REQUIRE(update.entities.size() == 1);
    }

    REQUIRE(payloads.size() == 2);
    REQUIRE(server.priority(1, first) == 0);
    REQUIRE(server.priority(2, first) == 0);
    REQUIRE(server.priority(1, second) == 1);
    REQUIRE(server.priority(2, second) == 1);
}

TEST_CASE("replication server skips destroyed and externally unmarked entities") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ecs::Entity destroyed = registry.create();
    const ecs::Entity unmarked = registry.create();
    const ecs::Entity live = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(destroyed, NetworkedPosition{}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(unmarked, NetworkedPosition{}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(live, NetworkedPosition{}) != nullptr);

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 512;
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& payload) {
        payloads.push_back(payload);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, destroyed, archetype));
    REQUIRE(start_sync(registry, unmarked, archetype));
    REQUIRE(start_sync(registry, live, archetype));

    REQUIRE(registry.destroy(destroyed));
    REQUIRE(registry.remove<kage::sync::Replicated>(unmarked));

    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].full);
    REQUIRE_FALSE(server.is_replicated(destroyed));
    REQUIRE_FALSE(server.is_replicated(unmarked));
    REQUIRE(server.is_replicated(live));
    REQUIRE(server.replicated_count() == 1);
}

TEST_CASE("replication server sends nothing when bandwidth cannot cover a serialized record") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);

    int sends = 0;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 20;
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer&) {
        ++sends;
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);

    REQUIRE(sends == 0);
    REQUIRE(server.priority(1, entity) == 1);
}

TEST_CASE("replication server parallel path releases quantized frames for unsent records") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 20;
    options.serialized_worker_threads = 2;
    options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& payload) {
        packets.emplace_back(client, payload);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));

    server.tick(registry);

    REQUIRE(packets.empty());
    REQUIRE(server.retained_quantized_frame_count() == 0U);
    REQUIRE(server.retained_quantized_frame_bytes() == 0U);
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

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& payload) {
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

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& payload) {
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

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& payload) {
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
    options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& payload) {
        sends.push_back({client, payload.byte_size()});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);

    REQUIRE(sends.size() == 2);
    REQUIRE(sends[0] == std::pair<kage::sync::ClientId, std::size_t>{1, 21});
    REQUIRE(sends[1] == std::pair<kage::sync::ClientId, std::size_t>{2, 25});
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

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);
    REQUIRE(payloads.size() == 2);
    for (auto sent : payloads) {
        ecs::BitBuffer packet = sent.second;
        REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::server_update_message);
        const auto frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
        packet.read_bits(kage::sync::protocol::server_packet_id_bits);
        packet.read_bits(32U);
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
        ecs::BitBuffer packet = sent.second;
        packet.read_bits(8U);
        const auto frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
        packet.read_bits(kage::sync::protocol::server_packet_id_bits);
        packet.read_bits(32U);
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

    std::vector<ecs::BitBuffer> payloads;
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
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& payload) {
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

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.mtu_bytes = 56;
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    REQUIRE(payloads[0].byte_size() == 29);
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

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.mtu_bytes = 21;
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    server.tick(registry);

    REQUIRE(payloads.size() == 2);
    for (const ecs::BitBuffer& payload : payloads) {
        REQUIRE(payload.byte_size() <= options.mtu_bytes);
        REQUIRE(payload.byte_size() == 21);
        REQUIRE(read_server_update(payload).entities.size() == 1);
    }
}
