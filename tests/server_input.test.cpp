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

TEST_CASE("replication server decodes client input, upserts owned entities, and ACKs frames") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(server_registry));

    const ecs::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(owned, kage_sync_tests::Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    std::vector<ecs::BitBuffer> updates;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        updates.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClient client;
    REQUIRE(client.set_input(client_registry, NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    std::vector<ecs::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != input_packets.end());

    REQUIRE(server.process_packet(server_registry, 1, *input_packet));
    server.tick(server_registry);

    REQUIRE(server_registry.contains<NetworkedPosition>(owned));
    REQUIRE(server_registry.get<NetworkedPosition>(owned).x == 5.0f);
    REQUIRE(server_registry.get<NetworkedPosition>(owned).y == 6.0f);
    REQUIRE(updates.size() == 1);
    const ServerUpdatePacket update = read_server_update(updates[0]);
    REQUIRE(update.input_ack_frame == 1);
    kage::sync::ReplicationServer::ClientInputStats input_stats = server.input_stats(1);
    REQUIRE(input_stats.latest_received_input_frame == 1);
    REQUIRE(input_stats.latest_applied_input_frame == 1);
    REQUIRE(input_stats.input_frames_applied == 1);
    REQUIRE(input_stats.input_starvation_frames == 0);

    REQUIRE(client.receive(client_registry, updates[0]));
    std::vector<ecs::BitBuffer> post_ack_packets = client.drain_packets();
    auto post_ack_packet = std::find_if(post_ack_packets.begin(), post_ack_packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_ack_message;
    });
    REQUIRE(post_ack_packet != post_ack_packets.end());
    ecs::BitBuffer ack_packet = *post_ack_packet;
    REQUIRE(static_cast<std::uint8_t>(ack_packet.read_bits(8U)) == kage::sync::protocol::client_ack_message);
    REQUIRE(static_cast<std::uint16_t>(ack_packet.read_bits(16U)) == 1);
    ack_packet.read_bits(kage::sync::protocol::server_packet_id_bits);
}

TEST_CASE("replication server phased tick replicates post-input simulation state") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(server_registry));
    server_registry.job<kage_sync_tests::Position, const NetworkedPosition>(0).each(
        [](ecs::Entity, kage_sync_tests::Position& position, const NetworkedPosition& input) {
            position.x += input.x;
            if (input.y != position.y) {
                position.y = input.y;
            }
        });

    const ecs::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(owned, kage_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    std::vector<ecs::BitBuffer> updates;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        updates.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClient client;
    REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 1.0f}));
    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    std::vector<ecs::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != input_packets.end());

    REQUIRE(server.process_packet(server_registry, 1, *input_packet));
    server.begin_tick(server_registry);
    server_registry.run_jobs();
    server.end_tick(server_registry);

    REQUIRE(server_registry.get<kage_sync_tests::Position>(owned).x == 1.0f);
    REQUIRE(server_registry.get<kage_sync_tests::Position>(owned).y == 1.0f);
    REQUIRE(updates.size() == 1);
    ecs::BitBuffer update_header = updates[0];
    REQUIRE(static_cast<std::uint8_t>(update_header.read_bits(8U)) == kage::sync::protocol::server_update_message);
    update_header.read_bits(32U);
    update_header.read_bits(kage::sync::protocol::server_packet_id_bits);
    REQUIRE(static_cast<kage::sync::SyncFrame>(update_header.read_bits(32U)) == 1);
    REQUIRE(client.receive(client_registry, updates[0]));
    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.get<kage_sync_tests::Position>(local).x == 1.0f);
    REQUIRE(client_registry.get<kage_sync_tests::Position>(local).y == 1.0f);
}

TEST_CASE("replication server skips old explicit input frames and keeps useful full frames") {
    ecs::Registry server_registry;
    kage_sync_tests::define_position_archetype(server_registry);
    kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(server_registry));

    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));

    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_input_message, 8U);
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

