#include "test_components.hpp"

#include "ashiato/bit_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t max_packet_bytes = 4096;
constexpr ashiato::sync::ClientId fuzz_peer = 1;

ashiato::BitBuffer make_selected_packet(
    std::uint8_t selector,
    const std::uint8_t* data,
    std::size_t size,
    const std::uint8_t* messages,
    std::size_t message_count) {
    const std::size_t packet_size = std::min(size, max_packet_bytes);
    std::vector<std::uint8_t> bytes(data, data + packet_size);
    if (!bytes.empty() && (selector & 0x80U) != 0U && message_count != 0U) {
        bytes[0] = messages[(selector >> 3U) % message_count];
    }

    ashiato::BitBuffer packet;
    packet.assign_bytes(std::move(bytes), packet_size * 8U);
    return packet;
}

void define_fuzz_schema(ashiato::Registry& registry) {
    const ashiato::Entity position =
        ashiato::sync::register_sync_component<ashiato_sync_tests::Position>(registry, "Position");
    const ashiato::Entity networked_position =
        ashiato::sync::register_sync_component<ashiato_sync_tests::NetworkedPosition>(registry, "NetworkedPosition");
    (void)networked_position;
    (void)ashiato::sync::define_archetype(
        registry,
        "FuzzActor",
        {{position, ashiato::sync::ReplicationAudience::All}});
}

void setup_client_registry(ashiato::Registry& registry) {
    define_fuzz_schema(registry);
    ashiato::sync::SyncSettings& settings = registry.write<ashiato::sync::SyncSettings>();
    settings.role = ashiato::sync::SyncRole::Client;
    registry.write<ashiato::sync::SyncAuthority>().authoritative = false;
    (void)ashiato::sync::set_client_input_component<ashiato_sync_tests::NetworkedPosition>(registry);
}

void setup_server_registry(ashiato::Registry& registry) {
    define_fuzz_schema(registry);
    ashiato::sync::SyncSettings& settings = registry.write<ashiato::sync::SyncSettings>();
    settings.role = ashiato::sync::SyncRole::Server;
    registry.write<ashiato::sync::SyncAuthority>().authoritative = true;
    (void)ashiato::sync::set_client_input_component<ashiato_sync_tests::NetworkedPosition>(registry);
}

ashiato::BitBuffer make_client_seed_update() {
    ashiato_sync_tests::Position position{1.0F, 2.0F};
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::server_update_message, 8U);
    packet.push_bits(1, 32U);
    packet.push_bits(1, ashiato::sync::protocol::server_packet_id_bits);
    packet.push_bits(0, 32U);
    packet.push_bits(1, 16U);
    packet.push_bool(false);
    ashiato::sync::protocol::write_network_entity_id(packet, 1U);
    packet.push_bool(true);
    packet.push_bits(0, 32U);
    packet.push_bool(false);
    packet.push_bits(1, 16U);
    packet.push_bits(1, ashiato::sync::protocol::bits_for_range(2U));
    packet.push_bytes(reinterpret_cast<const char*>(&position), sizeof(position));
    packet.push_bool(false);
    return packet;
}

ashiato::BitBuffer make_server_connect_response(bool accepted) {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::server_connect_response_message, 8U);
    packet.push_bool(accepted);
    if (accepted) {
        packet.push_unsigned_bits(fuzz_peer, 64U);
    } else {
        ashiato::sync::protocol::write_string(packet, "bad token");
    }
    return packet;
}

ashiato::BitBuffer make_server_pong() {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::server_pong_message, 8U);
    packet.push_bits(1, 32U);
    packet.push_bits(5, 32U);
    packet.push_bits(0, ashiato::sync::protocol::frame_subframe_bits);
    packet.push_bits(6, 32U);
    packet.push_bits(0, ashiato::sync::protocol::frame_subframe_bits);
    return packet;
}

ashiato::BitBuffer make_client_ack() {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::client_ack_message, 8U);
    packet.push_bits(1, 16U);
    packet.push_bits(1, ashiato::sync::protocol::server_packet_id_bits);
    return packet;
}

ashiato::BitBuffer make_client_connect_request() {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::client_connect_request_message, 8U);
    ashiato::sync::protocol::write_string(packet, "token");
    return packet;
}

ashiato::BitBuffer make_client_connect_ack() {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::client_connect_ack_message, 8U);
    packet.push_unsigned_bits(fuzz_peer, 64U);
    return packet;
}

ashiato::BitBuffer make_client_ping() {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::client_ping_message, 8U);
    packet.push_bits(1, 32U);
    return packet;
}

ashiato::BitBuffer make_client_input() {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::client_input_message, 8U);
    packet.push_bits(0, 16U);
    packet.push_bits(0, 32U);
    packet.push_bits(1, 16U);
    packet.push_bool(true);
    packet.push_bits(1, 32U);
    packet.push_bool(false);
    packet.push_bits(10, 8U);
    packet.push_bits(20, 8U);
    return packet;
}

