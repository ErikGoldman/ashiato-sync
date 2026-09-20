#include "test_components.hpp"

#include "ashiato/bit_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
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
    packet.write_bits(ashiato::sync::protocol::server_update_message, ashiato::sync::protocol::message_bits);
    packet.write_bits(1, 32U);
    packet.write_bits(1, ashiato::sync::protocol::server_packet_id_bits);
    packet.write_bits(0, 32U);
    packet.write_bits(1, 16U);
    packet.write_bool(false);
    ashiato::sync::protocol::write_network_entity_id(packet, 1U);
    packet.write_bool(true);
    packet.write_bits(0, 32U);
    packet.write_bool(false);
    packet.write_bits(1, 16U);
    packet.write_bits(1, ashiato::sync::protocol::bits_for_range(2U));
    packet.write_bytes(reinterpret_cast<const char*>(&position), sizeof(position));
    packet.write_bool(false);
    return packet;
}

ashiato::BitBuffer make_server_connect_response(bool accepted) {
    ashiato::BitBuffer packet;
    packet.write_bits(ashiato::sync::protocol::server_connect_response_message, ashiato::sync::protocol::message_bits);
    packet.write_bool(accepted);
    if (accepted) {
        packet.write_unsigned_bits(fuzz_peer, 64U);
    } else {
        ashiato::sync::protocol::write_string(packet, "bad token");
    }
    return packet;
}

ashiato::BitBuffer make_server_pong() {
    ashiato::BitBuffer packet;
    packet.write_bits(ashiato::sync::protocol::server_pong_message, ashiato::sync::protocol::message_bits);
    packet.write_bits(1, 32U);
    packet.write_bits(5, 32U);
    packet.write_bits(0, ashiato::sync::protocol::frame_subframe_bits);
    packet.write_bits(6, 32U);
    packet.write_bits(0, ashiato::sync::protocol::frame_subframe_bits);
    return packet;
}

ashiato::BitBuffer make_client_ack() {
    ashiato::BitBuffer packet;
    packet.write_bits(ashiato::sync::protocol::client_ack_message, ashiato::sync::protocol::message_bits);
    packet.write_bits(1, ashiato::sync::protocol::ack_count_bits);
    packet.write_bits(1, ashiato::sync::protocol::server_packet_id_bits);
    return packet;
}

ashiato::BitBuffer make_client_connect_request() {
    ashiato::BitBuffer packet;
    packet.write_bits(ashiato::sync::protocol::client_connect_request_message, ashiato::sync::protocol::message_bits);
    ashiato::sync::protocol::write_string(packet, "token");
    return packet;
}

ashiato::BitBuffer make_client_connect_ack() {
    ashiato::BitBuffer packet;
    packet.write_bits(ashiato::sync::protocol::client_connect_ack_message, ashiato::sync::protocol::message_bits);
    packet.write_unsigned_bits(fuzz_peer, 64U);
    return packet;
}

ashiato::BitBuffer make_client_ping() {
    ashiato::BitBuffer packet;
    packet.write_bits(ashiato::sync::protocol::client_ping_message, ashiato::sync::protocol::message_bits);
    packet.write_bits(1, 32U);
    return packet;
}

