#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

TEST_CASE("display interpolation samples fractional frames without mutating ECS") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<SmoothPosition>(server_registry, "SmoothPosition");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "SmoothActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<SmoothPosition>(server_entity, SmoothPosition{0.0f, 0.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, client_position));
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server.tick(server_registry);
    const UpdatePacket update = read_update(packets.back(), 1U);
    REQUIRE(update.records.size() == 1);
    const kage::sync::ClientEntityNetworkId client_entity_network_id =
        test_client_entity_network_id(1, update.records[0].network_id);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 0.0f);

    kage::sync::DisplayInterpolationSampleBuffer display;
    REQUIRE(client.sample_display_interpolation_frame(client_registry, 2.5, display));
    REQUIRE(display.entities.size() == 1);
    REQUIRE(display.entities[0].client_entity_network_id == client_entity_network_id);
    REQUIRE(display.entities[0].local_entity == local);
    REQUIRE(display.entities[0].frame == 1);
    REQUIRE(display.entities[0].alpha == Catch::Approx(0.5f));

    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(5.0f));
    REQUIRE(sampled.y == Catch::Approx(0.0f));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 0.0f);
}

TEST_CASE("display interpolation returns floor samples when the next frame is unavailable") {
    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, client_position));
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    const ecs::Entity server_entity{42};
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{7.0f, 3.0f}}})));

    kage::sync::DisplayInterpolationSampleBuffer display;
    REQUIRE(client.sample_display_interpolation_frame(client_registry, 2.75, display));
    REQUIRE(display.entities.size() == 1);

    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(7.0f));
    REQUIRE(sampled.y == Catch::Approx(3.0f));
}

TEST_CASE("display interpolation omits untagged components from samples") {
    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    const ecs::Entity server_entity{42};
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{7.0f, 3.0f}}})));

    kage::sync::DisplayInterpolationSampleBuffer display;
    REQUIRE(client.sample_display_interpolation_frame(client_registry, 2.0, display));
    REQUIRE(display.entities.empty());
}

TEST_CASE("display samples throw for non-display components instead of falling through to ECS") {
    auto define_actor = [](ecs::Registry& registry, bool display_position) {
        const ecs::Entity position =
            kage::sync::register_sync_component<SmoothPosition>(registry, "SmoothPosition");
        const ecs::Entity health = kage::sync::register_sync_component<Health>(registry, "Health");
        if (display_position) {
            REQUIRE(kage::sync::set_display_interpolated(registry, position));
        }
        return kage::sync::define_archetype(
            registry,
            "Actor",
            {
                {position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate},
                {health, kage::sync::ReplicationAudience::All},
            });
    };

    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_actor(server_registry, false);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<SmoothPosition>(server_entity, SmoothPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{100}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{0};
    server.tick(server_registry);
    REQUIRE(packets.size() == 2);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_actor(client_registry, true);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    REQUIRE(client.receive(client_registry, packets[0]));
    REQUIRE(client.receive(client_registry, packets[1]));

    kage::sync::DisplayInterpolationSampleBuffer display;
    REQUIRE(client.sample_display_interpolation_frame(client_registry, 2.5, display));
    REQUIRE(display.entities.size() == 1);

    SmoothPosition sampled_position;
    Health sampled_health;
    REQUIRE(display.entities[0].try_get_display_value(client_registry, sampled_position));
    REQUIRE(sampled_position.x == Catch::Approx(5.0f));
    REQUIRE_THROWS_AS(display.entities[0].try_get_display_value(client_registry, sampled_health), std::logic_error);

    REQUIRE(client.sample_display_interpolation_frame(client_registry, 3.0, display));
    REQUIRE(display.entities.size() == 1);
    REQUIRE_THROWS_AS(display.entities[0].try_get_display_value(client_registry, sampled_health), std::logic_error);
}

TEST_CASE("display interpolation steps entity destroy at the floor frame") {
    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, client_position));
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    const ecs::Entity server_entity{42};
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{7.0f, 3.0f}}})));
    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));

    kage::sync::DisplayInterpolationSampleBuffer display;
    REQUIRE(client.sample_display_interpolation_frame(client_registry, 2.5, display));
    REQUIRE(display.entities.size() == 1);

    REQUIRE(client.sample_display_interpolation_frame(client_registry, 3.0, display));
    REQUIRE(display.entities.empty());
}

TEST_CASE("client-owned display frame holds instead of rewinding when buffer depth grows") {
    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, client_position));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}})
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{20.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(3, {{server_entity, Position{30.0f, 0.0f}}})));

    REQUIRE(client.tick(client_registry, 3.0));
    const kage::sync::DisplayInterpolationSampleBuffer& before = client.display_interpolation_frame(client_registry);
    REQUIRE(before.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(before.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(21.5f));

    REQUIRE(client.set_interpolation_buffer_frames(3));
    REQUIRE(client.tick(client_registry, 1.0 / 120.0));
    const kage::sync::DisplayInterpolationSampleBuffer& after = client.display_interpolation_frame(client_registry);
    REQUIRE(after.entities.size() == 1);
    REQUIRE(after.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(21.5f));
}

TEST_CASE("client-owned display frame returns the previous valid sample when target data is missing") {
    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, client_position));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}})
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 2.0));
    const kage::sync::DisplayInterpolationSampleBuffer& before = client.display_interpolation_frame(client_registry);
    REQUIRE(before.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(before.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));

    REQUIRE(client.tick(client_registry, 1.0));
    const kage::sync::DisplayInterpolationSampleBuffer& after = client.display_interpolation_frame(client_registry);
    REQUIRE(after.entities.size() == 1);
    REQUIRE(after.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));
}

