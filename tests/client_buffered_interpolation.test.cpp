#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

TEST_CASE("buffered interpolation delays entity creation until the target frame") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.local_entity(first_allocated_client_entity_network_id(1)));

    REQUIRE_FALSE(client.apply_frame(client_registry, 2));
    REQUIRE_FALSE(client.local_entity(first_allocated_client_entity_network_id(1)));
    REQUIRE(client.apply_frame(client_registry, 3));

    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(client_registry.get<Position>(local).y == 2.0f);
}

TEST_CASE("entity mode selector chooses snap or buffered from decoded component data") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    const ecs::Entity snap_entity{41};
    const ecs::Entity buffered_entity{42};
    int selector_calls = 0;

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.interpolation_buffer_frames = 2;
    options.interpolation_buffer_capacity_frames = 8;
    options.entity_mode_selector = [&](const kage::sync::ReplicatedEntityUpdateView& update) {
        Position position;
        REQUIRE(update.try_get(client_registry, position));
        REQUIRE(update.archetype == client_archetype);
        ++selector_calls;
        return position.x > 10.0f
            ? kage::sync::ReplicationClientMode::BufferedInterpolation
            : kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(
            1,
            {
                {snap_entity, Position{1.0f, 2.0f}},
                {buffered_entity, Position{20.0f, 3.0f}},
            })));

    REQUIRE(selector_calls == 2);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, snap_entity)) == kage::sync::ReplicationClientMode::Snap);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, buffered_entity)) == kage::sync::ReplicationClientMode::BufferedInterpolation);

    const ecs::Entity snap_local = client.local_entity(test_client_entity_network_id(1, snap_entity));
    REQUIRE(snap_local);
    REQUIRE(client_registry.get<Position>(snap_local).x == 1.0f);
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, buffered_entity)));

    REQUIRE(client.apply_frame(client_registry, 3));
    const ecs::Entity buffered_local = client.local_entity(test_client_entity_network_id(1, buffered_entity));
    REQUIRE(buffered_local);
    REQUIRE(client_registry.get<Position>(buffered_local).x == 20.0f);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, buffered_entity)));
    REQUIRE(client.apply_frame(client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(buffered_local));

    REQUIRE(client.receive(client_registry, make_position_packet(4, {{buffered_entity, Position{2.0f, 3.0f}}})));
    REQUIRE(selector_calls == 3);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, buffered_entity, 2U)) == kage::sync::ReplicationClientMode::Snap);
    const ecs::Entity recreated_local = client.local_entity(test_client_entity_network_id(1, buffered_entity, 2U));
    REQUIRE(recreated_local);
    REQUIRE(client_registry.get<Position>(recreated_local).x == 2.0f);
}

TEST_CASE("entity mode selector can inspect synced tag masks") {
    ecs::Registry server_registry;
    const ecs::Entity server_visible = server_registry.register_component<Visible>("Visible");
    const ecs::Entity server_secret = server_registry.register_component<Secret>("Secret");
    const ecs::Entity server_position =
        kage::sync::register_sync_component<Position>(server_registry, "Position");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        kage::sync::SyncArchetypeDesc{
            "TaggedActor",
            {{server_visible, kage::sync::ReplicationAudience::All},
             {server_secret, kage::sync::ReplicationAudience::All}},
            {{server_position, kage::sync::ReplicationAudience::All}},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{4.0f, 8.0f}) != nullptr);
    REQUIRE(server_registry.add_tag(server_entity, server_visible));

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_visible = client_registry.register_component<Visible>("Visible");
    const ecs::Entity client_secret = client_registry.register_component<Secret>("Secret");
    const ecs::Entity client_position =
        kage::sync::register_sync_component<Position>(client_registry, "Position");
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                kage::sync::SyncArchetypeDesc{
                    "TaggedActor",
                    {{client_visible, kage::sync::ReplicationAudience::All},
                     {client_secret, kage::sync::ReplicationAudience::All}},
                    {{client_position, kage::sync::ReplicationAudience::All}},
                }) == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    int selector_calls = 0;
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    options.entity_mode_selector = [&](const kage::sync::ReplicatedEntityUpdateView& update) {
        ++selector_calls;
        REQUIRE(update.has_tag(client_registry, client_visible));
        REQUIRE_FALSE(update.has_tag(client_registry, client_secret));
        REQUIRE_FALSE(update.has_tag(client_registry, client_position));
        return kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(options);

    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(selector_calls == 1);
    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.has(local, client_visible));
    REQUIRE_FALSE(client_registry.has(local, client_secret));
}

