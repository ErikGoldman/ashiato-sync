#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace ashiato_sync_tests;

TEST_CASE("fractional tick sampling samples fractional frames without mutating ECS") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_position =
        ashiato::sync::register_sync_component<SmoothPosition>(server_registry, "SmoothPosition");
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato::sync::define_archetype(
        server_registry,
        "SmoothActor",
        {{server_position, ashiato::sync::ReplicationAudience::All}});
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<SmoothPosition>(server_entity, SmoothPosition{0.0f, 0.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ashiato::Registry client_registry;
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(client_registry, client_position));
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const ashiato::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }

    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server.tick(server_registry, server.options().fixed_dt_seconds);
    const UpdatePacket update = read_update(packets.back(), 1U);
    REQUIRE(update.records.size() == 1);
    const ashiato::sync::ClientEntityNetworkId client_entity_network_id =
        test_client_entity_network_id(1, update.records[0].network_id);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(apply_estimated_server_frame(client, client_registry, 2));
    const ashiato::Entity local = client.local_entity(client_entity_network_id);
    REQUIRE(local);
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 0.0f);

    ashiato::sync::FractionalTickSampleBuffer display;
    REQUIRE(sample_estimated_server_frame(client, client_registry, 2.5, display));
    REQUIRE(display.entities.size() == 1);
    REQUIRE(display.entities[0].client_entity_network_id == client_entity_network_id);
    REQUIRE(display.entities[0].local_entity == local);
    REQUIRE(display.entities[0].frame == 1);
    REQUIRE(display.entities[0].alpha == Catch::Approx(0.5f));

    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get_sampled_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(5.0f));
    REQUIRE(sampled.y == Catch::Approx(0.0f));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 0.0f);
}

TEST_CASE("fractional tick sampling returns floor samples when the next frame is unavailable") {
    ashiato::Registry client_registry;
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(client_registry, client_position));
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));
    const ashiato::Entity server_entity{42};
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{7.0f, 3.0f}}})));

    ashiato::sync::FractionalTickSampleBuffer display;
    REQUIRE(sample_estimated_server_frame(client, client_registry, 2.75, display));
    REQUIRE(display.entities.size() == 1);

    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get_sampled_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(7.0f));
    REQUIRE(sampled.y == Catch::Approx(3.0f));
}

TEST_CASE("fractional tick sampling omits untagged components from samples") {
    ashiato::Registry client_registry;
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));
    const ashiato::Entity server_entity{42};
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{7.0f, 3.0f}}})));

    ashiato::sync::FractionalTickSampleBuffer display;
    REQUIRE(sample_estimated_server_frame(client, client_registry, 2.0, display));
    REQUIRE(display.entities.empty());
}

TEST_CASE("fractional tick samples throw for non-sampled components instead of falling through to ECS") {
    auto define_actor = [](ashiato::Registry& registry, bool display_position) {
        const ashiato::Entity position =
            ashiato::sync::register_sync_component<SmoothPosition>(registry, "SmoothPosition");
        const ashiato::Entity health = ashiato::sync::register_sync_component<Health>(registry, "Health");
        if (display_position) {
            REQUIRE(ashiato::sync::set_fractional_tick_sampled(registry, position));
        }
        return ashiato::sync::define_archetype(
            registry,
            "Actor",
            {
                {position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate},
                {health, ashiato::sync::ReplicationAudience::All},
            });
    };

    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_actor(server_registry, false);
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<SmoothPosition>(server_entity, SmoothPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{100}) != nullptr);

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{0};
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 2);

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_actor(client_registry, true);
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));
    REQUIRE(client.receive(client_registry, packets[0]));
    REQUIRE(client.receive(client_registry, packets[1]));

    ashiato::sync::FractionalTickSampleBuffer display;
    REQUIRE(sample_estimated_server_frame(client, client_registry, 2.5, display));
    REQUIRE(display.entities.size() == 1);

    SmoothPosition sampled_position;
    Health sampled_health;
    REQUIRE(display.entities[0].try_get_sampled_value(client_registry, sampled_position));
    REQUIRE(sampled_position.x == Catch::Approx(5.0f));
    REQUIRE_THROWS_AS(display.entities[0].try_get_sampled_value(client_registry, sampled_health), std::logic_error);

    REQUIRE(sample_estimated_server_frame(client, client_registry, 3.0, display));
    REQUIRE(display.entities.size() == 1);
    REQUIRE_THROWS_AS(display.entities[0].try_get_sampled_value(client_registry, sampled_health), std::logic_error);
}

