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

namespace {

std::uint8_t packet_message(ashiato::BitBuffer packet) {
    return static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits));
}

}  // namespace

TEST_CASE("server connect response resends until client id is ACKed") {
    ashiato::Registry registry;
    define_position_archetype(registry);

    std::vector<std::pair<ashiato::sync::PeerId, ashiato::BitBuffer>> sent;
    ashiato::sync::ReplicationServerOptions options;
    options.connect_handler = [](const std::string& token, ashiato::sync::ClientId& client, std::string& error) {
        if (token != "token") {
            error = "bad token";
            return false;
        }
        client = 7;
        return true;
    };
    options.transport = [&](ashiato::sync::PeerId peer, const ashiato::BitBuffer& packet) {
        sent.push_back({peer, packet});
    };
    ashiato::sync::ReplicationServer server(registry, options);

    REQUIRE(server.process_packet(registry, 99, make_connect_request("token")));
    REQUIRE(server.has_client(7));
    REQUIRE(sent.size() == 1);
    REQUIRE(sent.back().first == 99);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(ashiato::sync::protocol::message_bits)) ==
            ashiato::sync::protocol::server_connect_response_message);
    REQUIRE(sent.back().second.read_bool());
    REQUIRE(sent.back().second.read_unsigned_bits(ashiato::sync::protocol::client_id_bits) == 7);

    sent.clear();
    for (int tick = 0; tick < 16; ++tick) {
        server.tick(registry, server.options().fixed_dt_seconds);
    }
    REQUIRE(sent.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(ashiato::sync::protocol::message_bits)) ==
            ashiato::sync::protocol::server_connect_response_message);

    ashiato::BitBuffer ack;
    ack.write_bits(ashiato::sync::protocol::client_connect_ack_message, ashiato::sync::protocol::message_bits);
    ack.write_unsigned_bits(7, ashiato::sync::protocol::client_id_bits);
    REQUIRE(server.process_packet(registry, 99, ack));

    sent.clear();
    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(sent.empty());

    REQUIRE(server.process_packet(registry, 100, make_connect_request("no")));
    REQUIRE(sent.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(ashiato::sync::protocol::message_bits)) ==
            ashiato::sync::protocol::server_connect_response_message);
    REQUIRE_FALSE(sent.back().second.read_bool());
    std::string error;
    REQUIRE(ashiato::sync::protocol::read_string(sent.back().second, error));
    REQUIRE(error == "bad token");
}

TEST_CASE("server duplicate connect request from same peer resends existing accepted id") {
    ashiato::Registry registry;
    define_position_archetype(registry);

    std::vector<std::pair<ashiato::sync::PeerId, ashiato::BitBuffer>> sent;
    ashiato::sync::ReplicationServerOptions options;
    options.connect_handler = [](const std::string&, ashiato::sync::ClientId& client, std::string&) {
        client = 5;
        return true;
    };
    options.transport = [&](ashiato::sync::ClientId peer, const ashiato::BitBuffer& packet) {
        sent.push_back({peer, packet});
    };
    ashiato::sync::ReplicationServer server(registry, options);

    REQUIRE(server.process_packet(registry, 77, make_connect_request("token")));
    REQUIRE(server.process_packet(registry, 77, make_connect_request("token")));
    REQUIRE(server.client_count() == 1U);
    REQUIRE(server.has_client(5));
    REQUIRE(sent.size() == 2);

    for (auto& [peer, packet] : sent) {
        REQUIRE(peer == 77);
        REQUIRE(static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits)) ==
                ashiato::sync::protocol::server_connect_response_message);
        REQUIRE(packet.read_bool());
        REQUIRE(packet.read_unsigned_bits(ashiato::sync::protocol::client_id_bits) == 5);
    }
}

TEST_CASE("server starts client replication only after connect response ACK") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<std::pair<ashiato::sync::PeerId, ashiato::BitBuffer>> sent;
    ashiato::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.connect_handler = [](const std::string&, ashiato::sync::ClientId& client, std::string&) {
        client = 7;
        return true;
    };
    options.transport = [&](ashiato::sync::ClientId peer, const ashiato::BitBuffer& packet) {
        sent.push_back({peer, packet});
    };
    ashiato::sync::ReplicationServer server(registry, options);

    REQUIRE(server.process_packet(registry, 99, make_connect_request("token")));
    REQUIRE(server.has_client(7));
    REQUIRE(sent.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(ashiato::sync::protocol::message_bits)) ==
            ashiato::sync::protocol::server_connect_response_message);

    sent.clear();
    for (int tick = 0; tick < 16; ++tick) {
        REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    }
    REQUIRE(std::none_of(sent.begin(), sent.end(), [](auto& delivered) {
        ashiato::BitBuffer packet = delivered.second;
        return static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits)) ==
            ashiato::sync::protocol::server_update_message;
    }));

    ashiato::BitBuffer ack;
    ack.write_bits(ashiato::sync::protocol::client_connect_ack_message, ashiato::sync::protocol::message_bits);
    ack.write_unsigned_bits(7, ashiato::sync::protocol::client_id_bits);
    REQUIRE(server.process_packet(registry, 99, ack));

    sent.clear();
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    REQUIRE(sent.size() == 1);
    ServerUpdatePacket update = read_server_update(sent.back().second);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].full);
}

