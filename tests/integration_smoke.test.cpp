#include "test_protocol.hpp"

#include "kage/sync/simulated_link.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

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
    kage::sync::ReplicationClient client{registry};
};

struct LifecycleEntity {
    ecs::Entity entity;
    int remaining_ticks = 0;
};

std::size_t predicted_position_count(ecs::Registry& registry) {
    std::size_t count = 0;
    registry.view<const PredictedPosition>().each([&](ecs::Entity, const PredictedPosition&) {
        ++count;
    });
    return count;
}

kage::sync::SyncArchetypeId define_sampled_predicted_archetype(ecs::Registry& registry) {
    const ecs::Entity position =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    kage::sync::set_fractional_tick_sampled(registry, position);
    return kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position, kage::sync::ReplicationAudience::All}});
}

std::size_t displayed_predicted_position_count(
    kage::sync::ReplicationClient& client,
    ecs::Registry& registry) {
    std::size_t count = 0;
    for (const kage::sync::FractionalTickSample& sample : client.fractional_tick_frame(registry).entities) {
        PredictedPosition position;
        if (sample.try_get_sampled_value(registry, position)) {
            ++count;
        }
    }
    return count;
}

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
    kage::sync::ReplicationServer server(server_registry, server_options);

    std::array<SmokeClient, 3> clients;
    auto setup_client = [&](std::size_t index,
                            kage::sync::ClientId id,
                            kage::sync::ClientId peer,
                            const char* token,
                            kage::sync::ReplicationClientMode mode) {
        kage::sync::ReplicationClientOptions options;
        options.session.connect_token = token;
        options.entities.default_mode = mode;
        options.buffered.auto_buffered_frame_lag = false;
        options.buffered.buffered_frame_lag = 2;
        options.prediction.auto_lead_frames = false;
        options.prediction.lead_frames = 2;

        SmokeClient& smoke = clients[index];
        smoke.id = id;
        smoke.peer = peer;
        smoke.mode = mode;
        const kage::sync::SyncArchetypeId client_archetype = define_predicted_archetype(smoke.registry);
        REQUIRE(client_archetype == server_archetype);
        smoke.client = kage::sync::ReplicationClient(smoke.registry, options);
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
            REQUIRE(smoke.client.tick(smoke.registry, smoke.client.fixed_dt_seconds()));
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
                REQUIRE(receive_at_local_frame(found->client, found->registry, packet, server_frame));
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
            Catch::Approx(static_cast<float>(clients[0].client.predicted_frame())));

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