TEST_CASE("set entity mode rejects unknown entities") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    REQUIRE_THROWS_AS(client.set_entity_mode(
        client_registry,
        test_client_entity_network_id(1, ecs::Entity{42}),
        kage::sync::ReplicationClientMode::BufferedInterpolation),
        kage::sync::ClientError);
}

TEST_CASE("set entity mode switches snap entities to buffered for future updates") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);

    client.set_entity_mode(
        client_registry,
        test_client_entity_network_id(1, server_entity),
        kage::sync::ReplicationClientMode::BufferedInterpolation);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, server_entity)) == kage::sync::ReplicationClientMode::BufferedInterpolation);

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{5.0f, 2.0f}}})));
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(client.apply_frame(client_registry, 3));
    REQUIRE(client_registry.get<Position>(local).x == 5.0f);
}

TEST_CASE("set entity mode switches buffered entities to snap immediately") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{7.0f, 2.0f}}})));
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    client.set_entity_mode(
        client_registry,
        test_client_entity_network_id(1, server_entity),
        kage::sync::ReplicationClientMode::Snap);
    REQUIRE(client_registry.get<Position>(local).x == 7.0f);
}

TEST_CASE("set entity mode switches buffered entities to predict and seeds prediction") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    registry.job<PredictedPosition>(0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.apply_frame(registry, 2));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{1.0f, 0.0f})));
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);
    client.set_entity_mode(
        registry,
        test_client_entity_network_id(1, server_entity),
        kage::sync::ReplicationClientMode::Predict);
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);

    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 2.0f);
}

TEST_CASE("set entity mode switches an entity by client network id") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClient client;
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f}, 1)));

    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, server_entity)) == kage::sync::ReplicationClientMode::Snap);

    const kage::sync::ClientEntityNetworkId network_id =
        test_client_entity_network_id(1, test_network_id(server_entity));
    client.set_entity_mode(registry, network_id, kage::sync::ReplicationClientMode::Predict);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, server_entity)) == kage::sync::ReplicationClientMode::Predict);

    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
}

TEST_CASE("set entity mode by client network id switches buffered delayed destroys to predict immediately") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    const ecs::Entity server_entity = registry.create();
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{1.0f, 0.0f})));
    REQUIRE(client.apply_frame(registry, 2));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);

    REQUIRE(client.receive(registry, make_destroy_packet(2, server_entity)));
    REQUIRE(registry.alive(local));
    const kage::sync::ClientEntityNetworkId network_id =
        test_client_entity_network_id(1, test_network_id(server_entity));
    REQUIRE(client.set_default_entity_mode(kage::sync::ReplicationClientMode::Predict));
    client.set_entity_mode(registry, network_id, kage::sync::ReplicationClientMode::Predict);
    REQUIRE_FALSE(registry.alive(local));
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, server_entity)) == kage::sync::ReplicationClientMode::Predict);
}

TEST_CASE("set entity mode switches buffered delayed destroys to snap immediately") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client_registry.alive(local));
    client.set_entity_mode(
        client_registry,
        test_client_entity_network_id(1, server_entity),
        kage::sync::ReplicationClientMode::Snap);
    REQUIRE_FALSE(client_registry.alive(local));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, server_entity)));
    REQUIRE_THROWS_AS(client.set_entity_mode(
        client_registry,
        test_client_entity_network_id(1, server_entity),
        kage::sync::ReplicationClientMode::BufferedInterpolation),
        kage::sync::ClientError);
}

TEST_CASE("snap-selected entities do not require interpolation trait hooks") {
    ecs::Registry client_registry;
    const ecs::Entity position = kage::sync::register_sync_component<Position>(client_registry, "Position");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "PositionActor",
        {{position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
}

TEST_CASE("buffered interpolation fills skipped frames with component trait interpolation") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<SmoothPosition>(server_registry, "SmoothPosition");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "SmoothActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<SmoothPosition>(server_entity, SmoothPosition{0.0f, 0.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server.tick(server_registry);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{20.0f, 0.0f};
    server.tick(server_registry);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{30.0f, 0.0f};
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(client.apply_frame(client_registry, 4));
    ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 10.0f);
    REQUIRE(client.apply_frame(client_registry, 5));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 20.0f);
    REQUIRE(client.apply_frame(client_registry, 6));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 30.0f);
}

