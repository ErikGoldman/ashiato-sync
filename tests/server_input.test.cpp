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

TEST_CASE("replication server decodes client input, upserts owned entities, and ACKs frames") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(server_registry));

    const ashiato::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::Position>(owned, ashiato_sync_tests::Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    std::vector<ashiato::BitBuffer> updates;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        updates.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(client_registry));

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    REQUIRE(client.set_input(client_registry, NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    std::vector<ashiato::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != input_packets.end());

    REQUIRE(server.process_packet(server_registry, 1, *input_packet));
    server.tick(server_registry, server.options().fixed_dt_seconds);

    REQUIRE(server_registry.contains<NetworkedPosition>(owned));
    REQUIRE(server_registry.get<NetworkedPosition>(owned).x == 5.0f);
    REQUIRE(server_registry.get<NetworkedPosition>(owned).y == 6.0f);
    REQUIRE(updates.size() == 1);
    const ServerUpdatePacket update = read_server_update(updates[0]);
    REQUIRE(update.input_ack_frame == 1);
    ashiato::sync::ReplicationServer::ClientInputStats input_stats = server.input_stats(1);
    REQUIRE(input_stats.latest_received_input_frame == 1);
    REQUIRE(input_stats.latest_applied_input_frame == 1);
    REQUIRE(input_stats.input_frames_applied == 1);
    REQUIRE(input_stats.input_starvation_frames == 0);

    REQUIRE(client.receive(client_registry, updates[0]));
    std::vector<ashiato::BitBuffer> post_ack_packets = client.drain_packets();
    auto post_ack_packet = std::find_if(post_ack_packets.begin(), post_ack_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_ack_message;
    });
    REQUIRE(post_ack_packet != post_ack_packets.end());
    ashiato::BitBuffer ack_packet = *post_ack_packet;
    REQUIRE(static_cast<std::uint8_t>(ack_packet.read_bits(8U)) == ashiato::sync::protocol::client_ack_message);
    REQUIRE(static_cast<std::uint16_t>(ack_packet.read_bits(16U)) == 1);
    ack_packet.read_bits(ashiato::sync::protocol::server_packet_id_bits);
}

TEST_CASE("replication server applies local client input without packets") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(registry));

    ashiato::sync::ReplicationServer server(registry);
    const ashiato::sync::ClientId local = server.add_local_client(registry);
    REQUIRE(local != ashiato::sync::invalid_client_id);

    const ashiato::Entity owned = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(owned, NetworkedPosition{}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(registry, owned, local));
    REQUIRE(start_sync(registry, owned, archetype));

    REQUIRE(server.set_local_input(registry, NetworkedPosition{7.0f, 8.0f}));
    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(registry.get<NetworkedPosition>(owned).x == 7.0f);
    REQUIRE(registry.get<NetworkedPosition>(owned).y == 8.0f);
    const ashiato::sync::ReplicationServer::ClientInputStats stats = server.input_stats(local);
    REQUIRE(stats.latest_received_input_frame == 1);
    REQUIRE(stats.latest_applied_input_frame == 1);
    REQUIRE(stats.input_frames_applied == 1);
}

TEST_CASE("replication server phased tick replicates post-input simulation state") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(server_registry));
    server_registry.job<ashiato_sync_tests::Position, const NetworkedPosition>(0).each(
        [](ashiato::Entity, ashiato_sync_tests::Position& position, const NetworkedPosition& input) {
            position.x += input.x;
            if (input.y != position.y) {
                position.y = input.y;
            }
        });

    const ashiato::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::Position>(owned, ashiato_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    std::vector<ashiato::BitBuffer> updates;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        updates.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(client_registry));

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 1.0f}));
    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    std::vector<ashiato::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != input_packets.end());

    REQUIRE(server.process_packet(server_registry, 1, *input_packet));
    REQUIRE(server.tick(server_registry, server.options().fixed_dt_seconds));

    REQUIRE(server_registry.get<ashiato_sync_tests::Position>(owned).x == 1.0f);
    REQUIRE(server_registry.get<ashiato_sync_tests::Position>(owned).y == 1.0f);
    REQUIRE(updates.size() == 1);
    ashiato::BitBuffer update_header = updates[0];
    REQUIRE(static_cast<std::uint8_t>(update_header.read_bits(8U)) == ashiato::sync::protocol::server_update_message);
    update_header.read_bits(32U);
    update_header.read_bits(ashiato::sync::protocol::server_packet_id_bits);
    REQUIRE(static_cast<ashiato::sync::SyncFrame>(update_header.read_bits(32U)) == 1);
    REQUIRE(client.receive(client_registry, updates[0]));
    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.get<ashiato_sync_tests::Position>(local).x == 1.0f);
    REQUIRE(client_registry.get<ashiato_sync_tests::Position>(local).y == 1.0f);
}