TEST_CASE("fractional tick sampling steps entity destroy at the floor frame") {
    ashiato::Registry client_registry;
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(client_registry, client_position));
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));
    const ashiato::Entity server_entity{42};
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{7.0f, 3.0f}}})));
    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));

    ashiato::sync::FractionalTickSampleBuffer display;
    REQUIRE(sample_estimated_server_frame(client, client_registry, 2.5, display));
    REQUIRE(display.entities.size() == 1);

    REQUIRE(sample_estimated_server_frame(client, client_registry, 3.0, display));
    REQUIRE(display.entities.empty());
}

TEST_CASE("client-owned fractional tick frame holds instead of rewinding when buffer depth grows") {
    ashiato::Registry client_registry;
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(client_registry, client_position));
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{client_position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}})
        .value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 1;
    options.clock.fixed_dt_seconds = 1.0;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));
    const ashiato::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{20.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(3, {{server_entity, Position{30.0f, 0.0f}}})));

    REQUIRE(client.tick(client_registry, 3.0));
    const ashiato::sync::FractionalTickSampleBuffer& before = client.fractional_tick_frame(client_registry);
    REQUIRE(before.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(before.entities[0].try_get_sampled_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(20.0f));

    REQUIRE(client.set_buffered_frame_lag(3));
    REQUIRE(client.tick(client_registry, 1.0 / 120.0));
    const ashiato::sync::FractionalTickSampleBuffer& after = client.fractional_tick_frame(client_registry);
    REQUIRE(after.entities.size() == 1);
    REQUIRE(after.entities[0].try_get_sampled_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(20.0f));
}

TEST_CASE("client-owned fractional tick frame returns the previous valid sample when target data is missing") {
    ashiato::Registry client_registry;
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(client_registry, client_position));
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{client_position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}})
        .value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 1;
    options.clock.fixed_dt_seconds = 1.0;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));
    const ashiato::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 2.0));
    const ashiato::sync::FractionalTickSampleBuffer& before = client.fractional_tick_frame(client_registry);
    REQUIRE(before.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(before.entities[0].try_get_sampled_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));

    REQUIRE(client.tick(client_registry, 1.0));
    const ashiato::sync::FractionalTickSampleBuffer& after = client.fractional_tick_frame(client_registry);
    REQUIRE(after.entities.size() == 1);
    REQUIRE(after.entities[0].try_get_sampled_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));
}

TEST_CASE("fractional tick sampling freezes buffered entities from latest state when target data is missing") {
    ashiato::Registry client_registry;
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(client_registry, client_position));
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{client_position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}})
        .value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 1;
    options.clock.fixed_dt_seconds = 1.0;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));
    const ashiato::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{10.0f, 0.0f}}})));

    ashiato::sync::FractionalTickSampleBuffer display;
    REQUIRE(sample_estimated_server_frame(client, client_registry, 10.0, display));
    REQUIRE(display.entities.size() == 1);
    REQUIRE(display.entities[0].client_entity_network_id == test_client_entity_network_id(1, server_entity));
    REQUIRE(display.entities[0].frame == 1);
    REQUIRE(display.entities[0].alpha == Catch::Approx(0.0f));

    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get_sampled_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));
    REQUIRE(sampled.y == Catch::Approx(0.0f));
}

TEST_CASE("client-owned fractional tick frame does not freeze destroyed buffered entities") {
    ashiato::Registry client_registry;
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(client_registry, client_position));
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{client_position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}})
        .value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 1;
    options.clock.fixed_dt_seconds = 1.0;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));
    const ashiato::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 2.0));
    REQUIRE(client.fractional_tick_frame(client_registry).entities.size() == 1);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client.tick(client_registry, 1.0));
    REQUIRE(client.fractional_tick_frame(client_registry).entities.empty());
}

TEST_CASE("client-owned fractional tick frame exposes snap and buffered entities in one loop") {
    ashiato::Registry client_registry;
    const ashiato::Entity smooth =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    const ashiato::Entity health = ashiato::sync::register_sync_component<Health>(client_registry, "Health");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(client_registry, smooth));
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "Actor",
                {
                    {smooth, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate},
                    {health, ashiato::sync::ReplicationAudience::All},
                })
        .value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Snap;
    options.buffered.buffered_frame_lag = 1;
    options.clock.fixed_dt_seconds = 1.0;
    options.entities.mode_selector = [](const ashiato::sync::ReplicatedEntityUpdateView& update) {
        return ashiato::sync::client_entity_network_id_wire_id(update.client_entity_network_id) ==
                test_network_id(ashiato::Entity{42})
            ? ashiato::sync::ReplicationClientMode::BufferedInterpolation
            : ashiato::sync::ReplicationClientMode::Snap;
    };
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{ashiato::Entity{42}, Position{10.0f, 0.0f}}}, 3U)));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{ashiato::Entity{43}, Position{20.0f, 0.0f}}}, 3U)));
    REQUIRE(client.tick(client_registry, 2.0));

    const ashiato::sync::FractionalTickSampleBuffer& display = client.fractional_tick_frame(client_registry);
    REQUIRE(display.entities.size() == 2);
    int found = 0;
    for (const ashiato::sync::FractionalTickSample& entity : display.entities) {
        SmoothPosition sampled;
        REQUIRE(entity.try_get_sampled_value(client_registry, sampled));
        if (entity.client_entity_network_id == test_client_entity_network_id(1, test_network_id(ashiato::Entity{42}))) {
            REQUIRE(sampled.x == Catch::Approx(10.0f));
            ++found;
        }
        if (entity.client_entity_network_id == test_client_entity_network_id(1, test_network_id(ashiato::Entity{43}))) {
            REQUIRE(sampled.x == Catch::Approx(20.0f));
            ++found;
        }
    }
    REQUIRE(found == 2);
}