TEST_CASE("buffered interpolation floors synced tags") {
    ecs::Registry server_registry;
    const ecs::Entity server_visible = server_registry.register_component<Visible>("Visible");
    const ecs::Entity server_secret = server_registry.register_component<Secret>("Secret");
    const ecs::Entity server_position =
        kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        kage::sync::SyncArchetypeDesc{
            "TaggedActor",
            {{server_visible, kage::sync::ReplicationAudience::All},
             {server_secret, kage::sync::ReplicationAudience::All}},
            {{server_position, kage::sync::ReplicationAudience::All}},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(server_entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add_tag(server_entity, server_visible));

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_visible = client_registry.register_component<Visible>("Visible");
    const ecs::Entity client_secret = client_registry.register_component<Secret>("Secret");
    const ecs::Entity client_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                kage::sync::SyncArchetypeDesc{
                    "TaggedActor",
                    {{client_visible, kage::sync::ReplicationAudience::All},
                     {client_secret, kage::sync::ReplicationAudience::All}},
                    {{client_position, kage::sync::ReplicationAudience::All}},
                }) == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    REQUIRE(server_registry.add_tag(server_entity, server_secret));
    packets.clear();
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(client_registry.has(local, client_visible));
    REQUIRE_FALSE(client_registry.has(local, client_secret));

    REQUIRE(client.apply_frame(client_registry, 3));
    REQUIRE(client_registry.has(local, client_visible));
    REQUIRE(client_registry.has(local, client_secret));
}

TEST_CASE("buffered interpolation delays component removal and entity destroy") {
    ecs::Registry server_registry;
    const ecs::Entity server_position = kage::sync::register_sync_component<Position>(server_registry, "Position");
    const ecs::Entity server_health = kage::sync::register_sync_component<Health>(server_registry, "Health");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "Actor",
        {
            {server_position, kage::sync::ReplicationAudience::All},
            {server_health, kage::sync::ReplicationAudience::All},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{7}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position = kage::sync::register_sync_component<Position>(client_registry, "Position");
    const ecs::Entity client_health = kage::sync::register_sync_component<Health>(client_registry, "Health");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "Actor",
        {
            {client_position, kage::sync::ReplicationAudience::All},
            {client_health, kage::sync::ReplicationAudience::All},
        });
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(client_registry.contains<Health>(local));
    for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    packets.clear();
    REQUIRE(server_registry.remove<Health>(server_entity));
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.apply_frame(client_registry, 2));
    REQUIRE(client_registry.contains<Health>(local));
    REQUIRE(client.apply_frame(client_registry, 3));
    REQUIRE_FALSE(client_registry.contains<Health>(local));
    for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    packets.clear();
    REQUIRE(server_registry.destroy(server_entity));
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.apply_frame(client_registry, 3));
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.apply_frame(client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(local));
}

TEST_CASE("buffered interpolation rejects interpolated components without trait hooks") {
    ecs::Registry server_registry;
    const ecs::Entity server_position = kage::sync::register_sync_component<Position>(server_registry, "Position");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);

    ecs::Registry client_registry;
    const ecs::Entity client_position = kage::sync::register_sync_component<Position>(client_registry, "Position");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "PositionActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    REQUIRE_FALSE(client.receive(client_registry, packets.back()));
    REQUIRE(client.pending_ack_count() == 0);
    REQUIRE_FALSE(client.local_entity(first_allocated_client_entity_network_id(1)));
}

TEST_CASE("buffered interpolation keeps step components held between interpolated samples") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<SmoothPosition>(server_registry, "SmoothPosition");
    const ecs::Entity server_health = kage::sync::register_sync_component<Health>(server_registry, "Health");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "MixedActor",
        {
            {server_position, kage::sync::ReplicationAudience::All},
            {server_health, kage::sync::ReplicationAudience::All},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<SmoothPosition>(server_entity, SmoothPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{10}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    const ecs::Entity client_health = kage::sync::register_sync_component<Health>(client_registry, "Health");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "MixedActor",
        {
            {client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate},
            {client_health, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Step},
        });
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }
    REQUIRE(client.apply_frame(client_registry, 3));
    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);

    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{20};
    server.tick(server_registry);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{20.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{30};
    server.tick(server_registry);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{30.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{40};
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(client.apply_frame(client_registry, 4));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 10.0f);
    REQUIRE(client_registry.get<Health>(local).value == 10);
    REQUIRE(client.apply_frame(client_registry, 5));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 20.0f);
    REQUIRE(client_registry.get<Health>(local).value == 10);
    REQUIRE(client.apply_frame(client_registry, 6));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 30.0f);
    REQUIRE(client_registry.get<Health>(local).value == 40);
}

