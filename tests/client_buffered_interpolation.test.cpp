#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace ashiato_sync_tests;

TEST_CASE("buffered interpolation delays entity creation until the target frame") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato_sync_tests::define_position_archetype(server_registry);
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    using SmallBufferedClient = ashiato::sync::ReplicationClientT<
        ashiato::sync::protocol::default_network_entity_id_tier0_bits,
        4,
        64>;
    SmallBufferedClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{2}}));
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.local_entity(first_allocated_client_entity_network_id(1)));

    REQUIRE_FALSE(apply_estimated_server_frame(client, client_registry, 2));
    REQUIRE_FALSE(client.local_entity(first_allocated_client_entity_network_id(1)));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));

    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(client_registry.get<Position>(local).y == 2.0f);
}

TEST_CASE("entity mode selector chooses snap or buffered from decoded component data") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(client_registry));

    const ashiato::Entity snap_entity{41};
    const ashiato::Entity buffered_entity{42};
    int selector_calls = 0;

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Snap;
    options.buffered.buffered_frame_lag = 2;
    options.entities.mode_selector = [&](const ashiato::sync::ReplicatedEntityUpdateView& update) {
        Position position;
        REQUIRE(update.try_get(client_registry, position));
        REQUIRE(update.archetype == client_archetype);
        ++selector_calls;
        return position.x > 10.0f
            ? ashiato::sync::ReplicationClientMode::BufferedInterpolation
            : ashiato::sync::ReplicationClientMode::Snap;
    };
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(
            1,
            {
                {snap_entity, Position{1.0f, 2.0f}},
                {buffered_entity, Position{20.0f, 3.0f}},
            })));

    REQUIRE(selector_calls == 2);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, snap_entity)) == ashiato::sync::ReplicationClientMode::Snap);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, buffered_entity)) == ashiato::sync::ReplicationClientMode::BufferedInterpolation);

    const ashiato::Entity snap_local = client.local_entity(test_client_entity_network_id(1, snap_entity));
    REQUIRE(snap_local);
    REQUIRE(client_registry.get<Position>(snap_local).x == 1.0f);
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, buffered_entity)));

    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));
    const ashiato::Entity buffered_local = client.local_entity(test_client_entity_network_id(1, buffered_entity));
    REQUIRE(buffered_local);
    REQUIRE(client_registry.get<Position>(buffered_local).x == 20.0f);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, buffered_entity)));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(buffered_local));

    REQUIRE(client.receive(client_registry, make_position_packet(4, {{buffered_entity, Position{2.0f, 3.0f}}})));
    REQUIRE(selector_calls == 3);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, buffered_entity, 2U)) == ashiato::sync::ReplicationClientMode::Snap);
    const ashiato::Entity recreated_local = client.local_entity(test_client_entity_network_id(1, buffered_entity, 2U));
    REQUIRE(recreated_local);
    REQUIRE(client_registry.get<Position>(recreated_local).x == 2.0f);
}

TEST_CASE("entity mode selector can inspect synced tag masks") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_visible = server_registry.register_component<Visible>("Visible");
    const ashiato::Entity server_secret = server_registry.register_component<Secret>("Secret");
    const ashiato::Entity server_position =
        ashiato::sync::register_sync_component<Position>(server_registry, "Position");
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato::sync::define_archetype(
        server_registry,
        ashiato::sync::SyncArchetypeDesc{
            "TaggedActor",
            {{server_visible, ashiato::sync::ReplicationAudience::All},
             {server_secret, ashiato::sync::ReplicationAudience::All}},
            {{server_position, ashiato::sync::ReplicationAudience::All}},
        });
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{4.0f, 8.0f}) != nullptr);
    REQUIRE(server_registry.add_tag(server_entity, server_visible));

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ashiato::Registry client_registry;
    const ashiato::Entity client_visible = client_registry.register_component<Visible>("Visible");
    const ashiato::Entity client_secret = client_registry.register_component<Secret>("Secret");
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<Position>(client_registry, "Position");
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                ashiato::sync::SyncArchetypeDesc{
                    "TaggedActor",
                    {{client_visible, ashiato::sync::ReplicationAudience::All},
                     {client_secret, ashiato::sync::ReplicationAudience::All}},
                    {{client_position, ashiato::sync::ReplicationAudience::All}},
                }) == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    int selector_calls = 0;
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Snap;
    options.buffered.buffered_frame_lag = 1;
    options.entities.mode_selector = [&](const ashiato::sync::ReplicatedEntityUpdateView& update) {
        ++selector_calls;
        REQUIRE(update.has_tag(client_registry, client_visible));
        REQUIRE_FALSE(update.has_tag(client_registry, client_secret));
        REQUIRE_FALSE(update.has_tag(client_registry, client_position));
        return ashiato::sync::ReplicationClientMode::Snap;
    };
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(selector_calls == 1);
    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.has(local, client_visible));
    REQUIRE_FALSE(client_registry.has(local, client_secret));
}

