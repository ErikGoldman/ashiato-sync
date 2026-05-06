#include "test_components.hpp"

#include "ecs/bit_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t max_packet_bytes = 4096;
constexpr kage::sync::ClientId fuzz_peer = 1;

ecs::BitBuffer make_selected_packet(
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

    ecs::BitBuffer packet;
    packet.assign_bytes(std::move(bytes), packet_size * 8U);
    return packet;
}

void define_fuzz_schema(ecs::Registry& registry) {
    const ecs::Entity position =
        kage::sync::register_sync_component<kage_sync_tests::Position>(registry, "Position");
    const ecs::Entity networked_position =
        kage::sync::register_sync_component<kage_sync_tests::NetworkedPosition>(registry, "NetworkedPosition");
    (void)networked_position;
    (void)kage::sync::define_archetype(
        registry,
        "FuzzActor",
        {{position, kage::sync::ReplicationAudience::All}});
}

void configure_client_registry(ecs::Registry& registry) {
    define_fuzz_schema(registry);
    kage::sync::configure_client(registry, fuzz_peer);
    (void)kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(registry);
}

void configure_server_registry(ecs::Registry& registry) {
    define_fuzz_schema(registry);
    kage::sync::configure_server(registry);
    (void)kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(registry);
}

ecs::BitBuffer make_client_seed_update() {
    kage_sync_tests::Position position{1.0F, 2.0F};
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_update_message, 8U);
    packet.push_bits(1, 32U);
    packet.push_bits(1, kage::sync::protocol::server_packet_id_bits);
    packet.push_bits(0, 32U);
    packet.push_bits(1, 16U);
    packet.push_bool(false);
    kage::sync::protocol::write_network_entity_id(packet, 1U);
    packet.push_bool(true);
    packet.push_bits(0, 32U);
    packet.push_bool(false);
    packet.push_bits(1, 16U);
    packet.push_bits(1, kage::sync::protocol::bits_for_range(2U));
    packet.push_bytes(reinterpret_cast<const char*>(&position), sizeof(position));
    packet.push_bool(false);
    return packet;
}

ecs::BitBuffer make_server_connect_response(bool accepted) {
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    packet.push_bool(accepted);
    if (accepted) {
        packet.push_unsigned_bits(fuzz_peer, 64U);
    } else {
        kage::sync::protocol::write_string(packet, "bad token");
    }
    return packet;
}

ecs::BitBuffer make_server_pong() {
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_pong_message, 8U);
    packet.push_bits(1, 32U);
    packet.push_bits(5, 32U);
    packet.push_bits(0, kage::sync::protocol::frame_subframe_bits);
    packet.push_bits(6, 32U);
    packet.push_bits(0, kage::sync::protocol::frame_subframe_bits);
    return packet;
}

ecs::BitBuffer make_client_ack() {
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_ack_message, 8U);
    packet.push_bits(1, 16U);
    packet.push_bits(1, kage::sync::protocol::server_packet_id_bits);
    return packet;
}

ecs::BitBuffer make_client_connect_request() {
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_connect_request_message, 8U);
    kage::sync::protocol::write_string(packet, "token");
    return packet;
}

ecs::BitBuffer make_client_connect_ack() {
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_connect_ack_message, 8U);
    packet.push_unsigned_bits(fuzz_peer, 64U);
    return packet;
}

ecs::BitBuffer make_client_ping() {
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_ping_message, 8U);
    packet.push_bits(1, 32U);
    packet.push_bits(5, 32U);
    packet.push_bits(0, kage::sync::protocol::frame_subframe_bits);
    return packet;
}

ecs::BitBuffer make_client_input() {
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::client_input_message, 8U);
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

void fuzz_client_receive(ecs::BitBuffer packet, bool seed_client) {
    ecs::Registry registry;
    configure_client_registry(registry);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    kage::sync::ReplicationClient client(options);
    if (seed_client) {
        (void)client.receive(registry, make_client_seed_update(), 1, 1);
    }
    (void)client.receive(registry, std::move(packet), 7, 7);
}

void fuzz_server_connected(ecs::BitBuffer packet, bool with_registry) {
    kage::sync::ReplicationServerOptions options;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);
    (void)server.add_client(fuzz_peer);

    if (with_registry) {
        ecs::Registry registry;
        configure_server_registry(registry);
        (void)server.process_packet(registry, fuzz_peer, std::move(packet));
    } else {
        (void)server.process_packet(fuzz_peer, std::move(packet));
    }
}

void fuzz_server_connect(ecs::BitBuffer packet) {
    ecs::Registry registry;
    configure_server_registry(registry);

    kage::sync::ReplicationServerOptions options;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);
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
        kage::sync::protocol::server_update_message,
        kage::sync::protocol::server_connect_response_message,
        kage::sync::protocol::server_pong_message,
    };
    constexpr std::uint8_t server_messages[] = {
        kage::sync::protocol::client_ack_message,
        kage::sync::protocol::client_connect_request_message,
        kage::sync::protocol::client_connect_ack_message,
        kage::sync::protocol::client_ping_message,
        kage::sync::protocol::client_input_message,
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