TEST_CASE("buffered interpolation validates wrapped buffer samples by frame") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 0.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        4});

    for (int frame = 1; frame <= 6; ++frame) {
        packets.clear();
        server_registry.write<Position>(server_entity) = Position{static_cast<float>(frame), 0.0f};
        server.tick(server_registry);
        REQUIRE(client.receive(client_registry, packets.back()));
        for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
            REQUIRE(server.process_packet(1, ack));
        }
    }

    REQUIRE_FALSE(client.apply_frame(client_registry, 3));
    REQUIRE_FALSE(client.local_entity(first_allocated_client_entity_network_id(1)));
    REQUIRE(client.apply_frame(client_registry, 7));

    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 6.0f);
}

TEST_CASE("buffered interpolation delays component additions from full updates") {
    ecs::Registry server_registry;
    const ecs::Entity server_position = kage::sync::register_sync_component<Position>(server_registry, "Position");
    const ecs::Entity server_health = kage::sync::register_sync_component<Health>(server_registry, "Health");
    const kage::sync::SyncArchetypeId position_archetype = kage::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const kage::sync::SyncArchetypeId health_archetype = kage::sync::define_archetype(
        server_registry,
        "HealthActor",
        {
            {server_position, kage::sync::ReplicationAudience::All},
            {server_health, kage::sync::ReplicationAudience::All},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, position_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position = kage::sync::register_sync_component<Position>(client_registry, "Position");
    const ecs::Entity client_health = kage::sync::register_sync_component<Health>(client_registry, "Health");
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "PositionActor",
                {{client_position, kage::sync::ReplicationAudience::All}}) == position_archetype);
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "HealthActor",
                {
                    {client_position, kage::sync::ReplicationAudience::All},
                    {client_health, kage::sync::ReplicationAudience::All},
                }) == health_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE_FALSE(client_registry.contains<Health>(local));
    for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    REQUIRE(server_registry.add<Health>(server_entity, Health{7}) != nullptr);
    REQUIRE(start_sync(server_registry, server_entity, health_archetype));
    packets.clear();
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.apply_frame(client_registry, 2));
    REQUIRE_FALSE(client_registry.contains<Health>(local));
    REQUIRE(client.apply_frame(client_registry, 3));
    REQUIRE(client_registry.get<Health>(local).value == 7);
}