TEST_CASE("set entity mode rejects unknown entities") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    REQUIRE_THROWS_AS(client.set_entity_mode(
        client_registry,
        test_client_entity_network_id(1, ashiato::Entity{42}),
        ashiato::sync::ReplicationClientMode::BufferedInterpolation),
        ashiato::sync::ClientError);
}

TEST_CASE("set entity mode switches snap entities to buffered for future updates") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    const ashiato::Entity server_entity{42};

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Snap;
    options.buffered.buffered_frame_lag = 1;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);

    client.set_entity_mode(
        client_registry,
        test_client_entity_network_id(1, server_entity),
        ashiato::sync::ReplicationClientMode::BufferedInterpolation);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, server_entity)) == ashiato::sync::ReplicationClientMode::BufferedInterpolation);

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{5.0f, 2.0f}}})));
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));
    REQUIRE(client_registry.get<Position>(local).x == 5.0f);
}

TEST_CASE("set entity mode switches buffered entities to snap immediately") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    const ashiato::Entity server_entity{42};

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 1;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 2));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{7.0f, 2.0f}}})));
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    client.set_entity_mode(
        client_registry,
        test_client_entity_network_id(1, server_entity),
        ashiato::sync::ReplicationClientMode::Snap);
    REQUIRE(client_registry.get<Position>(local).x == 7.0f);
}

TEST_CASE("set entity mode switches buffered entities to predict and seeds prediction") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    registry.job<PredictedPosition>(0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ashiato::Entity server_entity = registry.create();
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 1;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));

    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(apply_estimated_server_frame(client, registry, 2));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{1.0f, 0.0f})));
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);
    client.set_entity_mode(
        registry,
        test_client_entity_network_id(1, server_entity),
        ashiato::sync::ReplicationClientMode::Predict);
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);

    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(registry.get<PredictedPosition>(local).x == 2.0f);
}

TEST_CASE("set entity mode switches unmaterialized buffered entities to predict") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    const ashiato::Entity server_entity = registry.create();
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 2;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));

    const ashiato::sync::ClientEntityNetworkId client_entity_network_id =
        test_client_entity_network_id(1, server_entity);
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{3.0f, 0.0f})));
    REQUIRE(client.has_entity(client_entity_network_id));
    REQUIRE_FALSE(client.local_entity(client_entity_network_id));

    client.set_entity_mode(registry, client_entity_network_id, ashiato::sync::ReplicationClientMode::Predict);

    const ashiato::Entity local = client.local_entity(client_entity_network_id);
    REQUIRE(local);
    REQUIRE(registry.alive(local));
    REQUIRE(client.entity_mode(client_entity_network_id) == ashiato::sync::ReplicationClientMode::Predict);
    REQUIRE(registry.get<PredictedPosition>(local).x == 3.0f);
}

TEST_CASE("set entity mode switches an entity by client network id") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, {}));
    client.simulation_job<PredictedPosition>(registry, 0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f}, 1)));

    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, server_entity)) == ashiato::sync::ReplicationClientMode::Snap);

    const ashiato::sync::ClientEntityNetworkId client_entity_network_id =
        test_client_entity_network_id(1, test_network_id(server_entity));
    client.set_entity_mode(registry, client_entity_network_id, ashiato::sync::ReplicationClientMode::Predict);
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, server_entity)) == ashiato::sync::ReplicationClientMode::Predict);

    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
}

