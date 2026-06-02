#include "test_protocol.hpp"

#include "ashiato/sync/simulated_link.hpp"

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

using namespace ashiato_sync_tests;

ashiato::sync::SyncArchetypeId define_networked_position_archetype(ashiato::Registry& registry) {
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    return ashiato::sync::define_archetype(
        registry,
        "NetworkedPositionActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
}

void run_server_tick(ashiato::sync::ReplicationServer& server, ashiato::Registry& registry) {
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
}

TEST_CASE("replication server rejects malformed ACK packets") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    ashiato::sync::ReplicationServer server(registry);
    REQUIRE(server.add_client(1));

    ashiato::BitBuffer empty;
    REQUIRE_FALSE(server.process_packet(registry, 1, empty));

    ashiato::BitBuffer wrong_message;
    wrong_message.write_bits(ashiato::sync::protocol::server_update_message, ashiato::sync::protocol::message_bits);
    wrong_message.write_bits(0, ashiato::sync::protocol::ack_count_bits);
    REQUIRE_FALSE(server.process_packet(registry, 1, wrong_message));

    ashiato::BitBuffer truncated_record;
    truncated_record.write_bits(ashiato::sync::protocol::client_ack_message, ashiato::sync::protocol::message_bits);
    truncated_record.write_bits(1, ashiato::sync::protocol::ack_count_bits);
    truncated_record.write_bool(false);
    REQUIRE_FALSE(server.process_packet(registry, 1, truncated_record));

    REQUIRE_FALSE(server.process_packet(registry, 99, write_ack_packet(42)));
    REQUIRE_FALSE(server.process_packet(registry, 1, write_ack_packet(42)));
}

TEST_CASE("replication server rejects malformed connect ack and input packets") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    ashiato::sync::ReplicationServer server(registry);

    ashiato::BitBuffer malformed_connect;
    malformed_connect.write_bits(ashiato::sync::protocol::client_connect_request_message, ashiato::sync::protocol::message_bits);
    REQUIRE_FALSE(server.process_packet(registry, 10, malformed_connect));

    REQUIRE(server.add_client(1));

    ashiato::BitBuffer truncated_connect_ack;
    truncated_connect_ack.write_bits(ashiato::sync::protocol::client_connect_ack_message, ashiato::sync::protocol::message_bits);
    truncated_connect_ack.write_bits(1, ashiato::sync::protocol::client_id_bits - 1U);
    REQUIRE_FALSE(server.process_packet(registry, 1, truncated_connect_ack));

    ashiato::BitBuffer wrong_connect_ack;
    wrong_connect_ack.write_bits(ashiato::sync::protocol::client_connect_ack_message, ashiato::sync::protocol::message_bits);
    wrong_connect_ack.write_unsigned_bits(2, 64U);
    REQUIRE_FALSE(server.process_packet(registry, 1, wrong_connect_ack));

    ashiato::BitBuffer truncated_input;
    truncated_input.write_bits(ashiato::sync::protocol::client_input_message, ashiato::sync::protocol::message_bits);
    truncated_input.write_bits(0, ashiato::sync::protocol::ack_count_bits);
    REQUIRE_FALSE(server.process_packet(registry, 1, truncated_input));
}

TEST_CASE("replication server respects per-client bandwidth limits") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    std::vector<ashiato::Entity> entities;
    for (int i = 0; i < 3; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
        entities.push_back(entity);
    }

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& payload) {
        REQUIRE(client == 1);
        payloads.push_back(payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    for (const ashiato::Entity entity : entities) {
        REQUIRE(start_sync(registry, entity, archetype));
    }

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    std::vector<ashiato::Entity> sent;
    for (const EntityRecord& record : update.entities) {
        sent.push_back(entities[record.network_id - 1U]);
    }
    std::size_t unsent_count = 0;
    for (const ashiato::Entity entity : entities) {
        if (std::find(sent.begin(), sent.end(), entity) == sent.end()) {
            ++unsent_count;
        }
    }
    REQUIRE(unsent_count == 2);
}

TEST_CASE("replication server dynamic bandwidth charges transport overhead") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.bandwidth.enabled = true;
    options.bandwidth.min_bytes_per_second = 60;
    options.bandwidth.initial_bytes_per_second = 60;
    options.bandwidth.max_bytes_per_second = 60;
    options.bandwidth.max_burst_bytes = 21;
    options.bandwidth.transport_overhead_bytes_per_packet = 28;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        payloads.push_back(payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));

    run_server_tick(server, registry);

    REQUIRE(payloads.empty());
}