TEST_CASE("replication server applies future client inputs only when their frame is due and counts starvation") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(server_registry));

    const ecs::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(owned, kage_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClient client;
    REQUIRE(client.set_input(client_registry, NetworkedPosition{5.0f, 6.0f}));
    for (int tick = 0; tick < 3; ++tick) {
        REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    }
    std::vector<ecs::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != input_packets.end());
    REQUIRE(read_client_input_header(*input_packet).input_count == 3);
    REQUIRE(server.process_packet(server_registry, 1, *input_packet));

    server.tick(server_registry);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 1);
    REQUIRE(server.input_stats(1).input_frames_applied == 1);
    REQUIRE(server.input_stats(1).input_starvation_frames == 0);

    server.tick(server_registry);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 2);
    REQUIRE(server.input_stats(1).input_frames_applied == 2);
    REQUIRE(server.input_stats(1).input_starvation_frames == 0);

    server.tick(server_registry);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 3);
    REQUIRE(server.input_stats(1).input_frames_applied == 3);
    REQUIRE(server.input_stats(1).input_starvation_frames == 0);

    server.tick(server_registry);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 3);
    REQUIRE(server.input_stats(1).input_frames_applied == 3);
    REQUIRE(server.input_stats(1).input_starvation_frames == 1);
    REQUIRE(server.input_stats(1).input_reused_frames == 1);
}

TEST_CASE("replication server treats received client input frames as immutable") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(server_registry));

    const ecs::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(owned, kage_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClient client;
    REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 2.0f}));
    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    std::vector<ecs::BitBuffer> first_packets = client.drain_packets();
    auto first_input = std::find_if(first_packets.begin(), first_packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(first_input != first_packets.end());
    REQUIRE(server.process_packet(server_registry, 1, *first_input));

    REQUIRE(client.set_input(client_registry, NetworkedPosition{9.0f, 10.0f}));
    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    std::vector<ecs::BitBuffer> refreshed_packets = client.drain_packets();
    auto refreshed_input = std::find_if(refreshed_packets.begin(), refreshed_packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(refreshed_input != refreshed_packets.end());
    REQUIRE(server.process_packet(server_registry, 1, *refreshed_input));

    server.tick(server_registry);
    REQUIRE(server_registry.contains<NetworkedPosition>(owned));
    REQUIRE(server_registry.get<NetworkedPosition>(owned).x == 1.0f);
    REQUIRE(server_registry.get<NetworkedPosition>(owned).y == 2.0f);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 1);

    server.tick(server_registry);
    REQUIRE(server_registry.get<NetworkedPosition>(owned).x == 9.0f);
    REQUIRE(server_registry.get<NetworkedPosition>(owned).y == 10.0f);
    REQUIRE(server.input_stats(1).latest_applied_input_frame == 2);
}