TEST_CASE("set entity mode by client network id switches buffered delayed destroys to predict immediately") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    const ashiato::Entity server_entity = registry.create();
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 1;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));

    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{1.0f, 0.0f})));
    REQUIRE(apply_estimated_server_frame(client, registry, 2));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);

    REQUIRE(client.receive(registry, make_destroy_packet(2, server_entity)));
    REQUIRE(registry.alive(local));
    const ashiato::sync::ClientEntityNetworkId client_entity_network_id =
        test_client_entity_network_id(1, test_network_id(server_entity));
    REQUIRE(client.set_default_entity_mode(ashiato::sync::ReplicationClientMode::Predict));
    client.set_entity_mode(registry, client_entity_network_id, ashiato::sync::ReplicationClientMode::Predict);
    REQUIRE_FALSE(registry.alive(local));
    REQUIRE(client.entity_mode(test_client_entity_network_id(1, server_entity)) == ashiato::sync::ReplicationClientMode::Predict);
}

TEST_CASE("set entity mode switches buffered delayed destroys to snap immediately") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    const ashiato::Entity server_entity{42};

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 1;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 2));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client_registry.alive(local));
    client.set_entity_mode(
        client_registry,
        test_client_entity_network_id(1, server_entity),
        ashiato::sync::ReplicationClientMode::Snap);
    REQUIRE_FALSE(client_registry.alive(local));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, server_entity)));
    REQUIRE_THROWS_AS(client.set_entity_mode(
        client_registry,
        test_client_entity_network_id(1, server_entity),
        ashiato::sync::ReplicationClientMode::BufferedInterpolation),
        ashiato::sync::ClientError);
}

TEST_CASE("snap-selected entities do not require interpolation trait hooks") {
    ashiato::Registry client_registry;
    const ashiato::Entity position = ashiato::sync::register_sync_component<Position>(client_registry, "Position");
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato::sync::define_archetype(
        client_registry,
        "PositionActor",
        {{position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Snap;
    options.buffered.buffered_frame_lag = 1;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));
    const ashiato::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
}

TEST_CASE("buffered interpolation fills skipped frames with component trait interpolation") {
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
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    using SmallBufferedClient = ashiato::sync::ReplicationClientT<
        ashiato::sync::protocol::default_network_entity_id_tier0_bits,
        4,
        64>;
    SmallBufferedClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{2}}));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const ashiato::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }

    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server.tick(server_registry, server.options().fixed_dt_seconds);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{20.0f, 0.0f};
    server.tick(server_registry, server.options().fixed_dt_seconds);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{30.0f, 0.0f};
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(apply_estimated_server_frame(client, client_registry, 4));
    ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 10.0f);
    REQUIRE(apply_estimated_server_frame(client, client_registry, 5));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 20.0f);
    REQUIRE(apply_estimated_server_frame(client, client_registry, 6));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 30.0f);
}

TEST_CASE("buffered interpolation floors synced tags") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_visible = server_registry.register_component<Visible>("Visible");
    const ashiato::Entity server_secret = server_registry.register_component<Secret>("Secret");
    const ashiato::Entity server_position =
        ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato::sync::define_archetype(
        server_registry,
        ashiato::sync::SyncArchetypeDesc{
            "TaggedActor",
            {{server_visible, ashiato::sync::ReplicationAudience::All},
             {server_secret, ashiato::sync::ReplicationAudience::All}},
            {{server_position, ashiato::sync::ReplicationAudience::All}},
        });
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(server_entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add_tag(server_entity, server_visible));

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ashiato::Registry client_registry;
    const ashiato::Entity client_visible = client_registry.register_component<Visible>("Visible");
    const ashiato::Entity client_secret = client_registry.register_component<Secret>("Secret");
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                ashiato::sync::SyncArchetypeDesc{
                    "TaggedActor",
                    {{client_visible, ashiato::sync::ReplicationAudience::All},
                     {client_secret, ashiato::sync::ReplicationAudience::All}},
                    {{client_position, ashiato::sync::ReplicationAudience::All}},
                }) == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    using WrappedBufferedClient = ashiato::sync::ReplicationClientT<
        ashiato::sync::protocol::default_network_entity_id_tier0_bits,
        4,
        64>;
    WrappedBufferedClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const ashiato::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }

    REQUIRE(server_registry.add_tag(server_entity, server_secret));
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(apply_estimated_server_frame(client, client_registry, 2));
    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(client_registry.has(local, client_visible));
    REQUIRE_FALSE(client_registry.has(local, client_secret));

    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));
    REQUIRE(client_registry.has(local, client_visible));
    REQUIRE(client_registry.has(local, client_secret));
}

