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

namespace {

std::uint8_t packet_message(ecs::BitBuffer packet) {
    return static_cast<std::uint8_t>(packet.read_bits(8U));
}

}  // namespace

TEST_CASE("server connect response resends until client id is ACKed") {
    ecs::Registry registry;
    define_position_archetype(registry);

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> sent;
    kage::sync::ReplicationServerOptions options;
    options.connect_handler = [](const std::string& token, kage::sync::ClientId& client, std::string& error) {
        if (token != "token") {
            error = "bad token";
            return false;
        }
        client = 7;
        return true;
    };
    options.transport = [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
        sent.push_back({peer, packet});
    };
    kage::sync::ReplicationServer server(registry, options);

    REQUIRE(server.process_packet(registry, 99, make_connect_request("token")));
    REQUIRE(server.has_client(7));
    REQUIRE(sent.size() == 1);
    REQUIRE(sent.back().first == 99);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);
    REQUIRE(sent.back().second.read_bool());
    REQUIRE(sent.back().second.read_unsigned_bits(64U) == 7);

    sent.clear();
    for (int tick = 0; tick < 16; ++tick) {
        server.tick(registry, server.options().fixed_dt_seconds);
    }
    REQUIRE(sent.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);

    ecs::BitBuffer ack;
    ack.push_bits(kage::sync::protocol::client_connect_ack_message, 8U);
    ack.push_unsigned_bits(7, 64U);
    REQUIRE(server.process_packet(registry, 99, ack));

    sent.clear();
    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(sent.empty());

    REQUIRE(server.process_packet(registry, 100, make_connect_request("no")));
    REQUIRE(sent.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);
    REQUIRE_FALSE(sent.back().second.read_bool());
    std::string error;
    REQUIRE(kage::sync::protocol::read_string(sent.back().second, error));
    REQUIRE(error == "bad token");
}

TEST_CASE("server duplicate connect request from same peer resends existing accepted id") {
    ecs::Registry registry;
    define_position_archetype(registry);

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> sent;
    kage::sync::ReplicationServerOptions options;
    options.connect_handler = [](const std::string&, kage::sync::ClientId& client, std::string&) {
        client = 5;
        return true;
    };
    options.transport = [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
        sent.push_back({peer, packet});
    };
    kage::sync::ReplicationServer server(registry, options);

    REQUIRE(server.process_packet(registry, 77, make_connect_request("token")));
    REQUIRE(server.process_packet(registry, 77, make_connect_request("token")));
    REQUIRE(server.client_count() == 1U);
    REQUIRE(server.has_client(5));
    REQUIRE(sent.size() == 2);

    for (auto& [peer, packet] : sent) {
        REQUIRE(peer == 77);
        REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) ==
                kage::sync::protocol::server_connect_response_message);
        REQUIRE(packet.read_bool());
        REQUIRE(packet.read_unsigned_bits(64U) == 5);
    }
}

TEST_CASE("server starts client replication only after connect response ACK") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> sent;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.connect_handler = [](const std::string&, kage::sync::ClientId& client, std::string&) {
        client = 7;
        return true;
    };
    options.transport = [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
        sent.push_back({peer, packet});
    };
    kage::sync::ReplicationServer server(registry, options);

    REQUIRE(server.process_packet(registry, 99, make_connect_request("token")));
    REQUIRE(server.has_client(7));
    REQUIRE(sent.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(sent.back().second.read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);

    sent.clear();
    for (int tick = 0; tick < 16; ++tick) {
        REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    }
    REQUIRE(std::none_of(sent.begin(), sent.end(), [](auto& delivered) {
        ecs::BitBuffer packet = delivered.second;
        return static_cast<std::uint8_t>(packet.read_bits(8U)) ==
            kage::sync::protocol::server_update_message;
    }));

    ecs::BitBuffer ack;
    ack.push_bits(kage::sync::protocol::client_connect_ack_message, 8U);
    ack.push_unsigned_bits(7, 64U);
    REQUIRE(server.process_packet(registry, 99, ack));

    sent.clear();
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    REQUIRE(sent.size() == 1);
    ServerUpdatePacket update = read_server_update(sent.back().second);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].full);
}