void fuzz_client_receive(ashiato::BitBuffer packet, bool seed_client) {
    ashiato::Registry registry;
    setup_client_registry(registry);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Snap;
    options.session.local_client = fuzz_peer;
    ashiato::sync::ReplicationClient client(registry, options);
    if (seed_client) {
        (void)client.receive(registry, make_client_seed_update());
    }
    (void)client.receive(registry, std::move(packet));
}

void fuzz_server_connected(ashiato::BitBuffer packet, bool with_registry) {
    ashiato::sync::ReplicationServerOptions options;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::Registry server_registry;
    ashiato::sync::ReplicationServer server(server_registry, options);
    (void)server.add_client(fuzz_peer);

    if (with_registry) {
        ashiato::Registry registry;
        setup_server_registry(registry);
        (void)server.process_packet(registry, fuzz_peer, std::move(packet));
    } else {
        (void)server.process_packet(fuzz_peer, std::move(packet));
    }
}

void fuzz_server_connect(ashiato::BitBuffer packet) {
    ashiato::Registry registry;
    setup_server_registry(registry);

    ashiato::sync::ReplicationServerOptions options;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);
    (void)server.process_packet(registry, fuzz_peer, std::move(packet));
}

std::string_view trimmed_seed_name(const std::uint8_t* data, std::size_t size) {
    std::string_view seed(reinterpret_cast<const char*>(data), size);
    while (!seed.empty() && (seed.back() == '\n' || seed.back() == '\r' || seed.back() == ' ' || seed.back() == '\t')) {
        seed.remove_suffix(1U);
    }
    return seed;
}

bool run_named_seed(const std::uint8_t* data, std::size_t size) {
    const std::string_view seed = trimmed_seed_name(data, size);
    if (seed == "client-update") {
        fuzz_client_receive(make_client_seed_update(), false);
        return true;
    }
    if (seed == "client-seeded-update") {
        fuzz_client_receive(make_client_seed_update(), true);
        return true;
    }
    if (seed == "client-connect-accepted") {
        fuzz_client_receive(make_server_connect_response(true), false);
        return true;
    }
    if (seed == "client-connect-rejected") {
        fuzz_client_receive(make_server_connect_response(false), false);
        return true;
    }
    if (seed == "client-pong") {
        fuzz_client_receive(make_server_pong(), false);
        return true;
    }
    if (seed == "server-connect-request") {
        fuzz_server_connect(make_client_connect_request());
        return true;
    }
    if (seed == "server-connect-ack") {
        fuzz_server_connected(make_client_connect_ack(), false);
        return true;
    }
    if (seed == "server-ack") {
        fuzz_server_connected(make_client_ack(), false);
        return true;
    }
    if (seed == "server-ping") {
        fuzz_server_connected(make_client_ping(), false);
        return true;
    }
    if (seed == "server-input") {
        fuzz_server_connected(make_client_input(), true);
        return true;
    }
    return false;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0U) {
        return 0;
    }
    if (run_named_seed(data, size)) {
        return 0;
    }

    const std::uint8_t scenario = data[0];
    const std::uint8_t selector = size > 1U ? data[1] : 0U;
    const std::uint8_t* packet_data = data + std::min<std::size_t>(size, 2U);
    const std::size_t packet_size = size > 2U ? size - 2U : 0U;

    constexpr std::uint8_t client_messages[] = {
        ashiato::sync::protocol::server_update_message,
        ashiato::sync::protocol::server_connect_response_message,
        ashiato::sync::protocol::server_pong_message,
    };
    constexpr std::uint8_t server_messages[] = {
        ashiato::sync::protocol::client_ack_message,
        ashiato::sync::protocol::client_connect_request_message,
        ashiato::sync::protocol::client_connect_ack_message,
        ashiato::sync::protocol::client_ping_message,
        ashiato::sync::protocol::client_input_message,
    };

    switch (scenario % 5U) {
    case 0:
        fuzz_client_receive(
            make_selected_packet(selector, packet_data, packet_size, client_messages, std::size(client_messages)),
            false);
        break;
    case 1:
        fuzz_client_receive(
            make_selected_packet(selector, packet_data, packet_size, client_messages, std::size(client_messages)),
            true);
        break;
    case 2:
        fuzz_server_connect(
            make_selected_packet(selector, packet_data, packet_size, server_messages, std::size(server_messages)));
        break;
    case 3:
        fuzz_server_connected(
            make_selected_packet(selector, packet_data, packet_size, server_messages, std::size(server_messages)),
            false);
        break;
    default:
        fuzz_server_connected(
            make_selected_packet(selector, packet_data, packet_size, server_messages, std::size(server_messages)),
            true);
        break;
    }

    return 0;
}