TEST_CASE("two token clients move owned players with fresh input frames every tick") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(server_registry));

    const ecs::Entity first_owned = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(first_owned, kage_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, first_owned, 1));
    REQUIRE(start_sync(server_registry, first_owned, server_archetype));

    const ecs::Entity second_owned = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(second_owned, kage_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, second_owned, 2));
    REQUIRE(start_sync(server_registry, second_owned, server_archetype));

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> server_packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
        server_packets.push_back({peer, packet});
    };
    server_options.connect_handler = [](const std::string& token, kage::sync::ClientId&, std::string& error) {
        if (token != "token") {
            error = "bad token";
            return false;
        }
        return true;
    };
    kage::sync::ReplicationServer server(server_options);

    auto make_client_registry = [&]() {
        ecs::Registry registry;
        const kage::sync::SyncArchetypeId client_archetype = define_position_archetype(registry);
        REQUIRE(client_archetype == server_archetype);
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
        REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(registry));
        return registry;
    };

    ecs::Registry first_client_registry = make_client_registry();
    ecs::Registry second_client_registry = make_client_registry();
    kage::sync::ReplicationClientOptions client_options;
    client_options.connect_token = "token";
    kage::sync::ReplicationClient first_client(client_options);
    kage::sync::ReplicationClient second_client(client_options);

    constexpr kage::sync::ClientId first_peer = 101;
    constexpr kage::sync::ClientId second_peer = 202;
    const NetworkedPosition first_target{10.0f, 0.0f};
    const NetworkedPosition second_target{-10.0f, 0.0f};
    REQUIRE(first_client.set_input(first_client_registry, first_target));
    REQUIRE(second_client.set_input(second_client_registry, second_target));

    auto drain_client_to_server = [&](kage::sync::ClientId peer, kage::sync::ReplicationClient& client) {
        for (const ecs::BitBuffer& packet : client.drain_packets()) {
            REQUIRE(server.process_packet(server_registry, peer, packet));
        }
    };
    auto deliver_server_packets = [&](kage::sync::ClientId peer, kage::sync::ReplicationClient& client, ecs::Registry& registry) {
        std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> remaining;
        for (const auto& sent : server_packets) {
            if (sent.first == peer) {
                REQUIRE(client.receive(registry, sent.second));
            } else {
                remaining.push_back(sent);
            }
        }
        server_packets = std::move(remaining);
    };
    auto has_input_packet = [](const std::vector<ecs::BitBuffer>& packets) {
        return std::any_of(packets.begin(), packets.end(), [](ecs::BitBuffer packet) {
            return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
        });
    };

    drain_client_to_server(first_peer, first_client);
    drain_client_to_server(second_peer, second_client);
    deliver_server_packets(first_peer, first_client, first_client_registry);
    deliver_server_packets(second_peer, second_client, second_client_registry);
    REQUIRE(first_client.connection_state() == kage::sync::ReplicationClientConnectionState::Accepted);
    REQUIRE(second_client.connection_state() == kage::sync::ReplicationClientConnectionState::Accepted);

    drain_client_to_server(first_peer, first_client);
    drain_client_to_server(second_peer, second_client);
    server.tick(server_registry);
    deliver_server_packets(first_peer, first_client, first_client_registry);
    deliver_server_packets(second_peer, second_client, second_client_registry);
    REQUIRE(first_client.connection_state() == kage::sync::ReplicationClientConnectionState::Ready);
    REQUIRE(second_client.connection_state() == kage::sync::ReplicationClientConnectionState::Ready);

    std::vector<ecs::BitBuffer> first_bootstrap_packets = first_client.drain_packets();
    std::vector<ecs::BitBuffer> second_bootstrap_packets = second_client.drain_packets();
    REQUIRE(has_input_packet(first_bootstrap_packets));
    REQUIRE(has_input_packet(second_bootstrap_packets));
    for (const ecs::BitBuffer& packet : first_bootstrap_packets) {
        REQUIRE(server.process_packet(server_registry, first_peer, packet));
    }
    for (const ecs::BitBuffer& packet : second_bootstrap_packets) {
        REQUIRE(server.process_packet(server_registry, second_peer, packet));
    }

    std::array<kage::sync::SyncFrame, 2> last_applied_input{};
    std::array<int, 2> fresh_inputs_applied{};
    std::array<int, 2> reached_after_inputs{};
    constexpr int expected_inputs_to_target = 10;

    auto apply_movement_for_fresh_input = [&](
        std::size_t index,
        kage::sync::ClientId client,
        ecs::Entity entity,
        float target_x) {
        const kage::sync::ReplicationServer::ClientInputStats stats = server.input_stats(client);
        if (stats.latest_applied_input_frame == last_applied_input[index]) {
            return;
        }
        REQUIRE(stats.latest_applied_input_frame > last_applied_input[index]);
        last_applied_input[index] = stats.latest_applied_input_frame;
        ++fresh_inputs_applied[index];

        REQUIRE(server_registry.contains<NetworkedPosition>(entity));
        kage_sync_tests::Position& position = server_registry.write<kage_sync_tests::Position>(entity);
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
        REQUIRE(first_client.tick(first_client_registry, first_client.options().fixed_dt_seconds));
        REQUIRE(second_client.tick(second_client_registry, second_client.options().fixed_dt_seconds));
        drain_client_to_server(first_peer, first_client);
        drain_client_to_server(second_peer, second_client);

        server.tick(server_registry);
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
    REQUIRE(server_registry.get<kage_sync_tests::Position>(first_owned).x == 10.0f);
    REQUIRE(server_registry.get<kage_sync_tests::Position>(second_owned).x == -10.0f);
}

