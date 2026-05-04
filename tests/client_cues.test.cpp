#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

TEST_CASE("replicated snap cues play once with late time and ACK-driven resend") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    kage::sync::register_sync_cue<TestCue>(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    REQUIRE(kage::sync::emit_cue(server_registry, server_entity, 1, TestCue{7}, 1.0f));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    client_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(client_registry);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClient client;

    REQUIRE(client.receive(client_registry, packets[0], 10));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(client_registry.contains<CuePlayback>(local));
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);
    REQUIRE(client_registry.get<CuePlayback>(local).last_id == 7);
    REQUIRE(client_registry.get<CuePlayback>(local).last_late_seconds ==
            Catch::Approx(9.0 / 60.0f));

    packets.clear();
    server.tick(server_registry);
    REQUIRE_FALSE(packets.empty());
    REQUIRE(client.receive(client_registry, packets.back(), 11));
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);

    std::vector<kage::sync::BitBuffer> acks = client.drain_ack_packets();
    REQUIRE_FALSE(acks.empty());
    for (const kage::sync::BitBuffer& ack : acks) {
        REQUIRE(server.process_packet(1, ack));
    }
    packets.clear();
    server.tick(server_registry);
    REQUIRE_FALSE(packets.empty());
    REQUIRE(client.receive(client_registry, packets.back(), 12));
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);
}

TEST_CASE("owner-only cues replicate only to the cue owner") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    kage::sync::register_sync_cue<TestCue>(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 1));

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& packet) {
        packets.push_back({client, packet});
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    REQUIRE(kage::sync::emit_cue(server_registry, server_entity, 1, TestCue{11}, 1.0f, true));
    server.tick(server_registry);

    ecs::Registry owner_registry;
    REQUIRE(kage_sync_tests::define_position_archetype(owner_registry) == server_archetype);
    owner_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(owner_registry);
    kage::sync::configure_client(owner_registry, 1);

    ecs::Registry other_registry;
    REQUIRE(kage_sync_tests::define_position_archetype(other_registry) == server_archetype);
    other_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(other_registry);
    kage::sync::configure_client(other_registry, 2);

    kage::sync::ReplicationClient owner_client;
    kage::sync::ReplicationClient other_client;
    REQUIRE(owner_client.receive(owner_registry, packet_for(packets, 1)));
    REQUIRE(other_client.receive(other_registry, packet_for(packets, 2)));

    const ecs::Entity owner_local = owner_client.local_entity(server_entity);
    const ecs::Entity other_local = other_client.local_entity(server_entity);
    REQUIRE(owner_registry.contains<CuePlayback>(owner_local));
    REQUIRE(owner_registry.get<CuePlayback>(owner_local).last_id == 11);
    REQUIRE_FALSE(other_registry.contains<CuePlayback>(other_local));
}

TEST_CASE("default cues still replicate to all clients") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    kage::sync::register_sync_cue<TestCue>(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& packet) {
        packets.push_back({client, packet});
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    REQUIRE(kage::sync::emit_cue(server_registry, server_entity, 1, TestCue{12}, 1.0f));
    server.tick(server_registry);

    ecs::Registry first_registry;
    REQUIRE(kage_sync_tests::define_position_archetype(first_registry) == server_archetype);
    first_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(first_registry);
    kage::sync::configure_client(first_registry, 1);

    ecs::Registry second_registry;
    REQUIRE(kage_sync_tests::define_position_archetype(second_registry) == server_archetype);
    second_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(second_registry);
    kage::sync::configure_client(second_registry, 2);

    kage::sync::ReplicationClient first_client;
    kage::sync::ReplicationClient second_client;
    REQUIRE(first_client.receive(first_registry, packet_for(packets, 1)));
    REQUIRE(second_client.receive(second_registry, packet_for(packets, 2)));

    REQUIRE(first_registry.get<CuePlayback>(first_client.local_entity(server_entity)).last_id == 12);
    REQUIRE(second_registry.get<CuePlayback>(second_client.local_entity(server_entity)).last_id == 12);
}