TEST_CASE("replication server dynamic bandwidth sends when charged packet fits") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth.enabled = true;
    options.bandwidth.min_bytes_per_second = 49;
    options.bandwidth.initial_bytes_per_second = 49;
    options.bandwidth.max_bytes_per_second = 49;
    options.bandwidth.max_burst_bytes = 49;
    options.bandwidth.transport_overhead_bytes_per_packet = 28;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        payloads.push_back(payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));

    run_server_tick(server, registry);

    REQUIRE(payloads.size() == 1);
    REQUIRE(read_server_update(payloads.back()).entities.size() == 1);
}

TEST_CASE("shared dynamic bandwidth budget splits replay and live before redistributing") {
    ashiato::Registry replay_registry;
    const ashiato::sync::SyncArchetypeId replay_archetype = define_networked_position_archetype(replay_registry);
    const ashiato::Entity replay_entity = replay_registry.create();
    REQUIRE(replay_registry.add<NetworkedPosition>(replay_entity, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(replay_registry, replay_entity, replay_archetype));

    ashiato::Registry live_registry;
    const ashiato::sync::SyncArchetypeId live_archetype = define_networked_position_archetype(live_registry);
    const ashiato::Entity live_entity = live_registry.create();
    REQUIRE(live_registry.add<NetworkedPosition>(live_entity, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(live_registry, live_entity, live_archetype));

    std::vector<ashiato::BitBuffer> replay_payloads;
    std::vector<ashiato::BitBuffer> live_payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth.enabled = true;
    options.bandwidth.min_bytes_per_second = 49;
    options.bandwidth.initial_bytes_per_second = 49;
    options.bandwidth.max_bytes_per_second = 49;
    options.bandwidth.max_burst_bytes = 98;
    options.bandwidth.transport_overhead_bytes_per_packet = 28;

    ashiato::sync::ReplicationServer replay_server(replay_registry, options);
    ashiato::sync::ReplicationServer live_server(live_registry, options);
    replay_server.set_transport([&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        replay_payloads.push_back(payload);
    });
    live_server.set_transport([&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        live_payloads.push_back(payload);
    });
    REQUIRE(replay_server.add_client(1));
    REQUIRE(live_server.add_client(1));
    ashiato::sync::ReplicationReplayStreamer replay_streamer;
    REQUIRE(replay_streamer.attach_network_session_bandwidth(
        replay_server,
        ashiato::sync::ReplicationReplayNetworkSessionOptions{
            &live_server,
            1,
            ashiato::sync::ReplicationBandwidthParticipantOptions{1U, 1}}));

    run_server_tick(replay_server, replay_registry);
    run_server_tick(live_server, live_registry);

    REQUIRE(replay_payloads.size() == 1);
    REQUIRE(live_payloads.size() == 1);

    const ServerUpdatePacket replay_update = read_server_update(replay_payloads.back());
    REQUIRE(replay_server.process_packet(1, write_ack_packet(replay_update.packet_id)));
    REQUIRE(live_server.bandwidth_stats(1).delivered_bytes_window == 48);
}

TEST_CASE("live replication uses shared bandwidth when replay has nothing to send") {
    ashiato::Registry replay_registry;
    (void)define_networked_position_archetype(replay_registry);

    ashiato::Registry live_registry;
    const ashiato::sync::SyncArchetypeId live_archetype = define_networked_position_archetype(live_registry);
    const ashiato::Entity live_entity = live_registry.create();
    REQUIRE(live_registry.add<NetworkedPosition>(live_entity, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(live_registry, live_entity, live_archetype));

    std::vector<ashiato::BitBuffer> replay_payloads;
    std::vector<ashiato::BitBuffer> live_payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth.enabled = true;
    options.bandwidth.min_bytes_per_second = 49;
    options.bandwidth.initial_bytes_per_second = 49;
    options.bandwidth.max_bytes_per_second = 49;
    options.bandwidth.max_burst_bytes = 49;
    options.bandwidth.transport_overhead_bytes_per_packet = 28;

    ashiato::sync::ReplicationServer replay_server(replay_registry, options);
    ashiato::sync::ReplicationServer live_server(live_registry, options);
    replay_server.set_transport([&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        replay_payloads.push_back(payload);
    });
    live_server.set_transport([&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        live_payloads.push_back(payload);
    });
    REQUIRE(replay_server.add_client(1));
    REQUIRE(live_server.add_client(1));
    ashiato::sync::ReplicationReplayStreamer replay_streamer;
    REQUIRE(replay_streamer.attach_network_session_bandwidth(
        replay_server,
        ashiato::sync::ReplicationReplayNetworkSessionOptions{
            &live_server,
            1,
            ashiato::sync::ReplicationBandwidthParticipantOptions{1U, 1}}));

    run_server_tick(replay_server, replay_registry);
    run_server_tick(live_server, live_registry);

    REQUIRE(replay_payloads.empty());
    REQUIRE(live_payloads.size() == 1);
}

TEST_CASE("detaching replay participant releases queued live bandwidth flush") {
    ashiato::Registry replay_registry;
    (void)define_networked_position_archetype(replay_registry);

    ashiato::Registry live_registry;
    const ashiato::sync::SyncArchetypeId live_archetype = define_networked_position_archetype(live_registry);
    const ashiato::Entity live_entity = live_registry.create();
    REQUIRE(live_registry.add<NetworkedPosition>(live_entity, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(live_registry, live_entity, live_archetype));

    std::vector<ashiato::BitBuffer> live_payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth.enabled = true;
    options.bandwidth.min_bytes_per_second = 49;
    options.bandwidth.initial_bytes_per_second = 49;
    options.bandwidth.max_bytes_per_second = 49;
    options.bandwidth.max_burst_bytes = 49;
    options.bandwidth.transport_overhead_bytes_per_packet = 28;

    ashiato::sync::ReplicationServer replay_server(replay_registry, options);
    ashiato::sync::ReplicationServer live_server(live_registry, options);
    replay_server.set_transport([](ashiato::sync::ClientId, const ashiato::BitBuffer&) {});
    live_server.set_transport([&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        live_payloads.push_back(payload);
    });
    REQUIRE(replay_server.add_client(1));
    REQUIRE(live_server.add_client(1));
    ashiato::sync::ReplicationReplayStreamer replay_streamer;
    REQUIRE(replay_streamer.attach_network_session_bandwidth(
        replay_server,
        ashiato::sync::ReplicationReplayNetworkSessionOptions{
            &live_server,
            1,
            ashiato::sync::ReplicationBandwidthParticipantOptions{1U, 1}}));

    run_server_tick(live_server, live_registry);
    REQUIRE(live_payloads.empty());

    REQUIRE(replay_server.remove_client(replay_registry, 1));
    REQUIRE(live_payloads.size() == 1);
}

TEST_CASE("client bandwidth share options are tunable after joining a shared budget") {
    ashiato::Registry live_registry;
    ashiato::Registry replay_registry;
    ashiato::sync::ReplicationServerOptions options;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};

    ashiato::sync::ReplicationServer live_server(live_registry, options);
    ashiato::sync::ReplicationServer replay_server(replay_registry, options);
    REQUIRE(live_server.add_client(1));
    REQUIRE(replay_server.add_client(1));
    REQUIRE(replay_server.set_client_bandwidth_budget(
        1,
        live_server.client_bandwidth_budget(1),
        ashiato::sync::ReplicationBandwidthParticipantOptions{1U, 1}));

    REQUIRE(live_server.set_client_bandwidth_share(1, ashiato::sync::ReplicationBandwidthParticipantOptions{1U, 0}));
    REQUIRE(replay_server.set_client_bandwidth_share(1, ashiato::sync::ReplicationBandwidthParticipantOptions{3U, 1}));

    const ashiato::sync::ReplicationBandwidthParticipantOptions live_share = live_server.client_bandwidth_share(1);
    const ashiato::sync::ReplicationBandwidthParticipantOptions replay_share = replay_server.client_bandwidth_share(1);
    REQUIRE(live_share.weight == 1U);
    REQUIRE(live_share.priority == 0);
    REQUIRE(replay_share.weight == 3U);
    REQUIRE(replay_share.priority == 1);
}

TEST_CASE("replication server prioritizes pending destroys over creates when bandwidth limited") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ashiato::Entity destroyed = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(destroyed, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, destroyed, archetype));

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [](ashiato::sync::ClientId, ashiato::sync::ReplicationPriorityObject) {
        ashiato::sync::ReplicationPriorityDecision decision;
        decision.priority = 1000.0f;
        decision.component_mask = std::numeric_limits<std::uint64_t>::max();
        return decision;
    };
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        payloads.push_back(payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));

    run_server_tick(server, registry);
    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket initial = read_server_update(payloads.back());
    REQUIRE(initial.entities.size() == 1);
    REQUIRE(initial.entities[0].network_id == 1U);
    REQUIRE(server.acknowledge_entity(1, destroyed, initial.frame));

    REQUIRE(registry.destroy(destroyed));
    const ashiato::Entity created = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(created, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, created, archetype));

    payloads.clear();
    run_server_tick(server, registry);

    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].destroy);
    REQUIRE(update.entities[0].network_id == 1U);
}