TEST_CASE("replication server skips old explicit input frames and keeps useful full frames") {
    ashiato::Registry server_registry;
    ashiato_sync_tests::define_position_archetype(server_registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(server_registry));

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::client_input_message, 8U);
    packet.push_bits(0, 16U);
    packet.push_bits(2, 32U);
    packet.push_bits(4, 16U);
    packet.push_bool(true);
    packet.push_bits(1, 32U);
    packet.push_bool(false);
    packet.push_bits(10, 8U);
    packet.push_bits(10, 8U);
    for (int frame = 2; frame <= 4; ++frame) {
        packet.push_bool(true);
        packet.push_bits(1, 8U);
        packet.push_bits(1, 8U);
    }

    REQUIRE(server.process_packet(server_registry, 1, packet));
    REQUIRE(server.input_stats(1).latest_received_input_frame == 4);
}

TEST_CASE("replication server accepts input packets with stale piggybacked ACKs") {
    ashiato::Registry server_registry;
    ashiato_sync_tests::define_position_archetype(server_registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(server_registry));

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::client_input_message, 8U);
    packet.push_bits(1, 16U);
    packet.push_bits(42, ashiato::sync::protocol::server_packet_id_bits);
    packet.push_bits(2, 32U);
    packet.push_bits(4, 16U);
    packet.push_bool(true);
    packet.push_bits(1, 32U);
    packet.push_bool(false);
    packet.push_bits(10, 8U);
    packet.push_bits(10, 8U);
    for (int frame = 2; frame <= 4; ++frame) {
        packet.push_bool(true);
        packet.push_bits(1, 8U);
        packet.push_bits(1, 8U);
    }

    REQUIRE(server.process_packet(server_registry, 1, packet));
    REQUIRE(server.input_stats(1).latest_received_input_frame == 4);
}

TEST_CASE("replication server applies future client inputs only when their frame is due and counts starvation") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(server_registry));

    const ashiato::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::Position>(owned, ashiato_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(client_registry));

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    REQUIRE(client.set_input(client_registry, NetworkedPosition{5.0f, 6.0f}));
    for (int tick = 0; tick < 3; ++tick) {
        REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    }
    std::vector<ashiato::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != input_packets.end());
    REQUIRE(read_client_input_header(*input_packet).input_count == 3);
    REQUIRE(server.process_packet(server_registry, 1, *input_packet));

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 1);
    REQUIRE(server.input_stats(1).input_frames_applied == 1);
    REQUIRE(server.input_stats(1).input_starvation_frames == 0);

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 2);
    REQUIRE(server.input_stats(1).input_frames_applied == 2);
    REQUIRE(server.input_stats(1).input_starvation_frames == 0);

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 3);
    REQUIRE(server.input_stats(1).input_frames_applied == 3);
    REQUIRE(server.input_stats(1).input_starvation_frames == 0);

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 3);
    REQUIRE(server.input_stats(1).input_frames_applied == 3);
    REQUIRE(server.input_stats(1).input_starvation_frames == 1);
    REQUIRE(server.input_stats(1).input_reused_frames == 1);
}