TEST_CASE("client and server recover when each critical handshake packet is lost once") {
    using TestLink = ashiato::sync::SimulatedLink<ashiato::BitBuffer, ashiato::sync::ClientId>;

    constexpr ashiato::sync::PeerId peer = 99;
    constexpr ashiato::sync::ClientId accepted_client = 7;

    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    const ashiato::Entity entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(entity, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, entity, server_archetype));

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);

    double now_seconds = 0.0;
    bool dropped_connect_request = false;
    bool dropped_connect_response = false;
    bool dropped_connect_ack = false;
    bool dropped_server_update = false;

    TestLink upstream({0.0, 0.0, 0.0}, 11U);
    TestLink downstream({0.0, 0.0, 0.0}, 22U);

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.connect_handler = [&](const std::string& token, ashiato::sync::ClientId& client, std::string& error) {
        if (token != "token") {
            error = "bad token";
            return false;
        }
        client = accepted_client;
        return true;
    };
    server_options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& packet) {
        const std::uint8_t message = packet_message(packet);
        if (message == ashiato::sync::protocol::server_connect_response_message && !dropped_connect_response) {
            dropped_connect_response = true;
            return;
        }
        if (message == ashiato::sync::protocol::server_update_message && !dropped_server_update) {
            dropped_server_update = true;
            return;
        }
        (void)downstream.enqueue(client, packet, now_seconds);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);

    ashiato::sync::ReplicationClientOptions client_options;
    client_options.session.connect_token = "token";
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, client_options));
    client.set_packet_sender([&](const ashiato::BitBuffer& packet) {
        const std::uint8_t message = packet_message(packet);
        if (message == ashiato::sync::protocol::client_connect_request_message && !dropped_connect_request) {
            dropped_connect_request = true;
            return;
        }
        if (message == ashiato::sync::protocol::client_connect_ack_message && !dropped_connect_ack) {
            dropped_connect_ack = true;
            return;
        }
        (void)upstream.enqueue(peer, packet, now_seconds);
    });

    constexpr double dt_seconds = 1.0 / 60.0;
    for (int tick = 0; tick < 240; ++tick) {
        REQUIRE(client.tick(client_registry, dt_seconds));
        upstream.deliver_ready(now_seconds, [&](ashiato::sync::ClientId inbound_peer, const ashiato::BitBuffer& packet) {
            server.receive_packet(inbound_peer, packet);
        });

        REQUIRE(server.tick(server_registry, dt_seconds));
        downstream.deliver_ready(now_seconds, [&](ashiato::sync::ClientId inbound_client, const ashiato::BitBuffer& packet) {
            REQUIRE(inbound_client == peer);
            REQUIRE(client.receive(client_registry, packet));
        });

        if (client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Ready) {
            break;
        }
        now_seconds += dt_seconds;
    }

    INFO("dropped_connect_request=" << dropped_connect_request);
    INFO("dropped_connect_response=" << dropped_connect_response);
    INFO("dropped_connect_ack=" << dropped_connect_ack);
    INFO("dropped_server_update=" << dropped_server_update);
    REQUIRE(dropped_connect_request);
    REQUIRE(dropped_connect_response);
    REQUIRE(dropped_connect_ack);
    REQUIRE(dropped_server_update);
    REQUIRE(server.has_client(accepted_client));
    REQUIRE(client.client_id() == accepted_client);
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Ready);
}

TEST_CASE("replication client and server templates configure network id tier width") {
    ashiato::Registry registry;
    ashiato::sync::ReplicationClientT<8> client(registry, ashiato_sync_tests::make_test_client_options(registry, {}));
    REQUIRE(client.options().network.protocol.network_entity_id_tier0_bits == 8U);

    ashiato::sync::ReplicationServerT<8> server(registry);
    REQUIRE(server.options().protocol.network_entity_id_tier0_bits == 8U);
}