TEST_CASE("replication server parallel path prioritizes pending destroys over creates when bandwidth limited") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ashiato::Entity destroyed = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(destroyed, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, destroyed, archetype));

    std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.serialized_worker_threads = 2;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [](ashiato::sync::ClientId, ashiato::sync::ReplicationPriorityObject) {
        ashiato::sync::ReplicationPriorityDecision decision;
        decision.priority = 1000.0f;
        decision.component_mask = std::numeric_limits<std::uint64_t>::max();
        return decision;
    };
    options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& payload) {
        payloads.emplace_back(client, payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));

    run_server_tick(server, registry);
    REQUIRE(payloads.size() == 2);
    for (const ashiato::sync::ClientId client : {1ULL, 2ULL}) {
        const ServerUpdatePacket initial = read_server_update(packet_for(payloads, client));
        REQUIRE(initial.entities.size() == 1);
        REQUIRE(initial.entities[0].network_id == 1U);
        REQUIRE(server.acknowledge_entity(client, destroyed, initial.frame));
    }

    REQUIRE(registry.destroy(destroyed));
    const ashiato::Entity created = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(created, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, created, archetype));

    payloads.clear();
    run_server_tick(server, registry);

    REQUIRE(payloads.size() == 2);
    for (const ashiato::sync::ClientId client : {1ULL, 2ULL}) {
        const ServerUpdatePacket update = read_server_update(packet_for(payloads, client));
        REQUIRE(update.entities.size() == 1);
        REQUIRE(update.entities[0].destroy);
        REQUIRE(update.entities[0].network_id == 1U);
    }
}