TEST_CASE("client-owned display frame exposes snap and buffered entities in one loop") {
    ecs::Registry client_registry;
    const ecs::Entity smooth =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    const ecs::Entity health = kage::sync::register_sync_component<Health>(client_registry, "Health");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, smooth));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "Actor",
                {
                    {smooth, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate},
                    {health, kage::sync::ReplicationAudience::All},
                })
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    options.fixed_dt_seconds = 1.0;
    options.entity_mode_selector = [](const kage::sync::ReplicatedEntityUpdateView& update) {
        return kage::sync::client_entity_network_id_wire_id(update.client_entity_network_id) ==
                test_network_id(ecs::Entity{42})
            ? kage::sync::ReplicationClientMode::BufferedInterpolation
            : kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{ecs::Entity{42}, Position{10.0f, 0.0f}}}, 3U)));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{ecs::Entity{43}, Position{20.0f, 0.0f}}}, 3U)));
    REQUIRE(client.tick(client_registry, 2.0));

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(client_registry);
    REQUIRE(display.entities.size() == 2);
    int found = 0;
    for (const kage::sync::DisplayInterpolationSample& entity : display.entities) {
        SmoothPosition sampled;
        REQUIRE(entity.try_get_display_value(client_registry, sampled));
        if (entity.client_entity_network_id == test_client_entity_network_id(1, test_network_id(ecs::Entity{42}))) {
            REQUIRE(sampled.x == Catch::Approx(10.0f));
            ++found;
        }
        if (entity.client_entity_network_id == test_client_entity_network_id(1, test_network_id(ecs::Entity{43}))) {
            REQUIRE(sampled.x == Catch::Approx(20.0f));
            ++found;
        }
    }
    REQUIRE(found == 2);
}

TEST_CASE("client-owned display frame keeps previous entities while committing newly valid buffered entities") {
    ecs::Registry client_registry;
    const ecs::Entity smooth =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, smooth));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{smooth, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}})
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);

    const ecs::Entity existing{42};
    const ecs::Entity incoming{43};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{existing, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 2.0));
    REQUIRE(client.display_interpolation_frame(client_registry).entities.size() == 1);

    REQUIRE(client.set_default_entity_mode(kage::sync::ReplicationClientMode::BufferedInterpolation));
    REQUIRE(client.set_entity_mode(client_registry, existing, kage::sync::ReplicationClientMode::BufferedInterpolation));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{incoming, Position{20.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 1.0));

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(client_registry);
    REQUIRE(display.entities.size() == 2);

    int found = 0;
    for (const kage::sync::DisplayInterpolationSample& entity : display.entities) {
        SmoothPosition sampled;
        REQUIRE(entity.try_get_display_value(client_registry, sampled));
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

TEST_CASE("snap display error blending uses tick dt without mutating ECS") {
    ecs::Registry client_registry;
    const ecs::Entity smooth =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, smooth));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{smooth, kage::sync::ReplicationAudience::All}})
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{0.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 0.5));
    const kage::sync::DisplayInterpolationSampleBuffer& first = client.display_interpolation_frame(client_registry);
    REQUIRE(first.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(first.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(0.0f));

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{10.0f, 0.0f}}})));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<SmoothPosition>(local).x == Catch::Approx(10.0f));

    REQUIRE(client.tick(client_registry, 0.25));
    const kage::sync::DisplayInterpolationSampleBuffer& blended = client.display_interpolation_frame(client_registry);
    REQUIRE(blended.entities.size() == 1);
    REQUIRE(blended.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(2.5f));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == Catch::Approx(10.0f));

    const kage::sync::DisplayInterpolationSampleBuffer& repeated = client.display_interpolation_frame(client_registry);
    REQUIRE(repeated.entities.size() == 1);
    REQUIRE(repeated.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(2.5f));
}

TEST_CASE("snap display error blending clears after the accumulated tick dt consumes the error") {
    ecs::Registry client_registry;
    const ecs::Entity smooth =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, smooth));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{smooth, kage::sync::ReplicationAudience::All}})
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{0.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 1.0));

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(client_registry);
    REQUIRE(display.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));
}

TEST_CASE("display samples throw for unmarked snap components") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{0.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 0.25));

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(client_registry);
    REQUIRE(display.entities.size() == 1);
    Position sampled;
    REQUIRE_THROWS_AS(display.entities[0].try_get_display_value(client_registry, sampled), std::logic_error);
}

TEST_CASE("predicted client error blends display-interpolated resim corrections") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    REQUIRE(kage::sync::set_display_interpolated<PredictedPosition>(registry));
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 3.0f);

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(registry);
    REQUIRE(display.entities.size() == 1);
    PredictedPosition shown;
    REQUIRE(display.entities[0].try_get_display_value(registry, shown));
    REQUIRE(shown.x == Catch::Approx(2.2f));
}

TEST_CASE("predicted client display samples prediction history between fixed ticks") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    REQUIRE(kage::sync::set_display_interpolated<PredictedPosition>(registry));
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds * 0.5));

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(registry);
    REQUIRE(display.entities.size() == 1);
    PredictedPosition shown;
    REQUIRE(display.entities[0].try_get_display_value(registry, shown));
    REQUIRE(shown.x == Catch::Approx(2.7f));
}