TEST_CASE("buffered interpolation delays component removal and entity destroy") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_position = ashiato::sync::register_sync_component<Position>(server_registry, "Position");
    const ashiato::Entity server_health = ashiato::sync::register_sync_component<Health>(server_registry, "Health");
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato::sync::define_archetype(
        server_registry,
        "Actor",
        {
            {server_position, ashiato::sync::ReplicationAudience::All},
            {server_health, ashiato::sync::ReplicationAudience::All},
        });
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{7}) != nullptr);

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ashiato::Registry client_registry;
    const ashiato::Entity client_position = ashiato::sync::register_sync_component<Position>(client_registry, "Position");
    const ashiato::Entity client_health = ashiato::sync::register_sync_component<Health>(client_registry, "Health");
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato::sync::define_archetype(
        client_registry,
        "Actor",
        {
            {client_position, ashiato::sync::ReplicationAudience::All},
            {client_health, ashiato::sync::ReplicationAudience::All},
        });
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    using WrappedBufferedClient = ashiato::sync::ReplicationClientT<
        ashiato::sync::protocol::default_network_entity_id_tier0_bits,
        4,
        64>;
    WrappedBufferedClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 2));
    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(client_registry.contains<Health>(local));
    for (const ashiato::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }

    packets.clear();
    REQUIRE(server_registry.remove<Health>(server_entity));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 2));
    REQUIRE(client_registry.contains<Health>(local));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));
    REQUIRE_FALSE(client_registry.contains<Health>(local));
    for (const ashiato::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }

    packets.clear();
    REQUIRE(server_registry.destroy(server_entity));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client_registry.alive(local));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));
    REQUIRE(client_registry.alive(local));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(local));
}

TEST_CASE("buffered interpolation rejects interpolated components without trait hooks") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_position = ashiato::sync::register_sync_component<Position>(server_registry, "Position");
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, ashiato::sync::ReplicationAudience::All}});
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);

    ashiato::Registry client_registry;
    const ashiato::Entity client_position = ashiato::sync::register_sync_component<Position>(client_registry, "Position");
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato::sync::define_archetype(
        client_registry,
        "PositionActor",
        {{client_position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));
    REQUIRE_FALSE(client.receive(client_registry, packets.back()));
    REQUIRE(client.pending_ack_count() == 0);
    REQUIRE_FALSE(client.local_entity(first_allocated_client_entity_network_id(1)));
}

TEST_CASE("buffered interpolation keeps step components held between interpolated samples") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_position =
        ashiato::sync::register_sync_component<SmoothPosition>(server_registry, "SmoothPosition");
    const ashiato::Entity server_health = ashiato::sync::register_sync_component<Health>(server_registry, "Health");
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato::sync::define_archetype(
        server_registry,
        "MixedActor",
        {
            {server_position, ashiato::sync::ReplicationAudience::All},
            {server_health, ashiato::sync::ReplicationAudience::All},
        });
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<SmoothPosition>(server_entity, SmoothPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{10}) != nullptr);

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
    const ashiato::Entity client_health = ashiato::sync::register_sync_component<Health>(client_registry, "Health");
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato::sync::define_archetype(
        client_registry,
        "MixedActor",
        {
            {client_position, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate},
            {client_health, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Step},
        });
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{2}}));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const ashiato::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }
    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));
    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);

    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{20};
    server.tick(server_registry, server.options().fixed_dt_seconds);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{20.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{30};
    server.tick(server_registry, server.options().fixed_dt_seconds);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{30.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{40};
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(apply_estimated_server_frame(client, client_registry, 4));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 10.0f);
    REQUIRE(client_registry.get<Health>(local).value == 10);
    REQUIRE(apply_estimated_server_frame(client, client_registry, 5));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 20.0f);
    REQUIRE(client_registry.get<Health>(local).value == 10);
    REQUIRE(apply_estimated_server_frame(client, client_registry, 6));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 30.0f);
    REQUIRE(client_registry.get<Health>(local).value == 40);
}