TEST_CASE("replication server parallel path applies bandwidth limits independently per client") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    std::vector<ashiato::Entity> entities;
    for (int i = 0; i < 3; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
        REQUIRE(start_sync(registry, entity, archetype));
        entities.push_back(entity);
    }

    std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> packets;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.serialized_worker_threads = 2;
    options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& payload) {
        packets.emplace_back(client, payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(packets.size() == 2);
    for (const ashiato::sync::ClientId client : {1ULL, 2ULL}) {
        const ServerUpdatePacket update = read_server_update(packet_for(packets, client));
        REQUIRE(update.entities.size() == 1);

        std::size_t unsent_count = 0;
        for (const ashiato::Entity entity : entities) {
            if (entity != entities[update.entities[0].network_id - 1U]) {
                ++unsent_count;
            }
        }
        REQUIRE(unsent_count == 2);
    }
}

TEST_CASE("replication server parallel path batches records when budget allows") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    for (int i = 0; i < 3; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
        REQUIRE(start_sync(registry, entity, archetype));
    }

    std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> packets;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.serialized_worker_threads = 2;
    options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& payload) {
        packets.emplace_back(client, payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(packets.size() == 2);
    for (const ashiato::sync::ClientId client : {1ULL, 2ULL}) {
        const ServerUpdatePacket update = read_server_update(packet_for(packets, client));
        REQUIRE(update.entities.size() == 3);
    }
}

TEST_CASE("replication server rotates budget-limited sends by priority") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    std::vector<ashiato::Entity> entities;
    for (int i = 0; i < 3; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
        entities.push_back(entity);
    }

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        payloads.push_back(payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    for (const ashiato::Entity entity : entities) {
        REQUIRE(start_sync(registry, entity, archetype));
    }

    std::vector<ashiato::Entity> sent;
    for (int i = 0; i < 3; ++i) {
        server.tick(registry, server.options().fixed_dt_seconds);
        const ServerUpdatePacket update = read_server_update(payloads.back());
        REQUIRE(update.entities.size() == 1);
        sent.push_back(entities[update.entities[0].network_id - 1U]);
    }

    REQUIRE(sent.size() == 3);
    for (const ashiato::Entity entity : entities) {
        REQUIRE(std::find(sent.begin(), sent.end(), entity) != sent.end());
    }
}

TEST_CASE("replication server keeps per-client dirty queues independent") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ashiato::Entity first = registry.create();
    const ashiato::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{}) != nullptr);

    std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));
    REQUIRE(server.add_client(2));

    server.tick(registry, server.options().fixed_dt_seconds);
    for (const auto& [client, payload] : payloads) {
        const ServerUpdatePacket update = read_server_update(payload);
        REQUIRE((client == 1 || client == 2));
        REQUIRE(update.entities.size() == 1);
    }

    REQUIRE(payloads.size() == 2);
}