TEST_CASE("replication server treats received client input frames as immutable") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(server_registry));

    const ashiato::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::Position>(owned, ashiato_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(client_registry));

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 2.0f}));
    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    std::vector<ashiato::BitBuffer> first_packets = client.drain_packets();
    auto first_input = std::find_if(first_packets.begin(), first_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(first_input != first_packets.end());
    REQUIRE(server.process_packet(server_registry, 1, *first_input));

    REQUIRE(client.set_input(client_registry, NetworkedPosition{9.0f, 10.0f}));
    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    std::vector<ashiato::BitBuffer> refreshed_packets = client.drain_packets();
    auto refreshed_input = std::find_if(refreshed_packets.begin(), refreshed_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(refreshed_input != refreshed_packets.end());
    REQUIRE(server.process_packet(server_registry, 1, *refreshed_input));

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(server_registry.contains<NetworkedPosition>(owned));
    REQUIRE(server_registry.get<NetworkedPosition>(owned).x == 1.0f);
    REQUIRE(server_registry.get<NetworkedPosition>(owned).y == 2.0f);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 1);

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(server_registry.get<NetworkedPosition>(owned).x == 9.0f);
    REQUIRE(server_registry.get<NetworkedPosition>(owned).y == 10.0f);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 2);
}

TEST_CASE("two token clients move owned players with fresh input frames every tick") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(server_registry));

    const ashiato::Entity first_owned = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::Position>(first_owned, ashiato_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, first_owned, 1));
    REQUIRE(start_sync(server_registry, first_owned, server_archetype));

    const ashiato::Entity second_owned = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::Position>(second_owned, ashiato_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, second_owned, 2));
    REQUIRE(start_sync(server_registry, second_owned, server_archetype));

    std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> server_packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId peer, const ashiato::BitBuffer& packet) {
        server_packets.push_back({peer, packet});
    };
    server_options.connect_handler = [](const std::string& token, ashiato::sync::ClientId&, std::string& error) {
        if (token != "token") {
            error = "bad token";
            return false;
        }
        return true;
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);

    auto make_client_registry = [&]() {
        ashiato::Registry registry;
        const ashiato::sync::SyncArchetypeId client_archetype = define_position_archetype(registry);
        REQUIRE(client_archetype == server_archetype);
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
        REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(registry));
        return registry;
    };

    ashiato::Registry first_client_registry = make_client_registry();
    ashiato::Registry second_client_registry = make_client_registry();
    ashiato::sync::ReplicationClientOptions client_options;
    client_options.session.connect_token = "token";
    ashiato::sync::ReplicationClient first_client(first_client_registry, ashiato_sync_tests::make_test_client_options(first_client_registry, client_options));
    ashiato::sync::ReplicationClient second_client(second_client_registry, ashiato_sync_tests::make_test_client_options(second_client_registry, client_options));

    constexpr ashiato::sync::ClientId first_peer = 101;
    constexpr ashiato::sync::ClientId second_peer = 202;
    const NetworkedPosition first_target{10.0f, 0.0f};
    const NetworkedPosition second_target{-10.0f, 0.0f};
    REQUIRE(first_client.set_input(first_client_registry, first_target));
    REQUIRE(second_client.set_input(second_client_registry, second_target));

    auto drain_client_to_server = [&](ashiato::sync::ClientId peer, ashiato::sync::ReplicationClient& client) {
        for (const ashiato::BitBuffer& packet : client.drain_packets()) {
            REQUIRE(server.process_packet(server_registry, peer, packet));
        }
    };
    auto deliver_server_packets = [&](ashiato::sync::ClientId peer, ashiato::sync::ReplicationClient& client, ashiato::Registry& registry) {
        std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> remaining;
        for (const auto& sent : server_packets) {
            if (sent.first == peer) {
                REQUIRE(client.receive(registry, sent.second));
            } else {
                remaining.push_back(sent);
            }
        }
        server_packets = std::move(remaining);
    };
    auto has_input_packet = [](const std::vector<ashiato::BitBuffer>& packets) {
        return std::any_of(packets.begin(), packets.end(), [](ashiato::BitBuffer packet) {
            return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
        });
    };

    drain_client_to_server(first_peer, first_client);
    drain_client_to_server(second_peer, second_client);
    deliver_server_packets(first_peer, first_client, first_client_registry);
    deliver_server_packets(second_peer, second_client, second_client_registry);
    REQUIRE(first_client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Accepted);
    REQUIRE(second_client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Accepted);

    drain_client_to_server(first_peer, first_client);
    drain_client_to_server(second_peer, second_client);
    server.tick(server_registry, server.options().fixed_dt_seconds);
    deliver_server_packets(first_peer, first_client, first_client_registry);
    deliver_server_packets(second_peer, second_client, second_client_registry);
    REQUIRE(first_client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Ready);
    REQUIRE(second_client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Ready);

    std::vector<ashiato::BitBuffer> first_bootstrap_packets = first_client.drain_packets();
    std::vector<ashiato::BitBuffer> second_bootstrap_packets = second_client.drain_packets();
    REQUIRE(has_input_packet(first_bootstrap_packets));
    REQUIRE(has_input_packet(second_bootstrap_packets));
    for (const ashiato::BitBuffer& packet : first_bootstrap_packets) {
        REQUIRE(server.process_packet(server_registry, first_peer, packet));
    }
    for (const ashiato::BitBuffer& packet : second_bootstrap_packets) {
        REQUIRE(server.process_packet(server_registry, second_peer, packet));
    }

    std::array<ashiato::sync::SyncFrame, 2> last_applied_input{};
    std::array<int, 2> fresh_inputs_applied{};
    std::array<int, 2> reached_after_inputs{};
    constexpr int expected_inputs_to_target = 10;

    auto apply_movement_for_fresh_input = [&](
        std::size_t index,
        ashiato::sync::ClientId client,
        ashiato::Entity entity,
        float target_x) {
        const ashiato::sync::ReplicationServer::ClientInputStats stats = server.input_stats(client);
        if (stats.latest_applied_input_frame == last_applied_input[index]) {
            return;
        }
        REQUIRE(stats.latest_applied_input_frame > last_applied_input[index]);
        last_applied_input[index] = stats.latest_applied_input_frame;
        ++fresh_inputs_applied[index];

        REQUIRE(server_registry.contains<NetworkedPosition>(entity));
        ashiato_sync_tests::Position& position = server_registry.write<ashiato_sync_tests::Position>(entity);
        if (position.x < target_x) {
            position.x = std::min(position.x + 1.0f, target_x);
        } else if (position.x > target_x) {
            position.x = std::max(position.x - 1.0f, target_x);
        }

        if (fresh_inputs_applied[index] < expected_inputs_to_target) {
            REQUIRE(position.x != target_x);
        } else if (fresh_inputs_applied[index] == expected_inputs_to_target) {
            REQUIRE(position.x == target_x);
            reached_after_inputs[index] = fresh_inputs_applied[index];
        }
    };

    for (int tick = 0; tick < 30; ++tick) {
        REQUIRE(first_client.set_input(first_client_registry, first_target));
        REQUIRE(second_client.set_input(second_client_registry, second_target));
        REQUIRE(first_client.tick(first_client_registry, first_client.fixed_dt_seconds()));
        REQUIRE(second_client.tick(second_client_registry, second_client.fixed_dt_seconds()));
        drain_client_to_server(first_peer, first_client);
        drain_client_to_server(second_peer, second_client);

        server.tick(server_registry, server.options().fixed_dt_seconds);
        apply_movement_for_fresh_input(0, 1, first_owned, first_target.x);
        apply_movement_for_fresh_input(1, 2, second_owned, second_target.x);
        deliver_server_packets(first_peer, first_client, first_client_registry);
        deliver_server_packets(second_peer, second_client, second_client_registry);

        if (reached_after_inputs[0] != 0 && reached_after_inputs[1] != 0) {
            break;
        }
    }

    REQUIRE(reached_after_inputs[0] == expected_inputs_to_target);
    REQUIRE(reached_after_inputs[1] == expected_inputs_to_target);
    REQUIRE(fresh_inputs_applied[0] == expected_inputs_to_target);
    REQUIRE(fresh_inputs_applied[1] == expected_inputs_to_target);
    REQUIRE(server.input_stats(1).input_frames_applied >= static_cast<std::uint64_t>(expected_inputs_to_target));
    REQUIRE(server.input_stats(2).input_frames_applied >= static_cast<std::uint64_t>(expected_inputs_to_target));
    REQUIRE(server_registry.get<ashiato_sync_tests::Position>(first_owned).x == 10.0f);
    REQUIRE(server_registry.get<ashiato_sync_tests::Position>(second_owned).x == -10.0f);
}