TEST_CASE("client and server recover when each critical handshake packet is lost once") {
    using TestLink = kage::sync::SimulatedLink<ecs::BitBuffer, kage::sync::ClientId>;

    constexpr kage::sync::ClientId peer = 99;
    constexpr kage::sync::ClientId accepted_client = 7;

    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    const ecs::Entity entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(entity, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, entity, server_archetype));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);

    double now_seconds = 0.0;
    bool dropped_connect_request = false;
    bool dropped_connect_response = false;
    bool dropped_connect_ack = false;
    bool dropped_server_update = false;

    TestLink upstream({0.0, 0.0, 0.0}, 11U);
    TestLink downstream({0.0, 0.0, 0.0}, 22U);

    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.connect_handler = [&](const std::string& token, kage::sync::ClientId& client, std::string& error) {
        if (token != "token") {
            error = "bad token";
            return false;
        }
        client = accepted_client;
        return true;
    };
    server_options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& packet) {
        const std::uint8_t message = packet_message(packet);
        if (message == kage::sync::protocol::server_connect_response_message && !dropped_connect_response) {
            dropped_connect_response = true;
            return;
        }
        if (message == kage::sync::protocol::server_update_message && !dropped_server_update) {
            dropped_server_update = true;
            return;
        }
        (void)downstream.enqueue(client, packet, now_seconds);
    };
    kage::sync::ReplicationServer server(server_registry, server_options);

    kage::sync::ReplicationClientOptions client_options;
    client_options.session.connect_token = "token";
    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, client_options));
    client.set_packet_sender([&](const ecs::BitBuffer& packet) {
        const std::uint8_t message = packet_message(packet);
        if (message == kage::sync::protocol::client_connect_request_message && !dropped_connect_request) {
            dropped_connect_request = true;
            return;
        }
        if (message == kage::sync::protocol::client_connect_ack_message && !dropped_connect_ack) {
            dropped_connect_ack = true;
            return;
        }
        (void)upstream.enqueue(peer, packet, now_seconds);
    });

    constexpr double dt_seconds = 1.0 / 60.0;
    for (int tick = 0; tick < 240; ++tick) {
        REQUIRE(client.tick(client_registry, dt_seconds));
        upstream.deliver_ready(now_seconds, [&](kage::sync::ClientId inbound_peer, const ecs::BitBuffer& packet) {
            server.receive_packet(inbound_peer, packet);
        });

        REQUIRE(server.tick(server_registry, dt_seconds));
        downstream.deliver_ready(now_seconds, [&](kage::sync::ClientId inbound_client, const ecs::BitBuffer& packet) {
            REQUIRE(inbound_client == peer);
            REQUIRE(client.receive(client_registry, packet));
        });

        if (client.connection_state() == kage::sync::ReplicationClientConnectionState::Ready) {
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
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Ready);
}

TEST_CASE("replication client and server templates configure network id tier width") {
    ecs::Registry registry;
    kage::sync::ReplicationClientT<8> client(registry, kage_sync_tests::make_test_client_options(registry, {}));
    REQUIRE(client.options().network.protocol.network_entity_id_tier0_bits == 8U);

    kage::sync::ReplicationServerT<8> server(registry);
    REQUIRE(server.options().protocol.network_entity_id_tier0_bits == 8U);
}

TEST_CASE("replication client and server reject invalid network id tier widths") {
    ecs::Registry registry;
    kage::sync::ReplicationClientOptions client_options;
    client_options.network.protocol.network_entity_id_tier0_bits = 0U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClient(registry, client_options), std::invalid_argument);
    client_options.network.protocol.network_entity_id_tier0_bits = 23U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClient(registry, client_options), std::invalid_argument);

    kage::sync::ReplicationServerOptions server_options;
    server_options.protocol.network_entity_id_tier0_bits = 0U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationServer(registry, server_options), std::invalid_argument);
    server_options.protocol.network_entity_id_tier0_bits = 23U;
    REQUIRE_THROWS_AS(kage::sync::ReplicationServer(registry, server_options), std::invalid_argument);
}

TEST_CASE("replication client and server interoperate with custom network id tier width") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_position_archetype(server_registry);
    const ecs::Entity entity = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(
                entity,
                kage_sync_tests::Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, entity, server_archetype));

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& payload) {
        REQUIRE(client == 1);
        payloads.push_back(payload);
    };
    kage::sync::ReplicationServerT<8> server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry, server.options().fixed_dt_seconds);
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
    kage_sync_tests::configure_test_client_registry(client_registry, 1);
    kage::sync::ReplicationClientT<8> client(client_registry, kage_sync_tests::make_test_client_options(client_registry, {}));
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
    ecs::Registry registry;
    kage::sync::ReplicationServer server(registry);
    REQUIRE_FALSE(server.add_client(kage::sync::max_client_entity_network_id_client + 1U));

    REQUIRE_THROWS_AS(
        kage_sync_tests::configure_test_client_registry(registry, kage::sync::max_client_entity_network_id_client + 1U),
        std::invalid_argument);

    std::vector<ecs::BitBuffer> responses;
    kage::sync::ReplicationServerOptions options;
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& payload) {
        responses.push_back(payload);
    };
    options.connect_handler = [](const std::string&, kage::sync::ClientId& accepted, std::string&) {
        accepted = kage::sync::max_client_entity_network_id_client + 1U;
        return true;
    };
    kage::sync::ReplicationServer connect_server(registry, options);
    REQUIRE(connect_server.process_packet(registry, 99, make_connect_request("token")));
    REQUIRE_FALSE(connect_server.has_client(kage::sync::max_client_entity_network_id_client + 1U));
    REQUIRE(responses.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(responses[0].read_bits(8U)) ==
            kage::sync::protocol::server_connect_response_message);
    REQUIRE_FALSE(responses[0].read_bool());
    std::string error;
    REQUIRE(kage::sync::protocol::read_string(responses[0], error));
    REQUIRE(error == "client id out of range");
}