TEST_CASE("cue entity references resolve to client-local entities") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    kage::sync::register_sync_cue<ReferenceCue>(server_registry);
    const ecs::Entity target = server_registry.create();
    const ecs::Entity source = server_registry.create();
    REQUIRE(server_registry.add<Position>(target, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add<Position>(source, Position{3.0f, 4.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, source, 1));

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, target, server_archetype));
    REQUIRE(start_sync(server_registry, source, server_archetype));
    REQUIRE(kage::sync::emit_cue(
        server_registry,
        source,
        1,
        ReferenceCue{kage::sync::EntityReference{target}},
        1.0f,
        true));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    REQUIRE(kage_sync_tests::define_position_archetype(client_registry) == server_archetype);
    client_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<ReferenceCue>(client_registry);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ClientEntityNetworkId target_network_id = kage::sync::invalid_client_entity_network_id;
    kage::sync::ClientEntityNetworkId source_network_id = kage::sync::invalid_client_entity_network_id;
    kage::sync::ReplicationClientOptions client_options;
    client_options.entity_mode_selector = [&](const kage::sync::ReplicatedEntityUpdateView& update) {
        Position position{};
        if (update.try_get(client_registry, position) && position.x == 1.0f) {
            target_network_id = update.client_entity_network_id;
        } else {
            source_network_id = update.client_entity_network_id;
        }
        return kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(client_options);
    REQUIRE(client.receive(client_registry, packets[0]));

    const ecs::Entity local_source = client.local_entity(source_network_id);
    const ecs::Entity local_target = client.local_entity(target_network_id);
    REQUIRE(local_source);
    REQUIRE(local_target);
    const CuePlayback& playback = client_registry.get<CuePlayback>(local_source);
    REQUIRE(playback.plays == 1);
    REQUIRE(playback.last_target == local_target);
    REQUIRE(playback.last_target_network_id != kage::sync::invalid_client_entity_network_id);
}

TEST_CASE("entity references serialize as client-local network ids") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<Position>(server_registry, "Position");
    const ecs::Entity server_reference =
        kage::sync::register_sync_component<TargetReference>(server_registry, "TargetReference");
    const kage::sync::SyncArchetypeId position_archetype = kage::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const kage::sync::SyncArchetypeId reference_archetype = kage::sync::define_archetype(
        server_registry,
        "ReferenceActor",
        {{server_reference, kage::sync::ReplicationAudience::All}});

    const ecs::Entity target = server_registry.create();
    const ecs::Entity source = server_registry.create();
    REQUIRE(server_registry.add<Position>(target, Position{4.0f, 5.0f}) != nullptr);
    REQUIRE(server_registry.add<TargetReference>(
                source,
                TargetReference{kage::sync::EntityReference{target}}) != nullptr);
    REQUIRE(start_sync(server_registry, target, position_archetype));
    REQUIRE(start_sync(server_registry, source, reference_archetype));

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<Position>(client_registry, "Position");
    const ecs::Entity client_reference =
        kage::sync::register_sync_component<TargetReference>(client_registry, "TargetReference");
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "PositionActor",
                {{client_position, kage::sync::ReplicationAudience::All}}) == position_archetype);
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "ReferenceActor",
                {{client_reference, kage::sync::ReplicationAudience::All}}) == reference_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ClientEntityNetworkId target_network_id =
        kage::sync::invalid_client_entity_network_id;
    kage::sync::ClientEntityNetworkId source_network_id =
        kage::sync::invalid_client_entity_network_id;
    kage::sync::ReplicationClientOptions options;
    options.entity_mode_selector = [&](const kage::sync::ReplicatedEntityUpdateView& update) {
        if (update.archetype == position_archetype) {
            target_network_id = update.client_entity_network_id;
        } else if (update.archetype == reference_archetype) {
            source_network_id = update.client_entity_network_id;
        }
        return kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(options);
    REQUIRE(client.receive(client_registry, packets[0]));

    const ecs::Entity local_target = client.local_entity(target_network_id);
    const ecs::Entity local_source = client.local_entity(source_network_id);
    REQUIRE(local_target);
    REQUIRE(local_source);
    REQUIRE(client_registry.alive(local_target));
    REQUIRE(client_registry.alive(local_source));

    const TargetReference& reference = client_registry.get<TargetReference>(local_source);
    REQUIRE(reference.target.client_entity_network_id != kage::sync::invalid_client_entity_network_id);
    REQUIRE(reference.target.client_entity_network_id == target_network_id);
    REQUIRE(reference.target.entity == local_target);
    REQUIRE(client.local_entity(reference.target.client_entity_network_id) == local_target);
}

