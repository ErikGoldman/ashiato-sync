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

TEST_CASE("server connect response resends until client id is ACKed") {
    ecs::Registry registry;
    define_position_archetype(registry);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> sent;
    kage::sync::ReplicationServerOptions options;
    options.connect_handler = [](const std::string& token, kage::sync::ClientId& client, std::string& error) {
        if (token != "token") {
            error = "bad token";
            return false;
        }
        client = 7;
        return true;
    };
    options.transport = [&](kage::sync::ClientId peer, const kage::sync::BitBuffer& packet) {
        sent.push_back({peer, packet});
    };
    kage::sync::ReplicationServer server(options);

    REQUIRE(server.process_packet(99, make_connect_request("token")));
    REQUIRE(server.has_client(7));
    REQUIRE(sent.size() == 1);
    REQUIRE(sent.back().first == 99);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);
    REQUIRE(sent.back().second.read_bool());
    REQUIRE(sent.back().second.read_unsigned_bits(64U) == 7);

    sent.clear();
    for (int tick = 0; tick < 16; ++tick) {
        server.tick(registry);
    }
    REQUIRE(sent.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);

    kage::sync::BitBuffer ack;
    ack.push_bits(kage::sync::protocol::client_connect_ack_message, 8U);
    ack.push_unsigned_bits(7, 64U);
    REQUIRE(server.process_packet(99, ack));

    sent.clear();
    server.tick(registry);
    REQUIRE(sent.empty());

    REQUIRE(server.process_packet(100, make_connect_request("no")));
    REQUIRE(sent.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);
    REQUIRE_FALSE(sent.back().second.read_bool());
    std::string error;
    REQUIRE(kage::sync::protocol::read_string(sent.back().second, error));
    REQUIRE(error == "bad token");
}

TEST_CASE("replication client and server templates configure network id tier width") {
    kage::sync::ReplicationClientT<8> client;
    REQUIRE(client.options().protocol.network_entity_id_tier0_bits == 8U);

    kage::sync::ReplicationServerT<8> server;
    REQUIRE(server.options().protocol.network_entity_id_tier0_bits == 8U);
}

TEST_CASE("replication client and server reject invalid network id tier widths") {
    kage::sync::ReplicationClientOptions client_options;
    client_options.protocol.network_entity_id_tier0_bits = 0U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClient(client_options), std::invalid_argument);
    client_options.protocol.network_entity_id_tier0_bits = 23U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClient(client_options), std::invalid_argument);

    kage::sync::ReplicationServerOptions server_options;
    server_options.protocol.network_entity_id_tier0_bits = 0U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationServer(server_options), std::invalid_argument);
    server_options.protocol.network_entity_id_tier0_bits = 23U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationServer(server_options), std::invalid_argument);
}

TEST_CASE("replication client and server interoperate with custom network id tier width") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    const ecs::Entity entity = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(
                entity,
                kage_sync_tests::Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, entity, server_archetype));

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        REQUIRE(client == 1);
        payloads.push_back(payload);
    };
    kage::sync::ReplicationServerT<8> server(server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry);
    REQUIRE(payloads.size() == 1);

    const ServerUpdatePacket update = read_server_update(
        payloads[0],
        2U,
        sizeof(kage_sync_tests::Position) * 8U,
        kage::sync::ReplicationServerT<8>::network_entity_id_tier0_bits);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].network_id == 1U);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == server_archetype.value);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClientT<8> client;
    REQUIRE(client.receive(client_registry, payloads[0]));

    const kage::sync::ClientEntityNetworkId client_network_id =
        kage::sync::make_client_entity_network_id(1, update.entities[0].network_id, 1U);
    const ecs::Entity local = client.local_entity(client_network_id);
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client_registry.get<kage_sync_tests::Position>(local).x == 1.0f);
    REQUIRE(client_registry.get<kage_sync_tests::Position>(local).y == 2.0f);
    REQUIRE(client.drain_ack_packets().size() == 1);
}

TEST_CASE("replication rejects client ids that cannot fit in client entity network ids") {
    kage::sync::ReplicationServer server;
    REQUIRE_FALSE(server.add_client(kage::sync::max_client_entity_network_id_client + 1U));

    ecs::Registry registry;
    REQUIRE_THROWS_AS(
        kage::sync::configure_client(registry, kage::sync::max_client_entity_network_id_client + 1U),
        std::invalid_argument);

    std::vector<kage::sync::BitBuffer> responses;
    kage::sync::ReplicationServerOptions options;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        responses.push_back(payload);
    };
    options.connect_handler = [](const std::string&, kage::sync::ClientId& accepted, std::string&) {
        accepted = kage::sync::max_client_entity_network_id_client + 1U;
        return true;
    };
    kage::sync::ReplicationServer connect_server(options);
    REQUIRE(connect_server.process_packet(99, make_connect_request("token")));
    REQUIRE_FALSE(connect_server.has_client(kage::sync::max_client_entity_network_id_client + 1U));
    REQUIRE(responses.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(responses[0].read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);
    REQUIRE_FALSE(responses[0].read_bool());
    std::string error;
    REQUIRE(kage::sync::protocol::read_string(responses[0], error));
    REQUIRE(error == "client id out of range");
}