TEST_CASE("buffered interpolation validates wrapped buffer samples by frame") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato_sync_tests::define_position_archetype(server_registry);
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 0.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    using WrappedBufferedClient = ashiato::sync::ReplicationClientT<
        ashiato::sync::protocol::default_network_entity_id_tier0_bits,
        4,
        64>;
    WrappedBufferedClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));

    for (int frame = 1; frame <= 6; ++frame) {
        packets.clear();
        server_registry.write<Position>(server_entity) = Position{static_cast<float>(frame), 0.0f};
        server.tick(server_registry, server.options().fixed_dt_seconds);
        REQUIRE(client.receive(client_registry, packets.back()));
        for (const ashiato::BitBuffer& ack : client.drain_ack_packets()) {
            REQUIRE(server.process_packet(server_registry, 1, ack));
        }
    }

    REQUIRE_FALSE(apply_estimated_server_frame(client, client_registry, 3));
    REQUIRE_FALSE(client.local_entity(first_allocated_client_entity_network_id(1)));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 7));

    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 6.0f);
}

TEST_CASE("buffered interpolation rejects archetype changes while syncing") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_position = ashiato::sync::register_sync_component<Position>(server_registry, "Position");
    const ashiato::Entity server_health = ashiato::sync::register_sync_component<Health>(server_registry, "Health");
    const ashiato::sync::SyncArchetypeId position_archetype = ashiato::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, ashiato::sync::ReplicationAudience::All}});
    const ashiato::sync::SyncArchetypeId health_archetype = ashiato::sync::define_archetype(
        server_registry,
        "HealthActor",
        {
            {server_position, ashiato::sync::ReplicationAudience::All},
            {server_health, ashiato::sync::ReplicationAudience::All},
        });
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, position_archetype));

    ashiato::Registry client_registry;
    const ashiato::Entity client_position = ashiato::sync::register_sync_component<Position>(client_registry, "Position");
    const ashiato::Entity client_health = ashiato::sync::register_sync_component<Health>(client_registry, "Health");
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "PositionActor",
                {{client_position, ashiato::sync::ReplicationAudience::All}}) == position_archetype);
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "HealthActor",
                {
                    {client_position, ashiato::sync::ReplicationAudience::All},
                    {client_health, ashiato::sync::ReplicationAudience::All},
                }) == health_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 2));
    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE_FALSE(client_registry.contains<Health>(local));
    for (const ashiato::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }

    REQUIRE(server_registry.add<Health>(server_entity, Health{7}) != nullptr);
    REQUIRE(start_sync(server_registry, server_entity, health_archetype));
    REQUIRE_THROWS_AS(server.tick(server_registry, server.options().fixed_dt_seconds), std::logic_error);
}