TEST_CASE("buffered interpolation delays owner visibility reconciliation") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const ecs::Entity server_health = kage::sync::register_sync_component<Health>(server_registry, "Health");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "OwnedActor",
        {
            {server_position, kage::sync::ReplicationAudience::All},
            {server_health, kage::sync::ReplicationAudience::Owner},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(server_entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{42}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 1));

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId client_id, const ecs::BitBuffer& packet) {
        packets.push_back({client_id, packet});
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_one_registry;
    const ecs::Entity client_one_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_one_registry, "NetworkedPosition");
    const ecs::Entity client_one_health = kage::sync::register_sync_component<Health>(client_one_registry, "Health");
    REQUIRE(kage::sync::define_archetype(
                client_one_registry,
                "OwnedActor",
                {
                    {client_one_position, kage::sync::ReplicationAudience::All},
                    {client_one_health, kage::sync::ReplicationAudience::Owner},
                }) == server_archetype);
    kage::sync::configure_client(client_one_registry, 1);

    ecs::Registry client_two_registry;
    const ecs::Entity client_two_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_two_registry, "NetworkedPosition");
    const ecs::Entity client_two_health = kage::sync::register_sync_component<Health>(client_two_registry, "Health");
    REQUIRE(kage::sync::define_archetype(
                client_two_registry,
                "OwnedActor",
                {
                    {client_two_position, kage::sync::ReplicationAudience::All},
                    {client_two_health, kage::sync::ReplicationAudience::Owner},
                }) == server_archetype);
    kage::sync::configure_client(client_two_registry, 2);

    kage::sync::ReplicationClient client_one(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    kage::sync::ReplicationClient client_two(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});

    server.tick(server_registry);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    REQUIRE(client_one.apply_frame(client_one_registry, 2));
    REQUIRE(client_two.apply_frame(client_two_registry, 2));
    const ecs::Entity client_one_local = client_one.local_entity(first_allocated_client_entity_network_id(1));
    const ecs::Entity client_two_local = client_two.local_entity(first_allocated_client_entity_network_id(2));
    REQUIRE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE_FALSE(client_two_registry.contains<Health>(client_two_local));
    for (const ecs::BitBuffer& ack : client_one.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }
    for (const ecs::BitBuffer& ack : client_two.drain_ack_packets()) {
        REQUIRE(server.process_packet(2, ack));
    }

    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 2));
    packets.clear();
    server.tick(server_registry);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    REQUIRE(client_one.apply_frame(client_one_registry, 2));
    REQUIRE(client_two.apply_frame(client_two_registry, 2));
    REQUIRE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE_FALSE(client_two_registry.contains<Health>(client_two_local));

    REQUIRE(client_one.apply_frame(client_one_registry, 3));
    REQUIRE(client_two.apply_frame(client_two_registry, 3));
    REQUIRE_FALSE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE(client_two_registry.get<Health>(client_two_local).value == 42);
}

TEST_CASE("buffered interpolation applies valid entities when another target sample is missing") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});

    const ecs::Entity first{101};
    const ecs::Entity second{102};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{first, Position{1.0f, 0.0f}}, {second, Position{2.0f, 0.0f}}})));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{first, Position{3.0f, 0.0f}}})));

    REQUIRE_FALSE(client.apply_frame(client_registry, 3));
    REQUIRE(client.local_entity(test_client_entity_network_id(1, first)));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, second)));
    REQUIRE(client_registry.get<Position>(client.local_entity(test_client_entity_network_id(1, first))).x == 3.0f);
}

#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS

TEST_CASE("buffered interpolation diagnostics count missing target samples") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});

    const ecs::Entity first{101};
    const ecs::Entity second{102};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{first, Position{1.0f, 0.0f}}, {second, Position{2.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{first, Position{3.0f, 0.0f}}})));

    REQUIRE_FALSE(client.apply_frame(client_registry, 3));
    const kage::sync::ReplicationClientInterpolationDiagnostics& diagnostics = client.interpolation_diagnostics();
    REQUIRE(diagnostics.total_interpolated_entity_frame_checks == 2);
    REQUIRE(diagnostics.total_interpolated_entity_frame_starvations == 1);
    REQUIRE(diagnostics.window_interpolated_entity_frame_checks == 2);
    REQUIRE(diagnostics.window_interpolated_entity_frame_starvations == 1);
    REQUIRE(diagnostics.interpolated_entity_starvation_rate() == Catch::Approx(0.5f));
    REQUIRE(diagnostics.interpolated_entity_starvation_percent() == Catch::Approx(50.0f));
    REQUIRE(diagnostics.lifetime_interpolated_entity_starvation_rate() == Catch::Approx(0.5f));

    for (std::size_t offset = 0; offset < kage::sync::interpolation_diagnostics_window_frames; ++offset) {
        const kage::sync::SyncFrame frame = static_cast<kage::sync::SyncFrame>(3U + offset);
        REQUIRE(client.receive(client_registry, make_position_packet(
            frame,
            {{first, Position{static_cast<float>(frame), 0.0f}}, {second, Position{static_cast<float>(frame), 0.0f}}})));
        REQUIRE(client.apply_frame(client_registry, static_cast<kage::sync::SyncFrame>(frame + 1U)));
    }

    REQUIRE(diagnostics.total_interpolated_entity_frame_starvations == 1);
    REQUIRE(diagnostics.window_interpolated_entity_frame_checks == kage::sync::interpolation_diagnostics_window_frames * 2U);
    REQUIRE(diagnostics.window_interpolated_entity_frame_starvations == 0);
    REQUIRE(diagnostics.interpolated_entity_starvation_percent() == Catch::Approx(0.0f));
    REQUIRE(diagnostics.lifetime_interpolated_entity_starvation_percent() > 0.0f);

    client.reset_interpolation_diagnostics();
    REQUIRE(client.interpolation_diagnostics().total_interpolated_entity_frame_checks == 0);
    REQUIRE(client.interpolation_diagnostics().total_interpolated_entity_frame_starvations == 0);
    REQUIRE(client.interpolation_diagnostics().window_interpolated_entity_frame_checks == 0);
    REQUIRE(client.interpolation_diagnostics().window_interpolated_entity_frame_starvations == 0);
}
#endif

