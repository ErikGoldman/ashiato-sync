#include "test_protocol.hpp"

#include "kage/sync/simulated_link.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>

using namespace kage_sync_tests;

namespace {

std::uint8_t packet_message(ecs::BitBuffer packet) {
    return static_cast<std::uint8_t>(packet.read_bits(8U));
}

struct SmokeClient {
    kage::sync::ClientId id = kage::sync::invalid_client_id;
    kage::sync::ClientId peer = kage::sync::invalid_client_id;
    kage::sync::ReplicationClientMode mode = kage::sync::ReplicationClientMode::Snap;
    ecs::Registry registry;
    kage::sync::ReplicationClient client;
};

}  // namespace

TEST_CASE("three clients connect with prediction snap and buffered interpolation modes") {
    using Link = kage::sync::SimulatedLink<ecs::BitBuffer, kage::sync::ClientId>;

    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_predicted_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<PredictedPosition>(server_entity, PredictedPosition{0.0f, 10.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server_registry.job<PredictedPosition>(0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    double now_seconds = 0.0;
    Link upstream({0.0, 0.0, 0.0}, 11U);
    Link downstream({0.0, 0.0, 0.0}, 22U);

    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 4096;
    server_options.connect_handler = [](const std::string& token, kage::sync::ClientId& client, std::string& error) {
        if (token == "predict") {
            client = 1;
            return true;
        }
        if (token == "snap") {
            client = 2;
            return true;
        }
        if (token == "buffered") {
            client = 3;
            return true;
        }
        error = "unknown token";
        return false;
    };
    server_options.transport = [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
        REQUIRE(downstream.enqueue(peer, packet, now_seconds));
    };
    kage::sync::ReplicationServer server(server_options);

    std::array<SmokeClient, 3> clients;
    auto setup_client = [&](std::size_t index,
                            kage::sync::ClientId id,
                            kage::sync::ClientId peer,
                            const char* token,
                            kage::sync::ReplicationClientMode mode) {
        kage::sync::ReplicationClientOptions options;
        options.connect_token = token;
        options.default_entity_mode = mode;
        options.auto_interpolation_buffer_frames = false;
        options.interpolation_buffer_frames = 2;
        options.interpolation_buffer_capacity_frames = 8;
        options.auto_prediction_lead_frames = false;
        options.prediction_lead_frames = 2;

        SmokeClient& smoke = clients[index];
        smoke.id = id;
        smoke.peer = peer;
        smoke.mode = mode;
        const kage::sync::SyncArchetypeId client_archetype = define_predicted_archetype(smoke.registry);
        REQUIRE(client_archetype == server_archetype);
        smoke.client = kage::sync::ReplicationClient(options);
        if (mode == kage::sync::ReplicationClientMode::Predict) {
            smoke.client.simulation_job<PredictedPosition>(smoke.registry, 0).each(
                [](ecs::Entity, PredictedPosition& position) {
                    position.x += 1.0f;
                });
        }
        smoke.client.set_packet_sender([&, peer](const ecs::BitBuffer& packet) {
            REQUIRE(upstream.enqueue(peer, packet, now_seconds));
        });
    };

    setup_client(0, 1, 101, "predict", kage::sync::ReplicationClientMode::Predict);
    setup_client(1, 2, 102, "snap", kage::sync::ReplicationClientMode::Snap);
    setup_client(2, 3, 103, "buffered", kage::sync::ReplicationClientMode::BufferedInterpolation);

    float previous_buffered_x = 0.0f;
    bool saw_buffered_position = false;
    constexpr int max_ticks = 180;
    for (int tick = 0; tick < max_ticks; ++tick) {
        for (SmokeClient& smoke : clients) {
            REQUIRE(smoke.client.tick(smoke.registry, smoke.client.options().fixed_dt_seconds));
        }
        upstream.deliver_ready(now_seconds, [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
            server.receive_packet(peer, packet);
        });

        REQUIRE(server.tick(server_registry, server.options().fixed_dt_seconds));
        const kage::sync::SyncFrame server_frame = server.frame();
        downstream.deliver_ready(now_seconds, [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
            auto found = std::find_if(clients.begin(), clients.end(), [&](const SmokeClient& smoke) {
                return smoke.peer == peer;
            });
            REQUIRE(found != clients.end());
            if (packet_message(packet) == kage::sync::protocol::server_update_message) {
                REQUIRE(found->client.receive(found->registry, packet, server_frame));
            } else {
                REQUIRE(found->client.receive(found->registry, packet));
            }
        });

        const kage::sync::ClientEntityNetworkId buffered_network_id =
            first_allocated_client_entity_network_id(clients[2].id);
        const ecs::Entity buffered_local = clients[2].client.local_entity(buffered_network_id);
        if (buffered_local) {
            const float buffered_x = clients[2].registry.get<PredictedPosition>(buffered_local).x;
            if (saw_buffered_position) {
                REQUIRE(buffered_x >= previous_buffered_x);
            }
            previous_buffered_x = buffered_x;
            saw_buffered_position = true;
        }

        now_seconds += server.options().fixed_dt_seconds;
    }

    REQUIRE(server.has_client(1));
    REQUIRE(server.has_client(2));
    REQUIRE(server.has_client(3));

    const float server_x = server_registry.get<PredictedPosition>(server_entity).x;
    REQUIRE(server_x == Catch::Approx(static_cast<float>(server.frame())));

    for (SmokeClient& smoke : clients) {
        INFO("client_id=" << smoke.id);
        REQUIRE(smoke.client.connection_state() == kage::sync::ReplicationClientConnectionState::Ready);

        const kage::sync::ClientEntityNetworkId network_id = first_allocated_client_entity_network_id(smoke.id);
        const ecs::Entity local = smoke.client.local_entity(network_id);
        REQUIRE(local);
        REQUIRE(smoke.client.entity_mode(network_id) == smoke.mode);
        REQUIRE(smoke.registry.get<PredictedPosition>(local).y == Catch::Approx(10.0f));
    }

    const ecs::Entity predicted_local =
        clients[0].client.local_entity(first_allocated_client_entity_network_id(clients[0].id));
    REQUIRE(clients[0].registry.get<PredictedPosition>(predicted_local).x ==
            Catch::Approx(static_cast<float>(clients[0].client.input_frame())));

    const ecs::Entity snap_local =
        clients[1].client.local_entity(first_allocated_client_entity_network_id(clients[1].id));
    const float snap_x = clients[1].registry.get<PredictedPosition>(snap_local).x;
    REQUIRE(snap_x == Catch::Approx(server_x));

    const ecs::Entity buffered_local =
        clients[2].client.local_entity(first_allocated_client_entity_network_id(clients[2].id));
    REQUIRE(saw_buffered_position);
    REQUIRE(clients[2].client.has_applied_buffered_frame());
    const float buffered_x = clients[2].registry.get<PredictedPosition>(buffered_local).x;
    const float expected_buffered_x = static_cast<float>(clients[2].client.last_applied_buffered_frame());
    REQUIRE(buffered_x > 0.0f);
    REQUIRE(buffered_x <= server_x);
    REQUIRE(buffered_x >= server_x - 8.0f);
    REQUIRE(buffered_x == Catch::Approx(expected_buffered_x).margin(2.0f));
}