TEST_CASE("buffered interpolation delays owner visibility reconciliation") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_position =
        ashiato::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const ashiato::Entity server_health = ashiato::sync::register_sync_component<Health>(server_registry, "Health");
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato::sync::define_archetype(
        server_registry,
        "OwnedActor",
        {
            {server_position, ashiato::sync::ReplicationAudience::All},
            {server_health, ashiato::sync::ReplicationAudience::Owner},
        });
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(server_entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{42}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, server_entity, 1));

    std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId client_id, const ashiato::BitBuffer& packet) {
        packets.push_back({client_id, packet});
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ashiato::Registry client_one_registry;
    const ashiato::Entity client_one_position =
        ashiato::sync::register_sync_component<NetworkedPosition>(client_one_registry, "NetworkedPosition");
    const ashiato::Entity client_one_health = ashiato::sync::register_sync_component<Health>(client_one_registry, "Health");
    REQUIRE(ashiato::sync::define_archetype(
                client_one_registry,
                "OwnedActor",
                {
                    {client_one_position, ashiato::sync::ReplicationAudience::All},
                    {client_one_health, ashiato::sync::ReplicationAudience::Owner},
                }) == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_one_registry, 1);

    ashiato::Registry client_two_registry;
    const ashiato::Entity client_two_position =
        ashiato::sync::register_sync_component<NetworkedPosition>(client_two_registry, "NetworkedPosition");
    const ashiato::Entity client_two_health = ashiato::sync::register_sync_component<Health>(client_two_registry, "Health");
    REQUIRE(ashiato::sync::define_archetype(
                client_two_registry,
                "OwnedActor",
                {
                    {client_two_position, ashiato::sync::ReplicationAudience::All},
                    {client_two_health, ashiato::sync::ReplicationAudience::Owner},
                }) == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_two_registry, 2);

    ashiato::sync::ReplicationClient client_one(client_one_registry, ashiato_sync_tests::make_test_client_options(client_one_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));
    ashiato::sync::ReplicationClient client_two(client_two_registry, ashiato_sync_tests::make_test_client_options(client_two_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    REQUIRE(apply_estimated_server_frame(client_one, client_one_registry, 2));
    REQUIRE(apply_estimated_server_frame(client_two, client_two_registry, 2));
    const ashiato::Entity client_one_local = client_one.local_entity(first_allocated_client_entity_network_id(1));
    const ashiato::Entity client_two_local = client_two.local_entity(first_allocated_client_entity_network_id(2));
    REQUIRE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE_FALSE(client_two_registry.contains<Health>(client_two_local));
    for (const ashiato::BitBuffer& ack : client_one.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }
    for (const ashiato::BitBuffer& ack : client_two.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 2, ack));
    }

    REQUIRE(ashiato::sync::set_owner(server_registry, server_entity, 2));
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    REQUIRE(apply_estimated_server_frame(client_one, client_one_registry, 2));
    REQUIRE(apply_estimated_server_frame(client_two, client_two_registry, 2));
    REQUIRE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE_FALSE(client_two_registry.contains<Health>(client_two_local));

    REQUIRE(apply_estimated_server_frame(client_one, client_one_registry, 3));
    REQUIRE(apply_estimated_server_frame(client_two, client_two_registry, 3));
    REQUIRE_FALSE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE(client_two_registry.get<Health>(client_two_local).value == 42);
}

TEST_CASE("buffered interpolation applies valid entities when another target sample is missing") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));

    const ashiato::Entity first{101};
    const ashiato::Entity second{102};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{first, Position{1.0f, 0.0f}}, {second, Position{2.0f, 0.0f}}})));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{first, Position{3.0f, 0.0f}}})));

    REQUIRE_FALSE(apply_estimated_server_frame(client, client_registry, 3));
    REQUIRE(client.local_entity(test_client_entity_network_id(1, first)));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, second)));
    REQUIRE(client_registry.get<Position>(client.local_entity(test_client_entity_network_id(1, first))).x == 3.0f);
}

#ifdef ASHIATO_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS

TEST_CASE("buffered interpolation diagnostics count missing target samples") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));

    const ashiato::Entity first{101};
    const ashiato::Entity second{102};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{first, Position{1.0f, 0.0f}}, {second, Position{2.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{first, Position{3.0f, 0.0f}}})));

    REQUIRE_FALSE(apply_estimated_server_frame(client, client_registry, 3));
    const ashiato::sync::ReplicationClientInterpolationDiagnostics& diagnostics = client.interpolation_diagnostics();
    REQUIRE(diagnostics.total_interpolated_entity_frame_checks == 2);
    REQUIRE(diagnostics.total_interpolated_entity_frame_starvations == 1);
    REQUIRE(diagnostics.window_interpolated_entity_frame_checks == 2);
    REQUIRE(diagnostics.window_interpolated_entity_frame_starvations == 1);
    REQUIRE(diagnostics.interpolated_entity_starvation_rate() == Catch::Approx(0.5f));
    REQUIRE(diagnostics.interpolated_entity_starvation_percent() == Catch::Approx(50.0f));
    REQUIRE(diagnostics.lifetime_interpolated_entity_starvation_rate() == Catch::Approx(0.5f));

    for (std::size_t offset = 0; offset < ashiato::sync::interpolation_diagnostics_window_frames; ++offset) {
        const ashiato::sync::SyncFrame frame = static_cast<ashiato::sync::SyncFrame>(3U + offset);
        REQUIRE(client.receive(client_registry, make_position_packet(
            frame,
            {{first, Position{static_cast<float>(frame), 0.0f}}, {second, Position{static_cast<float>(frame), 0.0f}}})));
        REQUIRE(apply_estimated_server_frame(client, client_registry, static_cast<ashiato::sync::SyncFrame>(frame + 1U)));
    }

    REQUIRE(diagnostics.total_interpolated_entity_frame_starvations == 1);
    REQUIRE(diagnostics.window_interpolated_entity_frame_checks == ashiato::sync::interpolation_diagnostics_window_frames * 2U);
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
    ashiato::Registry registry;
    using SmallBufferedClient = ashiato::sync::ReplicationClientT<
        ashiato::sync::protocol::default_network_entity_id_tier0_bits,
        4,
        64>;

    REQUIRE_THROWS_AS(
        SmallBufferedClient(registry, ashiato::sync::ReplicationClientOptions{
            ashiato::sync::ReplicationClientNetworkOptions{1200},
            ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
            ashiato::sync::ReplicationClientBufferedOptions{4}}),
        std::invalid_argument);

    SmallBufferedClient client(registry, ashiato_sync_tests::make_test_client_options(registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{1}}));
    REQUIRE(client.set_buffered_frame_lag(3));
    REQUIRE_FALSE(client.set_buffered_frame_lag(4));
    REQUIRE(client.current_buffered_frame_lag() == 3);

    REQUIRE_THROWS_AS(
        SmallBufferedClient(registry, ashiato::sync::ReplicationClientOptions{
            ashiato::sync::ReplicationClientNetworkOptions{1200},
            ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
            ashiato::sync::ReplicationClientBufferedOptions{1, true, 4}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        ashiato::sync::ReplicationClient(registry, ashiato::sync::ReplicationClientOptions{
            ashiato::sync::ReplicationClientNetworkOptions{1200},
            ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
            ashiato::sync::ReplicationClientBufferedOptions{1, true, 1, -1.0f}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        ashiato::sync::ReplicationClient(registry, ashiato::sync::ReplicationClientOptions{
            ashiato::sync::ReplicationClientNetworkOptions{1200},
            ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
            ashiato::sync::ReplicationClientBufferedOptions{1, true, 1, 2.0f, 0.0f}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        ashiato::sync::ReplicationClient(registry, ashiato::sync::ReplicationClientOptions{
            ashiato::sync::ReplicationClientNetworkOptions{1200},
            ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
            ashiato::sync::ReplicationClientBufferedOptions{1, true, 1, 2.0f, 0.1f, 0.0f}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        ashiato::sync::ReplicationClient(registry, ashiato::sync::ReplicationClientOptions{
            ashiato::sync::ReplicationClientNetworkOptions{1200},
            ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
            ashiato::sync::ReplicationClientBufferedOptions{1, true, 1, 2.0f, 0.1f, 0.95f, 0.5f}}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        ashiato::sync::ReplicationClient(registry, ashiato::sync::ReplicationClientOptions{
            ashiato::sync::ReplicationClientNetworkOptions{1200},
            ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
            ashiato::sync::ReplicationClientBufferedOptions{1, true, 1, 2.0f, 0.1f, 0.95f, 1.05f, -0.1f}}),
        std::invalid_argument);
}

TEST_CASE("buffered interpolation does not recreate destroyed entities before the new target frame") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{2}}));
    const ashiato::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));
    const ashiato::Entity first_local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(first_local);
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(first_local));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(4, {{server_entity, Position{5.0f, 6.0f}}})));

    REQUIRE(apply_estimated_server_frame(client, client_registry, 5));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, server_entity)));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 6));
    const ashiato::Entity second_local = client.local_entity(test_client_entity_network_id(1, server_entity, 2U));
    REQUIRE(second_local);
    REQUIRE(second_local != first_local);
    REQUIRE(client_registry.get<Position>(second_local).x == 5.0f);
    REQUIRE(client_registry.get<Position>(second_local).y == 6.0f);
}