TEST_CASE("client-owned fractional tick frame keeps previous entities while committing newly valid buffered entities") {
    ashiato::Registry client_registry;
    const ashiato::Entity smooth =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(client_registry, smooth));
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{smooth, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}})
        .value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Snap;
    options.buffered.buffered_frame_lag = 1;
    options.clock.fixed_dt_seconds = 1.0;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    const ashiato::Entity existing{42};
    const ashiato::Entity incoming{43};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{existing, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 2.0));
    REQUIRE(client.fractional_tick_frame(client_registry).entities.size() == 1);

    REQUIRE(client.set_default_entity_mode(ashiato::sync::ReplicationClientMode::BufferedInterpolation));
    client.set_entity_mode(
        client_registry,
        test_client_entity_network_id(1, existing),
        ashiato::sync::ReplicationClientMode::BufferedInterpolation);
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{incoming, Position{20.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 1.0));

    const ashiato::sync::FractionalTickSampleBuffer& display = client.fractional_tick_frame(client_registry);
    REQUIRE(display.entities.size() == 2);

    int found = 0;
    for (const ashiato::sync::FractionalTickSample& entity : display.entities) {
        SmoothPosition sampled;
        REQUIRE(entity.try_get_sampled_value(client_registry, sampled));
        if (entity.client_entity_network_id == test_client_entity_network_id(1, test_network_id(existing))) {
            REQUIRE(sampled.x == Catch::Approx(10.0f));
            ++found;
        }
        if (entity.client_entity_network_id == test_client_entity_network_id(1, test_network_id(incoming))) {
            REQUIRE(sampled.x == Catch::Approx(20.0f));
            ++found;
        }
    }
    REQUIRE(found == 2);
}

TEST_CASE("snap fractional tick error blending uses tick dt without mutating ECS") {
    ashiato::Registry client_registry;
    const ashiato::Entity smooth =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(client_registry, smooth));
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{smooth, ashiato::sync::ReplicationAudience::All}})
        .value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Snap;
    options.clock.fixed_dt_seconds = 1.0;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));
    const ashiato::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{0.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 0.5));
    const ashiato::sync::FractionalTickSampleBuffer& first = client.fractional_tick_frame(client_registry);
    REQUIRE(first.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(first.entities[0].try_get_sampled_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(0.0f));

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{10.0f, 0.0f}}})));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.get<SmoothPosition>(local).x == Catch::Approx(10.0f));

    REQUIRE(client.tick(client_registry, 0.25));
    const ashiato::sync::FractionalTickSampleBuffer& blended = client.fractional_tick_frame(client_registry);
    REQUIRE(blended.entities.size() == 1);
    REQUIRE(blended.entities[0].try_get_sampled_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(2.5f));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == Catch::Approx(10.0f));

    const ashiato::sync::FractionalTickSampleBuffer& repeated = client.fractional_tick_frame(client_registry);
    REQUIRE(repeated.entities.size() == 1);
    REQUIRE(repeated.entities[0].try_get_sampled_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(2.5f));
}

TEST_CASE("snap fractional tick error blending clears after the accumulated tick dt consumes the error") {
    ashiato::Registry client_registry;
    const ashiato::Entity smooth =
        ashiato::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(client_registry, smooth));
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{smooth, ashiato::sync::ReplicationAudience::All}})
        .value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Snap;
    options.clock.fixed_dt_seconds = 1.0;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));
    const ashiato::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{0.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 1.0));

    const ashiato::sync::FractionalTickSampleBuffer& display = client.fractional_tick_frame(client_registry);
    REQUIRE(display.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get_sampled_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));
}

