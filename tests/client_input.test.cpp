#include "test_protocol.hpp"

#include "client/store/input_buffer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace ashiato_sync_tests;

namespace {

ashiato::sync::SyncComponentOps byte_input_component_ops() {
    ashiato::sync::SyncComponentOps ops;
    ops.serialization.quantized_size = 1U;
    ops.serialization.quantize = [](const void* input, std::uint8_t* out) {
        out[0] = *static_cast<const std::uint8_t*>(input);
    };
    ops.serialization.serialize = [](
        const std::uint8_t*,
        const std::uint8_t* current,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext&) {
        out.write_bits(current[0], 8U);
    };
    ops.serialization.push_to_registry = [](ashiato::Registry&, ashiato::Entity, const std::uint8_t*) {
        return true;
    };
    return ops;
}

}  // namespace

TEST_CASE("client input records trace samples for acknowledged frames") {
    ashiato::Registry registry;
    ashiato::sync::SyncSettings settings;
    settings.input_component = ashiato::Entity{11};
    settings.component_ops.emplace(settings.input_component.value, byte_input_component_ops());

    ashiato::sync::client_detail::ClientInputBuffer buffer;
    std::uint8_t input = 42U;
    REQUIRE(buffer.set_latest(registry, settings, settings.input_component, &input));

    ashiato::sync::client_detail::ClientInputRecord recorded;
    REQUIRE(buffer.record_frame(settings, 4U, 1U, &recorded));
    REQUIRE(recorded.bytes != nullptr);
    REQUIRE(recorded.bytes[0] == 42U);

    buffer.acknowledge_frame(1U);
    input = 77U;
    REQUIRE(buffer.set_latest(registry, settings, settings.input_component, &input));

    recorded = {};
    REQUIRE(buffer.record_frame(settings, 4U, 1U, &recorded));
    REQUIRE(recorded.frame == 1U);
    REQUIRE(recorded.component == settings.input_component);
    REQUIRE(recorded.bytes != nullptr);
    REQUIRE(recorded.bytes[0] == 42U);
}

TEST_CASE("replication client input applies to locally owned entities and drains with ACKs") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(registry));

    const ashiato::Entity owned = registry.create();
    const ashiato::Entity other = registry.create();
    REQUIRE(ashiato::sync::set_owner(registry, owned, 1));
    REQUIRE(ashiato::sync::set_owner(registry, other, 2));

    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, {}));
    REQUIRE(client.set_input(registry, NetworkedPosition{3.0f, 4.0f}));
    REQUIRE(registry.contains<NetworkedPosition>(owned));
    REQUIRE(registry.get<NetworkedPosition>(owned).x == 3.0f);
    REQUIRE(registry.get<NetworkedPosition>(owned).y == 4.0f);
    REQUIRE_FALSE(registry.contains<NetworkedPosition>(other));

    const ashiato::Entity server_entity = ashiato::Entity{1};
    REQUIRE(client.receive(registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    std::vector<ashiato::BitBuffer> packets;
    for (int tick = 0; tick < 4 && packets.empty(); ++tick) {
        REQUIRE(client.set_input(registry, NetworkedPosition{3.0f, 4.0f}));
        REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
        packets = client.drain_packets();
    }
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());
    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.ack_count == 1);
    REQUIRE(input.baseline_frame == 0);
    REQUIRE(input.input_count >= 1);
}

TEST_CASE("replication client sends full first input frame after unacked input buffer overwrite") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(registry));

    ashiato::sync::ReplicationClientOptions options;
    options.prediction.input_buffer_capacity_frames = 4;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    REQUIRE(client.set_input(registry, NetworkedPosition{1.0f, 2.0f}));
    for (int tick = 0; tick < 6; ++tick) {
        REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    }

    const std::vector<ashiato::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());
    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.baseline_frame == 0);
    REQUIRE(input.first_input_frame == 3);
    REQUIRE(input.input_count == 4);
    REQUIRE(input.first_input_full);
}

TEST_CASE("client input frames are immutable samples from the input clock") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(registry));

    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, {}));
    REQUIRE(client.set_input(registry, NetworkedPosition{3.0f, 4.0f}));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));

    std::vector<ashiato::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());

    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.baseline_frame == 0);
    REQUIRE(input.input_count == 1);
    REQUIRE(client.timing_stats().current_prediction_lead_frames == 2);
}

TEST_CASE("replication client acknowledges input using server input ack frame") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(registry));

    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, {}));
    REQUIRE(client.set_input(registry, NetworkedPosition{3.0f, 4.0f}));
    for (int tick = 0; tick < 6; ++tick) {
        REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    }
    (void)client.drain_packets();

    const ashiato::Entity server_entity{42};
    REQUIRE(client.receive(registry, make_position_packet(
        2,
        {{server_entity, Position{1.0f, 2.0f}}},
        2U,
        10U,
        2U)));

    std::vector<ashiato::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());

    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.baseline_frame == 2);
    REQUIRE(input.first_input_frame == 3);
    REQUIRE(input.input_count >= 1);
}

TEST_CASE("client does not send input packets before first server update") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(client_registry));

    ashiato::sync::ReplicationClientOptions options;
    options.session.connect_token = "token";
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    ashiato::BitBuffer accepted;
    accepted.write_bits(ashiato::sync::protocol::server_connect_response_message, ashiato::sync::protocol::message_bits);
    accepted.write_bool(true);
    accepted.write_unsigned_bits(1, 64U);
    REQUIRE(client.receive(client_registry, accepted));
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Accepted);

    REQUIRE(client.set_input(client_registry, NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    std::vector<ashiato::BitBuffer> packets = client.drain_packets();
    REQUIRE(std::none_of(packets.begin(), packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits)) == ashiato::sync::protocol::client_input_message;
    }));

    const ashiato::Entity server_entity{42};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Ready);

    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    packets = client.drain_packets();
    REQUIRE(std::any_of(packets.begin(), packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits)) == ashiato::sync::protocol::client_input_message;
    }));
}