TEST_CASE("buffered interpolation applies late destroys for already sampled target frames") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{2}}));
    const ashiato::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE_FALSE(apply_estimated_server_frame(client, client_registry, 4));

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE_FALSE(client_registry.alive(local));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, server_entity)));

    const std::vector<ashiato::BitBuffer> acks = client.drain_ack_packets();
    REQUIRE(acks.size() == 1);
    const std::vector<AckRecord> records = read_acks(acks[0]);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].packet_id == 2);
}

TEST_CASE("buffered interpolation applies late creates for already sampled target frames") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{2}}));
    const ashiato::Entity server_entity{42};

    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, server_entity)));

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{7.0f, 8.0f}}})));

    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 7.0f);
    REQUIRE(client_registry.get<Position>(local).y == 8.0f);

    const std::vector<ashiato::BitBuffer> acks = client.drain_ack_packets();
    REQUIRE(acks.size() == 1);
    const std::vector<AckRecord> records = read_acks(acks[0]);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].packet_id == 1);
}

TEST_CASE("buffered interpolation repopulates after destroy and create churn") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato_sync_tests::define_position_archetype(server_registry);

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 64U * 1024U;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{2}}));

    auto spawn_batch = [&](int begin) {
        std::vector<ashiato::Entity> entities;
        entities.reserve(96);
        for (int index = 0; index < 96; ++index) {
            const ashiato::Entity entity = server_registry.create();
            REQUIRE(server_registry.add<Position>(
                        entity,
                        Position{static_cast<float>(begin + index), static_cast<float>(index)})
                    != nullptr);
            REQUIRE(start_sync(server_registry, entity, server_archetype));
            entities.push_back(entity);
        }
        return entities;
    };
    auto deliver = [&]() {
        for (const ashiato::BitBuffer& packet : packets) {
            REQUIRE(client.receive(client_registry, packet));
        }
        packets.clear();
        for (const ashiato::BitBuffer& ack : client.drain_ack_packets()) {
            REQUIRE(server.process_packet(1, ack));
        }
    };
    auto client_position_count = [&]() {
        int count = 0;
        client_registry.view<const Position>().each([&](ashiato::Entity, const Position&) {
            ++count;
        });
        return count;
    };
    auto run_server_tick = [&]() {
        REQUIRE(server.tick(server_registry, server.options().fixed_dt_seconds));
    };

    std::vector<ashiato::Entity> entities = spawn_batch(0);
    run_server_tick();
    deliver();
    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));
    REQUIRE(client_position_count() == 96);

    for (ashiato::Entity entity : entities) {
        REQUIRE(server_registry.destroy(entity));
    }
    entities = spawn_batch(1000);
    run_server_tick();
    deliver();

    REQUIRE(apply_estimated_server_frame(client, client_registry, 4));
    REQUIRE(client_position_count() == 96);
}

TEST_CASE("buffered interpolation keeps client entity network ids alive until destroy frame applies") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, ashiato::sync::ReplicationClientOptions{
        ashiato::sync::ReplicationClientNetworkOptions{1200},
        ashiato::sync::ReplicationClientEntityOptions{ashiato::sync::ReplicationClientMode::BufferedInterpolation},
        ashiato::sync::ReplicationClientBufferedOptions{2}}));
    const ashiato::Entity server_entity{42};
    const ashiato::sync::ClientEntityNetworkId client_entity_network_id =
        test_client_entity_network_id(1, test_network_id(server_entity), 1U);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(apply_estimated_server_frame(client, client_registry, 3));
    const ashiato::Entity local = client.local_entity(client_entity_network_id);
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.is_alive_client_entity_network_id(client_entity_network_id));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.local_entity(client_entity_network_id) == local);
    REQUIRE(client.is_alive_client_entity_network_id(client_entity_network_id));

    REQUIRE(apply_estimated_server_frame(client, client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(local));
    REQUIRE_FALSE(client.is_alive_client_entity_network_id(client_entity_network_id));
    REQUIRE(client.drain_ack_packets().size() == 1);
}