TEST_CASE("client input packets reserve room for input when server ack backlog is large") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(server_registry));

    const ashiato::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::Position>(owned, ashiato_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> server_packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId peer, const ashiato::BitBuffer& packet) {
        server_packets.push_back({peer, packet});
    };
    server_options.connect_handler = [](const std::string& token, ashiato::sync::ClientId&, std::string& error) {
        if (token != "token") {
            error = "bad token";
            return false;
        }
        return true;
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(client_registry));

    ashiato::sync::ReplicationClientOptions client_options;
    client_options.session.connect_token = "token";
    client_options.network.mtu_bytes = 18;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, client_options));

    constexpr ashiato::sync::ClientId peer = 101;
    const NetworkedPosition input{5.0f, 0.0f};
    REQUIRE(client.set_input(client_registry, input));

    auto drain_client_to_server = [&]() {
        for (const ashiato::BitBuffer& packet : client.drain_packets()) {
            REQUIRE(server.process_packet(server_registry, peer, packet));
        }
    };
    auto deliver_server_packets = [&]() {
        std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> remaining;
        for (const auto& sent : server_packets) {
            if (sent.first == peer) {
                REQUIRE(client.receive(client_registry, sent.second));
            } else {
                remaining.push_back(sent);
            }
        }
        server_packets = std::move(remaining);
    };

    drain_client_to_server();
    deliver_server_packets();
    drain_client_to_server();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    deliver_server_packets();
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Ready);

    for (int tick = 0; tick < 40; ++tick) {
        server_registry.write<ashiato_sync_tests::Position>(owned).x = static_cast<float>(tick + 1);
        server.tick(server_registry, server.options().fixed_dt_seconds);
        deliver_server_packets();
    }

    std::vector<ashiato::BitBuffer> packets;
    for (int tick = 0; tick < 48; ++tick) {
        REQUIRE(client.set_input(client_registry, input));
        REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    }
    packets = client.drain_packets();
    const auto input_packet = std::find_if(packets.begin(), packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());

    const ClientInputPacket header = read_client_input_header(*input_packet);
    REQUIRE(header.ack_count > 0);
    REQUIRE(header.input_count > 0);
    for (const ashiato::BitBuffer& packet : packets) {
        REQUIRE(server.process_packet(server_registry, peer, packet));
    }
}