ashiato::BitBuffer make_client_input() {
    ashiato::BitBuffer packet;
    packet.write_bits(ashiato::sync::protocol::client_input_message, ashiato::sync::protocol::message_bits);
    packet.write_bits(0, ashiato::sync::protocol::ack_count_bits);
    packet.write_bits(0, 32U);
    packet.write_bool(true);
    packet.write_bits(1, 32U);
    packet.write_bits(1, ashiato::sync::protocol::input_count_bits);
    packet.write_bool(false);
    packet.write_bits(10, 8U);
    packet.write_bits(20, 8U);
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

class StatefulProtocolHarness {
public:
    StatefulProtocolHarness() {
        const ashiato::sync::SyncArchetypeId server_archetype = define_schema(server_registry_);
        (void)define_schema(client_registry_);
        setup_server_registry(server_registry_);
        setup_client_registry(client_registry_);

        ashiato::sync::ReplicationServerOptions server_options;
        server_options.bandwidth_limit_bytes_per_tick = 96U;
        server_options.mtu_bytes = 96U;
        server_options.protocol.max_pending_packet_acks_per_client = 7U;
        server_options.transport = [this](ashiato::sync::PeerId, const ashiato::BitBuffer& packet) {
            server_packets_.push_back(packet);
        };
        server_ = std::make_unique<ashiato::sync::ReplicationServer>(server_registry_, server_options);
        (void)server_->add_client(fuzz_peer);

        ashiato::sync::ReplicationClientOptions client_options;
        client_options.session.local_client = fuzz_peer;
        client_options.network.mtu_bytes = 96U;
        client_options.network.protocol.max_pending_packet_acks_per_client = 7U;
        client_ = std::make_unique<ashiato::sync::ReplicationClient>(client_registry_, client_options);
        spawn_entity(server_archetype);
    }

    void run(const std::uint8_t* data, std::size_t size) {
        std::size_t cursor = 0;
        std::size_t actions = 0;
        while (cursor < size && actions < 256U) {
            const std::uint8_t opcode = data[cursor++];
            apply(opcode, data, size, cursor);
            ++actions;
        }

        for (std::size_t drain = 0; drain < 16U; ++drain) {
            deliver_all_server_packets();
            (void)client_->tick(client_registry_, client_->fixed_dt_seconds());
            collect_client_packets();
            deliver_all_client_packets();
            (void)server_->tick(server_registry_, server_->options().fixed_dt_seconds);
        }
    }

private:
    static ashiato::sync::SyncArchetypeId define_schema(ashiato::Registry& registry) {
        const ashiato::Entity position =
            ashiato::sync::register_sync_component<ashiato_sync_tests::NetworkedPosition>(
                registry,
                "NetworkedPosition");
        return ashiato::sync::define_archetype(
            registry,
            "StatefulFuzzActor",
            {{position, ashiato::sync::ReplicationAudience::All}});
    }

    void spawn_entity(ashiato::sync::SyncArchetypeId archetype) {
        entity_ = server_registry_.create();
        (void)server_registry_.add<ashiato_sync_tests::NetworkedPosition>(
            entity_,
            ashiato_sync_tests::NetworkedPosition{});
        (void)server_registry_.add<ashiato::sync::Replicated>(
            entity_,
            ashiato::sync::Replicated{archetype});
        archetype_ = archetype;
    }

    void apply(
        std::uint8_t opcode,
        const std::uint8_t* data,
        std::size_t size,
        std::size_t& cursor) {
        switch (opcode % 10U) {
        case 0:
            (void)server_->tick(server_registry_, server_->options().fixed_dt_seconds);
            break;
        case 1:
            (void)client_->tick(client_registry_, client_->fixed_dt_seconds());
            collect_client_packets();
            break;
        case 2:
            deliver_selected(server_packets_, [&](ashiato::BitBuffer packet) {
                (void)client_->receive(client_registry_, std::move(packet));
            }, data, size, cursor);
            break;
        case 3:
            collect_client_packets();
            deliver_selected(client_packets_, [&](ashiato::BitBuffer packet) {
                (void)server_->process_packet(server_registry_, fuzz_peer, std::move(packet));
            }, data, size, cursor);
            break;
        case 4:
            mutate_entity(opcode);
            break;
        case 5:
            churn_entity();
            break;
        case 6:
            inject_packet(true, data, size, cursor);
            break;
        case 7:
            inject_packet(false, data, size, cursor);
            break;
        case 8:
            duplicate_selected_server_packet(data, size, cursor);
            break;
        default:
            deliver_all_server_packets();
            collect_client_packets();
            deliver_all_client_packets();
            break;
        }
    }

    template <typename Fn>
    static void deliver_selected(
        std::vector<ashiato::BitBuffer>& packets,
        Fn&& deliver,
        const std::uint8_t* data,
        std::size_t size,
        std::size_t& cursor) {
        if (packets.empty() || cursor >= size) {
            return;
        }
        const std::size_t index = static_cast<std::size_t>(data[cursor++]) % packets.size();
        ashiato::BitBuffer packet = std::move(packets[index]);
        packets.erase(packets.begin() + static_cast<std::ptrdiff_t>(index));
        deliver(std::move(packet));
    }

    void mutate_entity(std::uint8_t value) {
        if (!server_registry_.alive(entity_)) {
            return;
        }
        ashiato_sync_tests::NetworkedPosition& position =
            server_registry_.write<ashiato_sync_tests::NetworkedPosition>(entity_);
        position.x += static_cast<float>(value & 0x0fU) / 10.0F;
        position.y -= static_cast<float>((value >> 4U) & 0x0fU) / 10.0F;
    }

    void churn_entity() {
        if (server_registry_.alive(entity_)) {
            (void)server_registry_.destroy(entity_);
            return;
        }
        spawn_entity(archetype_);
    }

    void inject_packet(
        bool into_client,
        const std::uint8_t* data,
        std::size_t size,
        std::size_t& cursor) {
        if (cursor >= size) {
            return;
        }
        const std::size_t requested = static_cast<std::size_t>(data[cursor++]);
        const std::size_t packet_size = std::min({requested, size - cursor, max_packet_bytes});
        ashiato::BitBuffer packet;
        packet.assign_bytes(
            std::vector<std::uint8_t>(data + cursor, data + cursor + packet_size),
            packet_size * 8U);
        cursor += packet_size;
        if (into_client) {
            (void)client_->receive(client_registry_, std::move(packet));
        } else {
            (void)server_->process_packet(server_registry_, fuzz_peer, std::move(packet));
        }
    }

    void duplicate_selected_server_packet(
        const std::uint8_t* data,
        std::size_t size,
        std::size_t& cursor) {
        if (server_packets_.empty() || cursor >= size) {
            return;
        }
        const std::size_t index = static_cast<std::size_t>(data[cursor++]) % server_packets_.size();
        (void)client_->receive(client_registry_, server_packets_[index]);
        (void)client_->receive(client_registry_, server_packets_[index]);
    }

    void collect_client_packets() {
        std::vector<ashiato::BitBuffer> drained = client_->drain_packets();
        client_packets_.insert(
            client_packets_.end(),
            std::make_move_iterator(drained.begin()),
            std::make_move_iterator(drained.end()));
    }

    void deliver_all_server_packets() {
        std::vector<ashiato::BitBuffer> packets = std::move(server_packets_);
        server_packets_.clear();
        for (ashiato::BitBuffer& packet : packets) {
            (void)client_->receive(client_registry_, std::move(packet));
        }
    }

    void deliver_all_client_packets() {
        std::vector<ashiato::BitBuffer> packets = std::move(client_packets_);
        client_packets_.clear();
        for (ashiato::BitBuffer& packet : packets) {
            (void)server_->process_packet(server_registry_, fuzz_peer, std::move(packet));
        }
    }

    ashiato::Registry server_registry_;
    ashiato::Registry client_registry_;
    std::unique_ptr<ashiato::sync::ReplicationServer> server_;
    std::unique_ptr<ashiato::sync::ReplicationClient> client_;
    ashiato::Entity entity_;
    ashiato::sync::SyncArchetypeId archetype_;
    std::vector<ashiato::BitBuffer> server_packets_;
    std::vector<ashiato::BitBuffer> client_packets_;
};

void fuzz_stateful_sequence(const std::uint8_t* data, std::size_t size) {
    StatefulProtocolHarness harness;
    harness.run(data, size);
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

    switch (scenario % 6U) {
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
    case 4:
        fuzz_server_connected(
            make_selected_packet(selector, packet_data, packet_size, server_messages, std::size(server_messages)),
            true);
        break;
    default:
        fuzz_stateful_sequence(packet_data, packet_size);
        break;
    }

    return 0;
}