TEST_CASE("buffered interpolation validates client buffer options") {
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            0}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            3}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            4,
            4}),
        std::invalid_argument);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        4});
    REQUIRE(client.set_interpolation_buffer_frames(3));
    REQUIRE_FALSE(client.set_interpolation_buffer_frames(4));
    REQUIRE(client.current_interpolation_buffer_frames() == 3);

    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            4}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            1,
            -1.0f}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            1,
            2.0f,
            0.0f}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            1,
            2.0f,
            0.1f,
            0.0f}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            1,
            2.0f,
            0.1f,
            0.95f,
            0.5f}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            1,
            2.0f,
            0.1f,
            0.95f,
            1.05f,
            -0.1f}),
        std::invalid_argument);
}

TEST_CASE("buffered interpolation applies correct target frame after auto buffer depth changes") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8,
        true,
        1,
        4.0f,
        0.5f,
        0.90f,
        1.10f,
        0.10f});

    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);
    REQUIRE(client.receive(client_registry, make_position_packet(6, {{server_entity, Position{6.0f, 2.0f}}}), 6));
    REQUIRE(client.current_interpolation_buffer_frames() == 2);

    REQUIRE(client.apply_frame(client_registry, 7));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(client.apply_frame(client_registry, 8));
    REQUIRE(client_registry.get<Position>(local).x == 6.0f);

    REQUIRE(client.receive(client_registry, make_destroy_packet(7, server_entity), 7));
    REQUIRE(client.current_interpolation_buffer_frames() == 3);
    REQUIRE(client.apply_frame(client_registry, 9));
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.apply_frame(client_registry, 10));
    REQUIRE_FALSE(client_registry.alive(local));
}

TEST_CASE("buffered interpolation does not recreate destroyed entities before the new target frame") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.apply_frame(client_registry, 3));
    const ecs::Entity first_local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(first_local);
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client.apply_frame(client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(first_local));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(4, {{server_entity, Position{5.0f, 6.0f}}})));

    REQUIRE(client.apply_frame(client_registry, 5));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, server_entity)));
    REQUIRE(client.apply_frame(client_registry, 6));
    const ecs::Entity second_local = client.local_entity(test_client_entity_network_id(1, server_entity, 2U));
    REQUIRE(second_local);
    REQUIRE(second_local != first_local);
    REQUIRE(client_registry.get<Position>(second_local).x == 5.0f);
    REQUIRE(client_registry.get<Position>(second_local).y == 6.0f);
}

TEST_CASE("buffered interpolation applies late destroys for already sampled target frames") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.apply_frame(client_registry, 3));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE_FALSE(client.apply_frame(client_registry, 4));

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE_FALSE(client_registry.alive(local));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, server_entity)));

    const std::vector<ecs::BitBuffer> acks = client.drain_ack_packets();
    REQUIRE(acks.size() == 1);
    const std::vector<AckRecord> records = read_acks(acks[0]);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].packet_id == 2);
}

TEST_CASE("buffered interpolation keeps client entity network ids alive until destroy frame applies") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    const ecs::Entity server_entity{42};
    const kage::sync::ClientEntityNetworkId client_entity_network_id =
        test_client_entity_network_id(1, test_network_id(server_entity), 1U);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.apply_frame(client_registry, 3));
    const ecs::Entity local = client.local_entity(client_entity_network_id);
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.is_alive_network_id(client_entity_network_id));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.local_entity(client_entity_network_id) == local);
    REQUIRE(client.is_alive_network_id(client_entity_network_id));

    REQUIRE(client.apply_frame(client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(local));
    REQUIRE_FALSE(client.is_alive_network_id(client_entity_network_id));
    REQUIRE(client.drain_ack_packets().size() == 1);
}