TEST_CASE("replication server skips destroyed and externally unmarked entities") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ashiato::Entity destroyed = registry.create();
    const ashiato::Entity unmarked = registry.create();
    const ashiato::Entity live = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(destroyed, NetworkedPosition{}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(unmarked, NetworkedPosition{}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(live, NetworkedPosition{}) != nullptr);

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 512;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        payloads.push_back(payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, destroyed, archetype));
    REQUIRE(start_sync(registry, unmarked, archetype));
    REQUIRE(start_sync(registry, live, archetype));

    REQUIRE(registry.destroy(destroyed));
    REQUIRE(registry.remove<ashiato::sync::Replicated>(unmarked));

    server.tick(registry, server.options().fixed_dt_seconds);

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
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);

    int sends = 0;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 19;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer&) {
        ++sends;
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(sends == 0);
}

TEST_CASE("replication server parallel path releases quantized frames for unsent records") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_networked_position_archetype(registry);
    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> packets;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 19;
    options.serialized_worker_threads = 2;
    options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& payload) {
        packets.emplace_back(client, payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(packets.empty());
    REQUIRE(server.retained_quantized_frame_count() == 0U);
    REQUIRE(server.retained_quantized_frame_bytes() == 0U);
}

TEST_CASE("replication server serializes full and delta component payloads through transport") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& payload) {
        REQUIRE(client == 1);
        payloads.push_back(payload);
    };

    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry, server.options().fixed_dt_seconds);
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
    server.tick(registry, server.options().fixed_dt_seconds);

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