TEST_CASE("entity references remain resolvable when the referenced entity arrives later in the packet") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<Position>(server_registry, "Position");
    const ecs::Entity server_reference =
        kage::sync::register_sync_component<TargetReference>(server_registry, "TargetReference");
    const kage::sync::SyncArchetypeId position_archetype = kage::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const kage::sync::SyncArchetypeId reference_archetype = kage::sync::define_archetype(
        server_registry,
        "ReferenceActor",
        {{server_reference, kage::sync::ReplicationAudience::All}});

    const ecs::Entity source = server_registry.create();
    const ecs::Entity target = server_registry.create();
    REQUIRE(server_registry.add<TargetReference>(
                source,
                TargetReference{kage::sync::EntityReference{target}}) != nullptr);
    REQUIRE(server_registry.add<Position>(target, Position{4.0f, 5.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, source, reference_archetype));
    REQUIRE(start_sync(server_registry, target, position_archetype));

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<Position>(client_registry, "Position");
    const ecs::Entity client_reference =
        kage::sync::register_sync_component<TargetReference>(client_registry, "TargetReference");
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "PositionActor",
                {{client_position, kage::sync::ReplicationAudience::All}}) == position_archetype);
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "ReferenceActor",
                {{client_reference, kage::sync::ReplicationAudience::All}}) == reference_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ClientEntityNetworkId target_network_id =
        kage::sync::invalid_client_entity_network_id;
    kage::sync::ClientEntityNetworkId source_network_id =
        kage::sync::invalid_client_entity_network_id;
    kage::sync::ReplicationClientOptions options;
    options.entity_mode_selector = [&](const kage::sync::ReplicatedEntityUpdateView& update) {
        if (update.archetype == position_archetype) {
            target_network_id = update.client_entity_network_id;
        } else if (update.archetype == reference_archetype) {
            source_network_id = update.client_entity_network_id;
        }
        return kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(options);
    REQUIRE(client.receive(client_registry, packets[0]));

    const ecs::Entity local_target = client.local_entity(target_network_id);
    const ecs::Entity local_source = client.local_entity(source_network_id);
    REQUIRE(local_target);
    REQUIRE(local_source);

    const TargetReference& reference = client_registry.get<TargetReference>(local_source);
    REQUIRE(reference.target.client_entity_network_id != kage::sync::invalid_client_entity_network_id);
    REQUIRE(reference.target.client_entity_network_id == target_network_id);
    REQUIRE(reference.target.entity == ecs::Entity{});
    REQUIRE(client.local_entity(reference.target.client_entity_network_id) == local_target);

    kage::sync::EntityReference resolved = reference.target;
    REQUIRE(client.resolve_entity_reference(resolved) == kage::sync::EntityReferenceStatus::Alive);
    REQUIRE(resolved.client_entity_network_id == target_network_id);
    REQUIRE(resolved.entity == local_target);
}

TEST_CASE("resolve entity reference preserves pending references") {
    kage::sync::ReplicationClient client;

    kage::sync::EntityReference invalid;
    invalid.entity = ecs::Entity{77};
    REQUIRE(client.resolve_entity_reference(invalid) == kage::sync::EntityReferenceStatus::Invalid);
    REQUIRE_FALSE(invalid.entity);
    REQUIRE(invalid.client_entity_network_id == kage::sync::invalid_client_entity_network_id);

    const kage::sync::ClientEntityNetworkId pending_id = test_client_entity_network_id(1, 42U);
    kage::sync::EntityReference pending;
    pending.entity = ecs::Entity{77};
    pending.client_entity_network_id = pending_id;
    REQUIRE(client.resolve_entity_reference(pending) == kage::sync::EntityReferenceStatus::Pending);
    REQUIRE_FALSE(pending.entity);
    REQUIRE(pending.client_entity_network_id == pending_id);
}

TEST_CASE("resolve entity reference clears destroyed and reused network ids") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    const ecs::Entity server_entity{42};
    const std::uint32_t wire_id = test_network_id(server_entity);
    const kage::sync::ClientEntityNetworkId first_id = test_client_entity_network_id(1, wire_id, 1U);
    const kage::sync::ClientEntityNetworkId second_id = test_client_entity_network_id(1, wire_id, 2U);
    kage::sync::ReplicationClient client;

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    const ecs::Entity first_local = client.local_entity(first_id);
    REQUIRE(first_local);

    kage::sync::EntityReference alive{first_local, first_id};
    REQUIRE(client.resolve_entity_reference(alive) == kage::sync::EntityReferenceStatus::Alive);
    REQUIRE(alive.entity == first_local);
    REQUIRE(alive.client_entity_network_id == first_id);

    kage::sync::EntityReference destroyed{first_local, first_id};
    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client.resolve_entity_reference(destroyed) == kage::sync::EntityReferenceStatus::Destroyed);
    REQUIRE_FALSE(destroyed.entity);
    REQUIRE(destroyed.client_entity_network_id == kage::sync::invalid_client_entity_network_id);

    REQUIRE(client.receive(client_registry, make_position_packet(3, {{server_entity, Position{5.0f, 6.0f}}})));
    const ecs::Entity second_local = client.local_entity(second_id);
    REQUIRE(second_local);
    REQUIRE(second_local != first_local);

    kage::sync::EntityReference stale{first_local, first_id};
    REQUIRE(client.resolve_entity_reference(stale) == kage::sync::EntityReferenceStatus::Destroyed);
    REQUIRE_FALSE(stale.entity);
    REQUIRE(stale.client_entity_network_id == kage::sync::invalid_client_entity_network_id);

    kage::sync::EntityReference reused{ecs::Entity{}, second_id};
    REQUIRE(client.resolve_entity_reference(reused) == kage::sync::EntityReferenceStatus::Alive);
    REQUIRE(reused.entity == second_local);
    REQUIRE(reused.client_entity_network_id == second_id);
}