TEST_CASE("client input packets reserve room for input when server ack backlog is large") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(server_registry));

    const ecs::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(owned, kage_sync_tests::Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> server_packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
        server_packets.push_back({peer, packet});
    };
    server_options.connect_handler = [](const std::string& token, kage::sync::ClientId&, std::string& error) {
        if (token != "token") {
            error = "bad token";
            return false;
        }
        return true;
    };
    kage::sync::ReplicationServer server(server_options);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions client_options;
    client_options.connect_token = "token";
    client_options.mtu_bytes = 16;
    kage::sync::ReplicationClient client(client_options);

    constexpr kage::sync::ClientId peer = 101;
    const NetworkedPosition input{5.0f, 0.0f};
    REQUIRE(client.set_input(client_registry, input));

    auto drain_client_to_server = [&]() {
        for (const ecs::BitBuffer& packet : client.drain_packets()) {
            REQUIRE(server.process_packet(server_registry, peer, packet));
        }
    };
    auto deliver_server_packets = [&]() {
        std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> remaining;
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
    server.tick(server_registry);
    deliver_server_packets();
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Ready);

    for (int tick = 0; tick < 40; ++tick) {
        server_registry.write<kage_sync_tests::Position>(owned).x = static_cast<float>(tick + 1);
        server.tick(server_registry);
        deliver_server_packets();
    }

    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    std::vector<ecs::BitBuffer> packets = client.drain_packets();
    const auto input_packet = std::find_if(packets.begin(), packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());

    const ClientInputPacket header = read_client_input_header(*input_packet);
    REQUIRE(header.ack_count > 0);
    REQUIRE(header.input_count > 0);
    for (const ecs::BitBuffer& packet : packets) {
        REQUIRE(server.process_packet(server_registry, peer, packet));
    }
    REQUIRE(server.input_stats(1).latest_received_input_frame > 0);
}

TEST_CASE("client fills input frame gap when prediction starts from a later server frame") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_predicted_archetype(server_registry);
    kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(server_registry));

    std::vector<ecs::BitBuffer> updates;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        updates.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_predicted_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions client_options;
    client_options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(client_options);
    REQUIRE(client.set_input(client_registry, NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    std::vector<ecs::BitBuffer> initial_packets = client.drain_packets();
    auto initial_input = std::find_if(initial_packets.begin(), initial_packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(initial_input != initial_packets.end());
    REQUIRE(read_client_input_header(*initial_input).input_count == 2);
    REQUIRE(server.process_packet(server_registry, 1, *initial_input));

    for (int tick = 0; tick < 33; ++tick) {
        server.tick(server_registry);
    }
    const ecs::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<PredictedPosition>(owned, PredictedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));
    server.tick(server_registry);
    REQUIRE_FALSE(updates.empty());
    REQUIRE(client.receive(client_registry, updates.back()));

    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    std::vector<ecs::BitBuffer> resumed_packets = client.drain_packets();
    auto resumed_input = std::find_if(resumed_packets.begin(), resumed_packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(resumed_input != resumed_packets.end());
    const ClientInputPacket resumed_header = read_client_input_header(*resumed_input);
    REQUIRE(resumed_header.baseline_frame == 2);
    REQUIRE(resumed_header.input_count >= 33);
}

TEST_CASE("auto timing initializes prediction and interpolation quickly under realistic latency") {
    using TestLink = kage::sync::SimulatedLink<ecs::BitBuffer, kage::sync::ClientId>;

    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(server_registry));

    double now = 0.0;
    TestLink downstream({100.0, 0.0, 0.0}, 11U);
    kage::sync::ReplicationServerOptions server_options;
    server_options.fixed_dt_seconds = 1.0 / 60.0;
    server_options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& packet) {
        (void)downstream.enqueue(client, packet, now);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));

    const ecs::Entity replicated = server_registry.create();
    REQUIRE(server_registry.add<Position>(replicated, Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, replicated, 1));
    REQUIRE(start_sync(server_registry, replicated, server_archetype));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions client_options;
    client_options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    client_options.input_buffer_capacity_frames = 64;
    client_options.interpolation_buffer_capacity_frames = 64;
    client_options.auto_timing_warmup_samples = 3;
    kage::sync::ReplicationClient client(client_options);

    TestLink upstream({100.0, 0.0, 0.0}, 22U);
    int first_update_tick = -1;
    int recovered_tick = -1;
    int stable_ticks = 0;
    std::uint64_t previous_starvations = 0;
    constexpr double dt = 1.0 / 60.0;

    for (int tick = 0; tick < 90; ++tick) {
        REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 0.0f}));
        REQUIRE(client.tick(client_registry, dt));
        for (const ecs::BitBuffer& packet : client.drain_packets()) {
            (void)upstream.enqueue(1, packet, now);
        }
        upstream.deliver_ready(now, [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
            REQUIRE(server.process_packet(server_registry, peer, packet));
        });

        server_registry.write<Position>(replicated).x += 1.0f;
        server.tick(server_registry);

        downstream.deliver_ready(now, [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
            ecs::BitBuffer copy = packet;
            const auto message = static_cast<std::uint8_t>(copy.read_bits(8U));
            if (message == kage::sync::protocol::server_update_message && first_update_tick < 0) {
                first_update_tick = tick;
            }
            (void)client.receive(client_registry, packet);
        });

        const kage::sync::ReplicationClientTimingStats timing = client.timing_stats();
        const kage::sync::ReplicationServer::ClientInputStats input_stats = server.input_stats(1);
        const bool prediction_ready =
            timing.current_prediction_lead_frames + 1U >= timing.target_prediction_lead_frames;
        const bool interpolation_ready =
            timing.current_interpolation_buffer_frames + 1U >= timing.target_interpolation_buffer_frames &&
            timing.target_interpolation_buffer_frames + 1U >= timing.current_interpolation_buffer_frames;
        const bool display_ready = !client.display_interpolation_frame(client_registry).entities.empty();
        if (first_update_tick >= 0 &&
            input_stats.input_starvation_frames == previous_starvations &&
            prediction_ready &&
            interpolation_ready &&
            display_ready) {
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
    INFO("interpolation current=" << client.timing_stats().current_interpolation_buffer_frames);
    INFO("interpolation target=" << client.timing_stats().target_interpolation_buffer_frames);
    INFO("display samples=" << client.display_interpolation_frame(client_registry).entities.size());
    REQUIRE(first_update_tick >= 0);
    REQUIRE(recovered_tick >= 0);
    REQUIRE(client.timing_stats().current_prediction_lead_frames == client.timing_stats().target_prediction_lead_frames);
    REQUIRE(recovered_tick - first_update_tick <= 30);
}