TEST_CASE("replication server rejects archetype replacement while syncing") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ashiato::Entity health_component = ashiato::sync::register_sync_component<Health>(registry, "Health");
    const ashiato::sync::SyncArchetypeId position_archetype = ashiato::sync::define_archetype(
        registry,
        "PositionActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    const ashiato::sync::SyncArchetypeId actor_archetype = ashiato::sync::define_archetype(
        registry,
        "Actor",
        {
            {position_component, ashiato::sync::ReplicationAudience::All},
            {health_component, ashiato::sync::ReplicationAudience::All},
        });
    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add<Health>(entity, Health{50}) != nullptr);

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, position_archetype));

    server.tick(registry, server.options().fixed_dt_seconds);
    ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].full);
    REQUIRE(update.entities[0].archetype == position_archetype);
    REQUIRE(server.acknowledge_entity(1, entity, update.frame));

    REQUIRE(start_sync(registry, entity, actor_archetype));
    REQUIRE_THROWS_AS(server.tick(registry, server.options().fixed_dt_seconds), std::logic_error);
}

TEST_CASE("replication server applies bandwidth limits to actual serialized byte counts") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    const ashiato::Entity first = registry.create();
    const ashiato::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{2.0f, 2.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(payloads.size() == 1);
    NetworkedPayload fields = read_first_networked_payload(payloads[0]);
    REQUIRE_FALSE(fields.delta);
    REQUIRE(fields.x == 10);
    REQUIRE(fields.y == 10);
    registry.write<NetworkedPosition>(second) = NetworkedPosition{7.0f, 8.0f};
    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(payloads.size() == 2);
    fields = read_first_networked_payload(payloads[1]);
    REQUIRE_FALSE(fields.delta);
    REQUIRE(fields.x == 70);
    REQUIRE(fields.y == 80);
}

TEST_CASE("replication server filters owner-only serialized components") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ashiato::Entity health_component = ashiato::sync::register_sync_component<Health>(registry, "Health");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "OwnedActor",
        {
            {position_component, ashiato::sync::ReplicationAudience::All},
            {health_component, ashiato::sync::ReplicationAudience::Owner},
        });
    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add<Health>(entity, Health{42}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(registry, entity, 2));

    std::vector<std::pair<ashiato::sync::ClientId, std::size_t>> sends;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& payload) {
        sends.push_back({client, payload.byte_size()});
    };

    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(sends.size() == 2);
    REQUIRE(sends[0] == std::pair<ashiato::sync::ClientId, std::size_t>{1, 20});
    REQUIRE(sends[1] == std::pair<ashiato::sync::ClientId, std::size_t>{2, 24});
}

TEST_CASE("replication server serializes compact synced tag masks per client") {
    ashiato::Registry registry;
    const ashiato::Entity visible = registry.register_component<Visible>("Visible");
    const ashiato::Entity secret = registry.register_component<Secret>("Secret");
    const ashiato::Entity position =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        ashiato::sync::SyncArchetypeDesc{
            "TaggedActor",
            {{visible, ashiato::sync::ReplicationAudience::All},
             {secret, ashiato::sync::ReplicationAudience::Owner}},
            {{position, ashiato::sync::ReplicationAudience::All}},
        });
    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add_tag(entity, visible));
    REQUIRE(registry.add_tag(entity, secret));
    REQUIRE(ashiato::sync::set_owner(registry, entity, 1));

    std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(payloads.size() == 2);
    for (auto sent : payloads) {
        ashiato::BitBuffer packet = sent.second;
        REQUIRE(static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits)) == ashiato::sync::protocol::server_update_message);
        const auto frame = static_cast<ashiato::sync::SyncFrame>(packet.read_bits(32U));
        packet.read_bits(ashiato::sync::protocol::server_packet_id_bits);
        packet.read_bits(32U);
        REQUIRE(static_cast<std::uint16_t>(packet.read_bits(16U)) == 1);
        REQUIRE_FALSE(packet.read_bool());
        std::uint32_t network_id = 0;
        REQUIRE(ashiato::sync::protocol::read_network_entity_id(packet, network_id));
        REQUIRE(network_id != 0);
        REQUIRE(packet.read_bool());
        REQUIRE(ashiato::sync::SyncArchetypeId{static_cast<std::uint32_t>(packet.read_bits(32U))} == archetype);
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
    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(payloads.size() == 2);
    for (auto sent : payloads) {
        ashiato::BitBuffer packet = sent.second;
        packet.read_bits(ashiato::sync::protocol::message_bits);
        const auto frame = static_cast<ashiato::sync::SyncFrame>(packet.read_bits(32U));
        packet.read_bits(ashiato::sync::protocol::server_packet_id_bits);
        packet.read_bits(32U);
        ashiato::sync::SyncFrame baseline_frame = 0;
        REQUIRE(static_cast<std::uint16_t>(packet.read_bits(16U)) == 1);
        REQUIRE_FALSE(packet.read_bool());
        std::uint32_t network_id = 0;
        REQUIRE(ashiato::sync::protocol::read_network_entity_id(packet, network_id));
        REQUIRE(network_id != 0);
        REQUIRE_FALSE(packet.read_bool());
        REQUIRE(ashiato::sync::protocol::read_baseline_frame(packet, frame, baseline_frame));
        REQUIRE(packet.read_bool());
        REQUIRE_FALSE(packet.read_bool());
        REQUIRE(packet.read_unsigned_bits(2U) == (sent.first == 1 ? 2U : 0U));
        REQUIRE_FALSE(packet.read_bool());
    }
}