TEST_CASE("lifecycle churn stays bounded while switching buffered and predicted under latency spikes") {
    using Link = kage::sync::SimulatedLink<ecs::BitBuffer, kage::sync::ClientId>;

    constexpr std::size_t target_entities = 100;
    constexpr int warmup_ticks = 180;
    constexpr int total_ticks = 960;
    constexpr double tick_dt = 1.0 / 60.0;
    constexpr std::size_t min_client_entities = 70;
    constexpr std::size_t max_client_entities = 130;

    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_predicted_archetype(server_registry);
    kage_sync_tests::configure_test_server_registry(server_registry);

    std::mt19937 rng(1701);
    std::uniform_int_distribution<int> lifetime_ticks(90, 240);
    std::vector<LifecycleEntity> server_entities;
    server_entities.reserve(target_entities);
    std::uint32_t spawn_index = 0;
    auto spawn_until_target = [&]() {
        while (server_entities.size() < target_entities) {
            const ecs::Entity entity = server_registry.create();
            REQUIRE(server_registry.add<PredictedPosition>(
                        entity,
                        PredictedPosition{
                            static_cast<float>(spawn_index % 17U),
                            static_cast<float>((spawn_index / 17U) % 17U)})
                    != nullptr);
            REQUIRE(start_sync(server_registry, entity, server_archetype));
            server_entities.push_back(LifecycleEntity{entity, lifetime_ticks(rng)});
            ++spawn_index;
        }
    };
    auto update_server_world = [&]() {
        for (LifecycleEntity& entry : server_entities) {
            PredictedPosition& position = server_registry.write<PredictedPosition>(entry.entity);
            position.x += 0.05f;
            position.y += 0.025f;
            --entry.remaining_ticks;
        }
        server_entities.erase(
            std::remove_if(
                server_entities.begin(),
                server_entities.end(),
                [&](const LifecycleEntity& entry) {
                    if (entry.remaining_ticks > 0) {
                        return false;
                    }
                    REQUIRE(server_registry.destroy(entry.entity));
                    return true;
                }),
            server_entities.end());
        spawn_until_target();
    };
    spawn_until_target();

    double now_seconds = 0.0;
    Link upstream({35.0, 0.0, 0.0}, 41U);
    Link downstream({35.0, 0.0, 0.0}, 42U);

    kage::sync::ReplicationServerOptions server_options;
    server_options.fixed_dt_seconds = tick_dt;
    server_options.bandwidth_limit_bytes_per_tick = 1024U * 1024U;
    server_options.transport = [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
        REQUIRE(downstream.enqueue(peer, packet, now_seconds));
    };
    kage::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    ecs::Registry client_registry;
    kage_sync_tests::configure_test_client_registry(client_registry, 1);
    const kage::sync::SyncArchetypeId client_archetype = define_sampled_predicted_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);

    std::vector<kage::sync::ClientEntityNetworkId> known_entities;
    known_entities.reserve(target_entities * 2U);
    kage::sync::ReplicationClientMode client_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    kage::sync::ReplicationClientOptions client_options;
    client_options.clock.fixed_dt_seconds = tick_dt;
    client_options.entities.default_mode = client_mode;
    client_options.buffered.auto_buffered_frame_lag = false;
    client_options.buffered.buffered_frame_lag = 3;
    client_options.prediction.auto_lead_frames = false;
    client_options.prediction.lead_frames = 3;
    client_options.prediction.rollback_policy = kage::sync::ReplicationRollbackPolicy::OnlyAffected;
    client_options.entities.mode_selector = [&](const kage::sync::ReplicatedEntityUpdateView& update) {
        if (std::find(known_entities.begin(), known_entities.end(), update.client_entity_network_id) ==
            known_entities.end()) {
            known_entities.push_back(update.client_entity_network_id);
        }
        return client_mode;
    };
    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, client_options));
    client.simulation_job<PredictedPosition>(client_registry, 0).each(
        [](ecs::Entity, PredictedPosition& position) {
            position.x += 0.05f;
            position.y += 0.025f;
        });
    client.set_packet_sender([&](const ecs::BitBuffer& packet) {
        REQUIRE(upstream.enqueue(1, packet, now_seconds));
    });

    auto switch_client_mode = [&](kage::sync::ReplicationClientMode mode) {
        client_mode = mode;
        REQUIRE(client.set_default_entity_mode(mode));
        for (const kage::sync::ClientEntityNetworkId network_id : known_entities) {
            try {
                client.set_entity_mode(client_registry, network_id, mode);
            } catch (const kage::sync::ClientError& error) {
                if (error.status() != kage::sync::ClientStatus::EntityNotFound &&
                    error.status() != kage::sync::ClientStatus::EntityUnavailable) {
                    throw;
                }
            }
        }
        known_entities.erase(
            std::remove_if(
                known_entities.begin(),
                known_entities.end(),
                [&](kage::sync::ClientEntityNetworkId network_id) {
                    return !client.has_entity(network_id);
                }),
            known_entities.end());
    };

    std::size_t min_seen = std::numeric_limits<std::size_t>::max();
    std::size_t max_seen = 0;
    for (int tick = 0; tick < total_ticks; ++tick) {
        if (tick == 240 || tick == 600) {
            switch_client_mode(kage::sync::ReplicationClientMode::Predict);
        } else if (tick == 420 || tick == 780) {
            switch_client_mode(kage::sync::ReplicationClientMode::BufferedInterpolation);
        }

        if ((tick >= 300 && tick < 390) || (tick >= 660 && tick < 750)) {
            upstream.settings.latency_ms = 450.0;
            downstream.settings.latency_ms = 450.0;
        } else {
            upstream.settings.latency_ms = 35.0;
            downstream.settings.latency_ms = 35.0;
        }

        downstream.deliver_ready(now_seconds, [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
            REQUIRE(peer == 1);
            client.receive_packet(packet);
        });

        REQUIRE(client.tick(client_registry, tick_dt));

        upstream.deliver_ready(now_seconds, [&](kage::sync::ClientId peer, const ecs::BitBuffer& packet) {
            REQUIRE(peer == 1);
            server.receive_packet(peer, packet);
        });

        update_server_world();
        REQUIRE(server.tick(server_registry, tick_dt));
        REQUIRE(server_entities.size() == target_entities);

        const std::size_t client_entities = predicted_position_count(client_registry);
        const std::size_t displayed_entities = displayed_predicted_position_count(client, client_registry);
        if (tick >= warmup_ticks) {
            CAPTURE(tick, client_entities, displayed_entities, client_mode, upstream.settings.latency_ms);
            REQUIRE(client_entities >= min_client_entities);
            REQUIRE(client_entities <= max_client_entities);
            REQUIRE(displayed_entities >= min_client_entities);
            REQUIRE(displayed_entities <= max_client_entities);
            min_seen = std::min(min_seen, client_entities);
            max_seen = std::max(max_seen, client_entities);
        }

        now_seconds += tick_dt;
    }

    REQUIRE(min_seen <= target_entities);
    REQUIRE(max_seen >= target_entities);
}
