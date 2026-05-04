#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

TEST_CASE("replication client input applies to locally owned entities and drains with ACKs") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    kage::sync::configure_client(registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(registry));

    const ecs::Entity owned = registry.create();
    const ecs::Entity other = registry.create();
    REQUIRE(kage::sync::set_owner(registry, owned, 1));
    REQUIRE(kage::sync::set_owner(registry, other, 2));

    kage::sync::ReplicationClient client;
    REQUIRE(client.set_input(registry, NetworkedPosition{3.0f, 4.0f}));
    REQUIRE(registry.contains<NetworkedPosition>(owned));
    REQUIRE(registry.get<NetworkedPosition>(owned).x == 3.0f);
    REQUIRE(registry.get<NetworkedPosition>(owned).y == 4.0f);
    REQUIRE_FALSE(registry.contains<NetworkedPosition>(other));

    const ecs::Entity server_entity = ecs::Entity{1};
    REQUIRE(client.receive(registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));

    const std::vector<ecs::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());
    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.ack_count == 1);
    REQUIRE(input.baseline_frame == 0);
    REQUIRE(input.input_count == 1);
}

TEST_CASE("replication client sends full first input frame after unacked input buffer overwrite") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    kage::sync::configure_client(registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(registry));

    kage::sync::ReplicationClientOptions options;
    options.input_buffer_capacity_frames = 4;
    kage::sync::ReplicationClient client(options);
    REQUIRE(client.set_input(registry, NetworkedPosition{1.0f, 2.0f}));
    for (int tick = 0; tick < 6; ++tick) {
        REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    }

    const std::vector<ecs::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());
    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.baseline_frame == 0);
    REQUIRE(input.first_input_frame == 3);
    REQUIRE(input.input_count == 4);
    REQUIRE(input.first_input_full);
}

TEST_CASE("client input frames are immutable samples from the input clock") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    kage::sync::configure_client(registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(registry));

    kage::sync::ReplicationClient client;
    REQUIRE(client.set_input(registry, NetworkedPosition{3.0f, 4.0f}));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));

    std::vector<ecs::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());

    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.baseline_frame == 0);
    REQUIRE(input.input_count == 1);
    REQUIRE(client.timing_stats().current_prediction_lead_frames == 2);
}

TEST_CASE("client does not send input packets before first server update") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions options;
    options.connect_token = "token";
    kage::sync::ReplicationClient client(options);

    ecs::BitBuffer accepted;
    accepted.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    accepted.push_bool(true);
    accepted.push_unsigned_bits(1, 64U);
    REQUIRE(client.receive(client_registry, accepted));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Accepted);

    REQUIRE(client.set_input(client_registry, NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    std::vector<ecs::BitBuffer> packets = client.drain_packets();
    REQUIRE(std::none_of(packets.begin(), packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    }));

    const ecs::Entity server_entity{42};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Ready);

    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    packets = client.drain_packets();
    REQUIRE(std::any_of(packets.begin(), packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    }));
}