TEST_CASE("replication client and server reject invalid network id tier widths") {
    ashiato::Registry registry;
    ashiato::sync::ReplicationClientOptions client_options;
    client_options.network.protocol.network_entity_id_tier0_bits = 0U;
    REQUIRE_THROWS_AS(ashiato::sync::ReplicationClient(registry, client_options), std::invalid_argument);
    client_options.network.protocol.network_entity_id_tier0_bits = 23U;
    REQUIRE_THROWS_AS(ashiato::sync::ReplicationClient(registry, client_options), std::invalid_argument);

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.protocol.network_entity_id_tier0_bits = 0U;
    REQUIRE_THROWS_AS(ashiato::sync::ReplicationServer(registry, server_options), std::invalid_argument);
    server_options.protocol.network_entity_id_tier0_bits = 23U;
    REQUIRE_THROWS_AS(ashiato::sync::ReplicationServer(registry, server_options), std::invalid_argument);
}

TEST_CASE("replication client and server interoperate with custom network id tier width") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    const ashiato::Entity entity = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::Position>(
                entity,
                ashiato_sync_tests::Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, entity, server_archetype));

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& payload) {
        REQUIRE(client == 1);
        payloads.push_back(payload);
    };
    ashiato::sync::ReplicationServerT<8> server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(payloads.size() == 1);

    const ServerUpdatePacket update = read_server_update(
        payloads[0],
        2U,
        sizeof(ashiato_sync_tests::Position) * 8U,
        ashiato::sync::ReplicationServerT<8>::network_entity_id_tier0_bits);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].network_id == 1U);

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == server_archetype.value);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    ashiato::sync::ReplicationClientT<8> client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    REQUIRE(client.receive(client_registry, payloads[0]));

    const ashiato::sync::ClientEntityNetworkId client_network_id =
        ashiato::sync::make_client_entity_network_id(1, update.entities[0].network_id, 1U);
    const ashiato::Entity local = client.local_entity(client_network_id);
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client_registry.get<ashiato_sync_tests::Position>(local).x == 1.0f);
    REQUIRE(client_registry.get<ashiato_sync_tests::Position>(local).y == 2.0f);
    REQUIRE(client.drain_ack_packets().size() == 1);
}

TEST_CASE("replication rejects client ids that cannot fit in client entity network ids") {
    ashiato::Registry registry;
    ashiato::sync::ReplicationServer server(registry);
    REQUIRE_FALSE(server.add_client(ashiato::sync::max_client_entity_network_id_client + 1U));

    REQUIRE_THROWS_AS(
        ashiato_sync_tests::configure_test_client_registry(registry, ashiato::sync::max_client_entity_network_id_client + 1U),
        std::invalid_argument);

    std::vector<ashiato::BitBuffer> responses;
    ashiato::sync::ReplicationServerOptions options;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& payload) {
        responses.push_back(payload);
    };
    options.connect_handler = [](const std::string&, ashiato::sync::ClientId& accepted, std::string&) {
        accepted = ashiato::sync::max_client_entity_network_id_client + 1U;
        return true;
    };
    ashiato::sync::ReplicationServer connect_server(registry, options);
    REQUIRE_FALSE(connect_server.process_packet(registry, 99, make_connect_request("token")));
    REQUIRE_FALSE(connect_server.has_client(ashiato::sync::max_client_entity_network_id_client + 1U));
    REQUIRE(responses.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(responses[0].read_bits(ashiato::sync::protocol::message_bits)) ==
            ashiato::sync::protocol::server_connect_response_message);
    REQUIRE_FALSE(responses[0].read_bool());
    std::string error;
    REQUIRE(ashiato::sync::protocol::read_string(responses[0], error));
    REQUIRE(error == "client id out of range");
}

TEST_CASE("server reuses freed client ids after the client id counter wraps") {
    ashiato::Registry registry;
    define_position_archetype(registry);

    std::vector<ashiato::BitBuffer> responses;
    ashiato::sync::ReplicationServerOptions options;
    options.transport = [&](ashiato::sync::PeerId peer, const ashiato::BitBuffer& payload) {
        REQUIRE(peer == 1000U);
        responses.push_back(payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);

    for (std::uint16_t client = 1; client <= ashiato::sync::max_client_entity_network_id_client; ++client) {
        REQUIRE(server.add_client(static_cast<ashiato::sync::ClientId>(client)));
    }
    REQUIRE(server.remove_client(registry, 1));

    REQUIRE(server.process_packet(registry, 1000U, make_connect_request("token")));
    REQUIRE(server.has_client(1));
    REQUIRE(responses.size() == 1);
    ashiato::BitBuffer response = responses[0];
    REQUIRE(static_cast<std::uint8_t>(response.read_bits(ashiato::sync::protocol::message_bits)) ==
            ashiato::sync::protocol::server_connect_response_message);
    REQUIRE(response.read_bool());
    REQUIRE(response.read_unsigned_bits(ashiato::sync::protocol::client_id_bits) == 1);
}