TEST_CASE("client fills input frame gap when prediction starts from a later server frame") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_predicted_archetype(server_registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(server_registry));

    std::vector<ashiato::BitBuffer> updates;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        updates.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_predicted_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(client_registry));

    ashiato::sync::ReplicationClientOptions client_options;
    client_options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, client_options));
    REQUIRE(client.set_input(client_registry, NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    std::vector<ashiato::BitBuffer> initial_packets = client.drain_packets();
    auto initial_input = std::find_if(initial_packets.begin(), initial_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(initial_input != initial_packets.end());
    REQUIRE(read_client_input_header(*initial_input).input_count == 2);
    REQUIRE(server.process_packet(server_registry, 1, *initial_input));

    for (int tick = 0; tick < 33; ++tick) {
        server.tick(server_registry, server.options().fixed_dt_seconds);
    }
    const ashiato::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<PredictedPosition>(owned, PredictedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE_FALSE(updates.empty());
    REQUIRE(client.receive(client_registry, updates.back()));

    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    std::vector<ashiato::BitBuffer> resumed_packets = client.drain_packets();
    auto resumed_input = std::find_if(resumed_packets.begin(), resumed_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(resumed_input != resumed_packets.end());
    const ClientInputPacket resumed_header = read_client_input_header(*resumed_input);
    REQUIRE(resumed_header.baseline_frame >= 2);
    REQUIRE(resumed_header.input_count > 0);
}

TEST_CASE("auto timing initializes prediction and buffered playback quickly under realistic latency") {
    using TestLink = ashiato::sync::SimulatedLink<ashiato::BitBuffer, ashiato::sync::ClientId>;

    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(server_registry));

    double now = 0.0;
    TestLink downstream({100.0, 0.0, 0.0}, 11U);
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.fixed_dt_seconds = 1.0 / 60.0;
    server_options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& packet) {
        (void)downstream.enqueue(client, packet, now);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    const ashiato::Entity replicated = server_registry.create();
    REQUIRE(server_registry.add<Position>(replicated, Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, replicated, 1));
    REQUIRE(start_sync(server_registry, replicated, server_archetype));

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(client_registry));

    ashiato::sync::ReplicationClientOptions client_options;
    client_options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    client_options.prediction.input_buffer_capacity_frames = 64;
    client_options.clock.auto_timing_warmup_samples = 3;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, client_options));

    TestLink upstream({100.0, 0.0, 0.0}, 22U);
    int first_update_tick = -1;
    int recovered_tick = -1;
    int stable_ticks = 0;
    std::uint64_t previous_starvations = 0;
    constexpr double dt = 1.0 / 60.0;

    for (int tick = 0; tick < 90; ++tick) {
        REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 0.0f}));
        REQUIRE(client.tick(client_registry, dt));
        for (const ashiato::BitBuffer& packet : client.drain_packets()) {
            (void)upstream.enqueue(1, packet, now);
        }
        upstream.deliver_ready(now, [&](ashiato::sync::ClientId peer, const ashiato::BitBuffer& packet) {
            REQUIRE(server.process_packet(server_registry, peer, packet));
        });

        server_registry.write<Position>(replicated).x += 1.0f;
        server.tick(server_registry, server.options().fixed_dt_seconds);

        downstream.deliver_ready(now, [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
            ashiato::BitBuffer copy = packet;
            const auto message = static_cast<std::uint8_t>(copy.read_bits(8U));
            if (message == ashiato::sync::protocol::server_update_message && first_update_tick < 0) {
                first_update_tick = tick;
            }
            (void)client.receive(client_registry, packet);
        });

        const ashiato::sync::ReplicationClientTimingStats timing = client.timing_stats();
        const ashiato::sync::ReplicationServer::ClientInputStats input_stats = server.input_stats(1);
        const bool prediction_ready =
            timing.current_prediction_lead_frames + 1U >= timing.target_prediction_lead_frames;
        const bool buffered_ready =
            timing.current_buffered_frame_lag + 1U >= timing.target_buffered_frame_lag &&
            timing.target_buffered_frame_lag + 1U >= timing.current_buffered_frame_lag;
        if (first_update_tick >= 0 &&
            input_stats.input_starvation_frames == previous_starvations &&
            prediction_ready &&
            buffered_ready) {
            ++stable_ticks;
            if (stable_ticks >= 3) {
                recovered_tick = tick;
                break;
            }
        } else {
            stable_ticks = 0;
        }
        previous_starvations = input_stats.input_starvation_frames;
        now += dt;
    }

    INFO("first_update_tick=" << first_update_tick);
    INFO("recovered_tick=" << recovered_tick);
    INFO("starvations=" << server.input_stats(1).input_starvation_frames);
    INFO("prediction current=" << client.timing_stats().current_prediction_lead_frames);
    INFO("prediction target=" << client.timing_stats().target_prediction_lead_frames);
    INFO("buffered lag current=" << client.timing_stats().current_buffered_frame_lag);
    INFO("buffered lag target=" << client.timing_stats().target_buffered_frame_lag);
    REQUIRE(first_update_tick >= 0);
    REQUIRE(recovered_tick >= 0);
    REQUIRE(client.timing_stats().current_prediction_lead_frames == client.timing_stats().target_prediction_lead_frames);
    REQUIRE(recovered_tick - first_update_tick <= 30);
}