TEST_CASE("fractional tick samples throw for unmarked snap components") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Snap;
    options.clock.fixed_dt_seconds = 1.0;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));
    const ashiato::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{0.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 0.25));

    const ashiato::sync::FractionalTickSampleBuffer& display = client.fractional_tick_frame(client_registry);
    REQUIRE(display.entities.size() == 1);
    Position sampled;
    REQUIRE_THROWS_AS(display.entities[0].try_get_sampled_value(client_registry, sampled), std::logic_error);
}

TEST_CASE("predicted client error blends fractional-tick-sampled resim corrections") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    REQUIRE(ashiato::sync::set_fractional_tick_sampled<PredictedPosition>(registry));
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition>(registry, 0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(registry.get<PredictedPosition>(local).x == 3.0f);

    const ashiato::sync::FractionalTickSampleBuffer& display = client.fractional_tick_frame(registry);
    REQUIRE(display.entities.size() == 1);
    PredictedPosition shown;
    REQUIRE(display.entities[0].try_get_sampled_value(registry, shown));
    REQUIRE(shown.x == Catch::Approx(1.2f));
}

TEST_CASE("predicted client fractional tick samples prediction history between fixed ticks") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    REQUIRE(ashiato::sync::set_fractional_tick_sampled<PredictedPosition>(registry));
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition>(registry, 0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds() * 0.5));

    const ashiato::sync::FractionalTickSampleBuffer& display = client.fractional_tick_frame(registry);
    REQUIRE(display.entities.size() == 1);
    REQUIRE(display.entities[0].frame == 2);
    REQUIRE(display.entities[0].alpha >= 0.0f);
    REQUIRE(display.entities[0].alpha < 1.0f);
    PredictedPosition shown;
    REQUIRE(display.entities[0].try_get_sampled_value(registry, shown));
    REQUIRE(shown.x > 1.0f);
    REQUIRE(shown.x < 2.0f);
}

TEST_CASE("predicted fractional tick sample endpoints stay stable across resimulation") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    REQUIRE(ashiato::sync::set_fractional_tick_sampled<PredictedPosition>(registry));

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    options.clock.fixed_dt_seconds = 1.0;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition>(registry, 0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds() * 0.5));

    const ashiato::sync::FractionalTickSampleBuffer& before_resim = client.fractional_tick_frame(registry);
    REQUIRE(before_resim.entities.size() == 1);
    REQUIRE(before_resim.entities[0].source == ashiato::sync::FractionalTickSample::Source::Predicted);
    REQUIRE(before_resim.entities[0].next_frame_present);
    const ashiato::sync::SyncFrame sampled_next_frame = before_resim.entities[0].source_next_frame;
    const std::uint64_t sampled_next_hash = before_resim.entities[0].next_component_hash;

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{20.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));

    const ashiato::sync::FractionalTickSampleBuffer& after_resim = client.fractional_tick_frame(registry);
    REQUIRE(after_resim.entities.size() == 1);
    REQUIRE(after_resim.entities[0].source == ashiato::sync::FractionalTickSample::Source::Predicted);
    REQUIRE(after_resim.entities[0].source_floor_frame == sampled_next_frame);
    REQUIRE(after_resim.entities[0].floor_component_hash == sampled_next_hash);
}

TEST_CASE("predicted client fractional tick sample lags one fixed tick at tick boundaries") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    REQUIRE(ashiato::sync::set_fractional_tick_sampled<PredictedPosition>(registry));
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    options.prediction.auto_lead_frames = false;
    options.prediction.lead_frames = 0;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition>(registry, 0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    tick_client_fixed_frames(client, registry, 2);

    const ashiato::sync::FractionalTickSampleBuffer& display = client.fractional_tick_frame(registry);
    REQUIRE(display.entities.size() == 1);
    REQUIRE(display.entities[0].frame == 2);
    REQUIRE(display.entities[0].alpha == Catch::Approx(0.0f));
    PredictedPosition shown;
    REQUIRE(display.entities[0].try_get_sampled_value(registry, shown));
    REQUIRE(shown.x == Catch::Approx(1.0f));
}

TEST_CASE("predicted client fractional tick sample emits during single-sample warmup") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    REQUIRE(ashiato::sync::set_fractional_tick_sampled<PredictedPosition>(registry));
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{4.0f, 0.0f})));

    const ashiato::sync::FractionalTickSampleBuffer& display = client.fractional_tick_frame(registry);
    REQUIRE(display.entities.size() == 1);
    REQUIRE(display.entities[0].frame == 1);
    REQUIRE(display.entities[0].alpha == Catch::Approx(0.0f));
    PredictedPosition shown;
    REQUIRE(display.entities[0].try_get_sampled_value(registry, shown));
    REQUIRE(shown.x == Catch::Approx(4.0f));
}
