#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

TEST_CASE("replication client tick emits packets through configured sender") {
    ecs::Registry registry;
    kage::sync::configure_client(registry, 1);

    std::vector<ecs::BitBuffer> sent;
    kage::sync::ReplicationClient client;
    client.set_packet_sender([&](const ecs::BitBuffer& packet) {
        sent.push_back(packet);
    });

    REQUIRE(client.tick(registry, 0.0));
    REQUIRE_FALSE(sent.empty());
}

TEST_CASE("replication client queued receive packets are processed during tick") {
    ecs::Registry registry;
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClient client;
    ecs::BitBuffer response;
    response.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    response.push_bits(1U, 1U);
    response.push_unsigned_bits(1U, 64U);
    client.receive_packet(response);

    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds * 0.5));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Accepted);
    REQUIRE(client.continuous_receive_frame() == Catch::Approx(0.5));
}

TEST_CASE("replication client rejects malformed connect responses") {
    ecs::Registry registry;
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.connect_token = "token";
    kage::sync::ReplicationClient client(options);
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Connecting);

    ecs::BitBuffer truncated_accept;
    truncated_accept.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    truncated_accept.push_bool(true);
    truncated_accept.push_bits(1, 8U);
    REQUIRE_FALSE(client.receive(registry, truncated_accept));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Connecting);

    ecs::BitBuffer truncated_reject;
    truncated_reject.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    truncated_reject.push_bool(false);
    truncated_reject.push_bits(5, 16U);
    REQUIRE_FALSE(client.receive(registry, truncated_reject));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Connecting);

    ecs::BitBuffer invalid_client_id;
    invalid_client_id.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    invalid_client_id.push_bool(true);
    invalid_client_id.push_unsigned_bits(kage::sync::max_client_entity_network_id_client + 1U, 64U);
    REQUIRE_FALSE(client.receive(registry, invalid_client_id));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Connecting);
}

TEST_CASE("replication client stores rejected connect response errors") {
    ecs::Registry registry;
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.connect_token = "token";
    kage::sync::ReplicationClient client(options);

    ecs::BitBuffer rejected;
    rejected.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    rejected.push_bool(false);
    kage::sync::protocol::write_string(rejected, "bad token");

    REQUIRE(client.receive(registry, rejected));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Rejected);
    REQUIRE(client.connect_error() == "bad token");
}

TEST_CASE("replication client queued stale receive packets do not fail tick") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity server_entity{42};
    const ecs::BitBuffer packet =
        make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}});
    REQUIRE(client.receive(registry, packet, 1));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.receive(registry, packet, 1));

    client.receive_packet(packet);
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.pending_ack_count() == 0);
}

TEST_CASE("replication client delays server update packet loss until gaps leave the receive window") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(2, {{server_entity, Position{3.0f, 4.0f}}}, 2U, 3U),
        2));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 2);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 0);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client fills packet loss gaps from reordered server update packets") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity first_entity{42};
    const ecs::Entity second_entity{43};
    const ecs::Entity reordered_entity{44};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(3, {{second_entity, Position{3.0f, 4.0f}}}, 2U, 3U),
        3));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(4, {{reordered_entity, Position{2.0f, 3.0f}}}, 2U, 2U),
        4));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 3);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client counts packet loss when gaps leave the receive window") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(3, {{server_entity, Position{3.0f, 4.0f}}}, 2U, 3U),
        3));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(66, {{server_entity, Position{6.0f, 6.0f}}}, 2U, 66U),
        66));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 3);
    REQUIRE(stats.server_update_packets_missing == 1);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 0);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(1.0f / 4.0f));
}

TEST_CASE("replication client treats duplicate server update packet ids as duplicate without loss") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity first_entity{42};
    const ecs::Entity duplicate_packet_id_entity{43};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(2, {{duplicate_packet_id_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        2));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 2);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client fills packet loss gaps from reordered wrapped packet ids") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity first_entity{42};
    const ecs::Entity second_entity{43};
    const ecs::Entity reordered_entity{44};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 254U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(2, {{second_entity, Position{3.0f, 4.0f}}}, 2U, 1U),
        2));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(3, {{reordered_entity, Position{2.0f, 3.0f}}}, 2U, 255U),
        3));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 3);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client does not undo confirmed loss for packets older than the receive window") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity first_entity{42};
    const ecs::Entity second_entity{43};
    const ecs::Entity far_entity{44};
    const ecs::Entity late_entity{45};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(3, {{second_entity, Position{3.0f, 4.0f}}}, 2U, 3U),
        3));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(66, {{far_entity, Position{6.0f, 6.0f}}}, 2U, 66U),
        66));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(67, {{late_entity, Position{2.0f, 3.0f}}}, 2U, 2U),
        67));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 4);
    REQUIRE(stats.server_update_packets_missing == 1);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(1.0f / 5.0f));
}

TEST_CASE("client connect handshake ACKs accepted id until first update") {
    ecs::Registry client_registry;
    kage_sync_tests::define_position_archetype(client_registry);

    kage::sync::ReplicationClientOptions options;
    options.connect_token = "token";
    kage::sync::ReplicationClient client(options);

    std::vector<ecs::BitBuffer> packets = client.drain_packets();
    REQUIRE(packets.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(packets[0].read_bits(8U)) ==
            kage::sync::protocol::client_connect_request_message);
    std::string token;
    REQUIRE(kage::sync::protocol::read_string(packets[0], token));
    REQUIRE(token == "token");

    ecs::BitBuffer accepted;
    accepted.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    accepted.push_bool(true);
    accepted.push_unsigned_bits(7, 64U);
    REQUIRE(client.receive(client_registry, accepted));
    REQUIRE(client.client_id() == 7);
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Accepted);
    REQUIRE(client_registry.get<kage::sync::SyncSettings>().local_client == 7);

    packets = client.drain_packets();
    REQUIRE_FALSE(packets.empty());
    bool saw_connect_ack = false;
    for (ecs::BitBuffer packet : packets) {
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message == kage::sync::protocol::client_connect_ack_message) {
            saw_connect_ack = true;
            REQUIRE(packet.read_unsigned_bits(64U) == 7);
        }
    }
    REQUIRE(saw_connect_ack);

    const ecs::Entity server_entity{42};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Ready);

    REQUIRE(client.tick(client_registry, client.options().connect_resend_interval_seconds));
    packets = client.drain_packets();
    for (ecs::BitBuffer packet : packets) {
        REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) !=
                kage::sync::protocol::client_connect_ack_message);
    }
}