TEST_CASE("auto timing recovers prediction and interpolation quickly after packet loss burst") {
    using TestLink = kage::sync::SimulatedLink<ecs::BitBuffer, kage::sync::ClientId>;

    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(server_registry));

    double now = 0.0;
    TestLink downstream({50.0, 0.0, 0.0}, 33U);
    kage::sync::ReplicationServerOptions server_options;
    server_options.fixed_dt_seconds = 1.0 / 60.0;
    server_options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& packet) {
        (void)downstream.enqueue(client, packet, now);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));

    const ecs::Entity replicated = server_registry.create();
    REQUIRE(server_registry.add<Position>(replicated, Position{0.0f, 0.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, replicated, 1));
    REQUIRE(start_sync(server_registry, replicated, server_archetype));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions client_options;
    client_options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    client_options.input_buffer_capacity_frames = 64;
    client_options.interpolation_buffer_capacity_frames = 64;
    client_options.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClient client(client_options);

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
        for (const ecs::BitBuffer& packet : client.drain_packets()) {
            (void)upstream.enqueue(1, packet, now);
        }
        upstream.deliver_ready(now, [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
            REQUIRE(server.process_packet(server_registry, peer, packet));
        });

        server_registry.write<Position>(replicated).x += 1.0f;
        server.tick(server_registry);

        downstream.deliver_ready(now, [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
            ecs::BitBuffer copy = packet;
            const auto message = static_cast<std::uint8_t>(copy.read_bits(8U));
            if (message == kage::sync::protocol::server_update_message &&
                tick >= loss_end_tick &&
                first_post_loss_update_tick < 0) {
                first_post_loss_update_tick = tick;
            }
            (void)client.receive(client_registry, packet);
        });

        const kage::sync::ReplicationClientTimingStats timing = client.timing_stats();
        const kage::sync::ReplicationServer::ClientInputStats input_stats = server.input_stats(1);
        const bool prediction_ready =
            timing.current_prediction_lead_frames + 1U >= timing.target_prediction_lead_frames;
        const bool interpolation_ready =
            timing.current_interpolation_buffer_frames + 1U >= timing.target_interpolation_buffer_frames &&
            timing.target_interpolation_buffer_frames + 1U >= timing.current_interpolation_buffer_frames;
        const bool display_ready = !client.display_interpolation_frame(client_registry).entities.empty();
        if (first_post_loss_update_tick >= 0 &&
            input_stats.input_starvation_frames == previous_starvations &&
            prediction_ready &&
            interpolation_ready &&
            display_ready) {
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
    INFO("interpolation current=" << client.timing_stats().current_interpolation_buffer_frames);
    INFO("interpolation target=" << client.timing_stats().target_interpolation_buffer_frames);
    INFO("display samples=" << client.display_interpolation_frame(client_registry).entities.size());
    REQUIRE(first_post_loss_update_tick >= 0);
    REQUIRE(recovered_tick >= 0);
    REQUIRE(recovered_tick - first_post_loss_update_tick <= 30);
}