TEST_CASE("replication server applies sphere priorities and component LOD masks") {
    ashiato::Registry registry;
    const ashiato::Entity health_component = ashiato::sync::register_sync_component<Health>(registry, "Health");
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "SphereFilteredActor",
        {{health_component, ashiato::sync::ReplicationAudience::All},
         {position_component, ashiato::sync::ReplicationAudience::All}});

    const ashiato::Entity near = registry.create();
    REQUIRE(registry.add<Health>(near, Health{75}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(near, NetworkedPosition{1.0f, 0.0f}) != nullptr);
    REQUIRE(start_sync(registry, near, archetype));

    const ashiato::Entity far = registry.create();
    REQUIRE(registry.add<Health>(far, Health{25}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(far, NetworkedPosition{10.0f, 0.0f}) != nullptr);
    REQUIRE(start_sync(registry, far, archetype));

    std::vector<ashiato::BitBuffer> payloads;
    std::size_t prioritizer_calls = 0;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](ashiato::sync::ClientId client, ashiato::sync::ReplicationPriorityObject object) {
        REQUIRE(client == 1);
        ++prioritizer_calls;
        const NetworkedPosition& position = registry.get<NetworkedPosition>(object.entity);
        ashiato::sync::ReplicationPriorityDecision decision;
        decision.priority = position.x <= 2.0f ? 100.0f : 0.0f;
        decision.component_mask = std::uint64_t{1} << 0U;
        return decision;
    };
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(prioritizer_calls == 2);
    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket update = read_server_update(payloads[0], 3U, sizeof(Health) * 8U);
    REQUIRE(update.entities.size() == 2);
    REQUIRE(update.entities[0].network_id != 0);
    REQUIRE(update.entities[0].components.size() == 1);
    REQUIRE(update.entities[0].components[0].component_index == 1);
}

TEST_CASE("replication server packs entity records up to the configured mtu") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    const ashiato::Entity first = registry.create();
    const ashiato::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{2.0f, 2.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.mtu_bytes = 56;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(payloads.size() == 1);
    REQUIRE(payloads[0].byte_size() == 29);
    const ServerUpdatePacket update = read_server_update(payloads[0]);
    REQUIRE(update.entities.size() == 2);
}

TEST_CASE("replication server splits packed updates at the mtu boundary") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    const ashiato::Entity first = registry.create();
    const ashiato::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{2.0f, 2.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.mtu_bytes = 21;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(payloads.size() == 2);
    for (const ashiato::BitBuffer& payload : payloads) {
        REQUIRE(payload.byte_size() <= options.mtu_bytes);
        REQUIRE(payload.byte_size() == 20);
        REQUIRE(read_server_update(payload).entities.size() == 1);
    }
}