TEST_CASE("auto timing recovers prediction and buffered playback quickly after packet loss burst") {
    using TestLink = ashiato::sync::SimulatedLink<ashiato::BitBuffer, ashiato::sync::ClientId>;

    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(server_registry));

    double now = 0.0;
    TestLink downstream({50.0, 0.0, 0.0}, 33U);
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.fixed_dt_seconds = 1.0 / 60.0;
    server_options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& packet) {
        (void)downstream.enqueue(client, packet, now);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    const ashiato::Entity replicated = server_registry.create();
    REQUIRE(server_registry.add<Position>(replicated, Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, replicated, 1));
    REQUIRE(start_sync(server_registry, replicated, server_archetype));

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(client_registry));

    ashiato::sync::ReplicationClientOptions client_options;
    client_options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    client_options.prediction.input_buffer_capacity_frames = 64;
    client_options.clock.auto_timing_warmup_samples = 1;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, client_options));

    TestLink upstream({50.0, 0.0, 0.0}, 44U);
    int first_post_loss_update_tick = -1;
    int recovered_tick = -1;
    int stable_ticks = 0;
    std::uint64_t previous_starvations = 0;
    constexpr int loss_begin_tick = 90;
    constexpr int loss_end_tick = 130;
    constexpr double dt = 1.0 / 60.0;

    for (int tick = 0; tick < 210; ++tick) {
        if (tick == loss_begin_tick) {
            upstream.settings.latency_ms = 300.0;
            downstream.settings.latency_ms = 300.0;
            upstream.settings.loss_percent = 100.0;
            downstream.settings.loss_percent = 100.0;
        } else if (tick == loss_end_tick) {
            upstream.settings.latency_ms = 50.0;
            downstream.settings.latency_ms = 50.0;
            upstream.settings.loss_percent = 0.0;
            downstream.settings.loss_percent = 0.0;
        }

        REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 0.0f}));
        REQUIRE(client.tick(client_registry, dt));
        for (const ashiato::BitBuffer& packet : client.drain_packets()) {
            (void)upstream.enqueue(1, packet, now);
        }
        upstream.deliver_ready(now, [&](ashiato::sync::ClientId peer, const ashiato::BitBuffer& packet) {
            REQUIRE(server.process_packet(server_registry, peer, packet));
        });

        server_registry.write<Position>(replicated).x += 1.0f;
        server.tick(server_registry, server.options().fixed_dt_seconds);

        downstream.deliver_ready(now, [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
            ashiato::BitBuffer copy = packet;
            const auto message = static_cast<std::uint8_t>(copy.read_bits(8U));
            if (message == ashiato::sync::protocol::server_update_message &&
                tick >= loss_end_tick &&
                first_post_loss_update_tick < 0) {
                first_post_loss_update_tick = tick;
            }
            (void)client.receive(client_registry, packet);
        });

        const ashiato::sync::ReplicationClientTimingStats timing = client.timing_stats();
        const ashiato::sync::ReplicationServer::ClientInputStats input_stats = server.input_stats(1);
        const bool prediction_ready =
            timing.current_prediction_lead_frames + 1U >= timing.target_prediction_lead_frames;
        const bool buffered_ready =
            timing.current_buffered_frame_lag + 1U >= timing.target_buffered_frame_lag &&
            timing.target_buffered_frame_lag + 1U >= timing.current_buffered_frame_lag;
        if (first_post_loss_update_tick >= 0 &&
            input_stats.input_starvation_frames == previous_starvations &&
            prediction_ready &&
            buffered_ready) {
            ++stable_ticks;
            if (stable_ticks >= 3) {
                recovered_tick = tick;
                break;
            }
        } else if (first_post_loss_update_tick >= 0) {
            stable_ticks = 0;
        }
        previous_starvations = input_stats.input_starvation_frames;
        now += dt;
    }

    INFO("first_post_loss_update_tick=" << first_post_loss_update_tick);
    INFO("recovered_tick=" << recovered_tick);
    INFO("starvations=" << server.input_stats(1).input_starvation_frames);
    INFO("prediction current=" << client.timing_stats().current_prediction_lead_frames);
    INFO("prediction target=" << client.timing_stats().target_prediction_lead_frames);
    INFO("buffered lag current=" << client.timing_stats().current_buffered_frame_lag);
    INFO("buffered lag target=" << client.timing_stats().target_buffered_frame_lag);
    REQUIRE(first_post_loss_update_tick >= 0);
    REQUIRE(recovered_tick >= 0);
    REQUIRE(recovered_tick - first_post_loss_update_tick <= 30);
}
