#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace ashiato_sync_tests;

namespace {
struct RuntimeTestCue {
    std::int32_t id = 0;
};

bool deserialize_runtime_test_cue(
    ashiato::sync::SyncCueTypeId,
    void*,
    ashiato::BitBuffer& payload,
    ashiato::sync::CueValue& out,
    ashiato::ComponentSerializationContext&) {
    out.emplace<RuntimeTestCue>(RuntimeTestCue{static_cast<std::int32_t>(payload.read_bits(16U))});
    return true;
}

bool play_runtime_test_cue(
    ashiato::sync::SyncCueTypeId,
    void*,
    ashiato::Registry& registry,
    ashiato::Entity owner,
    const void* value,
    float late_seconds,
    ashiato::sync::SyncFrame frame) {
    const RuntimeTestCue* cue = static_cast<const RuntimeTestCue*>(value);
    if (cue == nullptr) {
        return false;
    }
    if (!registry.contains<CuePlayback>(owner)) {
        registry.add<CuePlayback>(owner);
    }
    CuePlayback& playback = registry.write<CuePlayback>(owner);
    ++playback.plays;
    playback.last_id = cue->id;
    playback.last_late_seconds = late_seconds;
    playback.last_frame = frame;
    return true;
}

bool rollback_runtime_test_cue(
    ashiato::sync::SyncCueTypeId,
    void*,
    ashiato::Registry& registry,
    ashiato::Entity owner,
    const void*) {
    if (!registry.contains<CuePlayback>(owner)) {
        registry.add<CuePlayback>(owner);
    }
    ++registry.write<CuePlayback>(owner).rollbacks;
    return true;
}

bool equal_runtime_test_cue(
    ashiato::sync::SyncCueTypeId,
    void*,
    const void* lhs,
    const void* rhs) {
    const RuntimeTestCue* left = static_cast<const RuntimeTestCue*>(lhs);
    const RuntimeTestCue* right = static_cast<const RuntimeTestCue*>(rhs);
    return left != nullptr && right != nullptr && left->id == right->id;
}
}

TEST_CASE("runtime cue ops register by key and raw cues enqueue") {
    ashiato::Registry registry;
    ashiato::sync::SyncCueOps ops;
    ops.name = "RuntimeCue";
    ops.deserialize_into = &deserialize_runtime_test_cue;
    ops.play = &play_runtime_test_cue;
    ops.rollback = &rollback_runtime_test_cue;
    ops.equals = &equal_runtime_test_cue;
    const ashiato::sync::SyncCueTypeId cue_type =
        ashiato::sync::register_runtime_sync_cue(registry, "RuntimeCue", ops);
    REQUIRE(ashiato::sync::register_runtime_sync_cue(registry, "RuntimeCue", ops) == cue_type);

    ashiato::sync::SyncCueTypeId found = 0;
    REQUIRE(ashiato::sync::find_runtime_sync_cue(registry, "RuntimeCue", found));
    REQUIRE(found == cue_type);

    const ashiato::Entity entity = registry.create();
    ashiato::BitBuffer payload;
    payload.write_bits(44, 16U);
    REQUIRE(registry.write<ashiato::sync::CueDispatcher>().emit_raw(
        registry.get<ashiato::sync::SyncSettings>(),
        3,
        entity,
        cue_type,
        payload,
        1.0f,
        true));
    REQUIRE_FALSE(registry.write<ashiato::sync::CueDispatcher>().emit_raw(
        registry.get<ashiato::sync::SyncSettings>(),
        3,
        entity,
        static_cast<ashiato::sync::SyncCueTypeId>(cue_type + 1U),
        payload,
        1.0f,
        true));

    const ashiato::sync::QueuedSyncCueView queued = registry.get<ashiato::sync::CueDispatcher>().view();
    REQUIRE(queued.size == 1);
    REQUIRE(queued.data[0].entity == entity);
    REQUIRE(queued.data[0].frame == 3);
    REQUIRE(queued.data[0].type == cue_type);
    REQUIRE(queued.data[0].only_replicate_to_owner);
    REQUIRE(queued.data[0].payload == payload);
}

TEST_CASE("replicated snap cues play once with late time and stop resending after ACK") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato_sync_tests::define_position_archetype(server_registry);
    ashiato::sync::register_sync_cue<TestCue>(server_registry);
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
    REQUIRE(ashiato_sync_tests::emit_test_cue(server_registry, server_entity, 1, TestCue{7}, 1.0f));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    client_registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(client_registry);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));

    REQUIRE(receive_at_local_frame(client, client_registry, packets[0], 10));
    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(client_registry.contains<CuePlayback>(local));
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);
    REQUIRE(client_registry.get<CuePlayback>(local).last_id == 7);
    REQUIRE(client_registry.get<CuePlayback>(local).last_late_seconds ==
            Catch::Approx(9.0 / 60.0f));

    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE_FALSE(packets.empty());
    REQUIRE(receive_at_local_frame(client, client_registry, packets.back(), 11));
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);

    std::vector<ashiato::BitBuffer> acks = client.drain_ack_packets();
    REQUIRE_FALSE(acks.empty());
    for (const ashiato::BitBuffer& ack : acks) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.empty());
}

TEST_CASE("replicated snap cues are sentinel-delimited without the old count cap") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato_sync_tests::define_position_archetype(server_registry);
    ashiato::sync::register_sync_cue<TestCue>(server_registry);
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

    constexpr int cue_count = 20;
    for (int cue_index = 0; cue_index < cue_count; ++cue_index) {
        REQUIRE(ashiato_sync_tests::emit_test_cue(
            server_registry,
            server_entity,
            1,
            TestCue{cue_index},
            1.0f));
    }
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    client_registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(client_registry);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));

    REQUIRE(receive_at_local_frame(client, client_registry, packets[0], 10));
    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(client_registry.contains<CuePlayback>(local));
    REQUIRE(client_registry.get<CuePlayback>(local).plays == cue_count);
    REQUIRE(client_registry.get<CuePlayback>(local).last_id == cue_count - 1);
}

TEST_CASE("buffered cues play once when their target frame applies") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato_sync_tests::define_position_archetype(server_registry);
    ashiato::sync::register_sync_cue<TestCue>(server_registry);
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
    REQUIRE(ashiato_sync_tests::emit_test_cue(server_registry, server_entity, 1, TestCue{21}, 1.0f));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);

    ashiato::Registry client_registry;
    REQUIRE(ashiato_sync_tests::define_position_archetype(client_registry) == server_archetype);
    client_registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(client_registry);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 1;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    REQUIRE(client.receive(client_registry, packets[0]));
    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE_FALSE(local);

    REQUIRE(client.apply_frame(client_registry, 1));
    const ashiato::Entity applied = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(applied);
    REQUIRE(client_registry.get<CuePlayback>(applied).plays == 1);
    REQUIRE(client_registry.get<CuePlayback>(applied).last_id == 21);

    REQUIRE(client.apply_frame(client_registry, 1));
    REQUIRE(client_registry.get<CuePlayback>(applied).plays == 1);
}

TEST_CASE("late buffered cues for already applied frames play immediately") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato_sync_tests::define_position_archetype(server_registry);
    ashiato::sync::register_sync_cue<TestCue>(server_registry);
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
    REQUIRE(ashiato_sync_tests::emit_test_cue(server_registry, server_entity, 1, TestCue{22}, 1.0f));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);

    ashiato::Registry client_registry;
    REQUIRE(ashiato_sync_tests::define_position_archetype(client_registry) == server_archetype);
    client_registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(client_registry);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 1;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    REQUIRE(client.apply_frame(client_registry, 1));
    REQUIRE(client.receive(client_registry, packets[0]));
    const ashiato::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);
    REQUIRE(client_registry.get<CuePlayback>(local).last_id == 22);

    REQUIRE(client.apply_frame(client_registry, 1));
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);
}

TEST_CASE("owner-only cues replicate only to the cue owner") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato_sync_tests::define_position_archetype(server_registry);
    ashiato::sync::register_sync_cue<TestCue>(server_registry);
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, server_entity, 1));

    std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& packet) {
        packets.push_back({client, packet});
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    REQUIRE(ashiato_sync_tests::emit_test_cue(server_registry, server_entity, 1, TestCue{11}, 1.0f, true));
    server.tick(server_registry, server.options().fixed_dt_seconds);

    ashiato::Registry owner_registry;
    REQUIRE(ashiato_sync_tests::define_position_archetype(owner_registry) == server_archetype);
    owner_registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(owner_registry);
    ashiato_sync_tests::configure_test_client_registry(owner_registry, 1);

    ashiato::Registry other_registry;
    REQUIRE(ashiato_sync_tests::define_position_archetype(other_registry) == server_archetype);
    other_registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(other_registry);
    ashiato_sync_tests::configure_test_client_registry(other_registry, 2);

    ashiato::sync::ReplicationClient owner_client(owner_registry, ashiato_sync_tests::make_test_client_options(owner_registry, {}));
    ashiato::sync::ReplicationClient other_client(other_registry, ashiato_sync_tests::make_test_client_options(other_registry, {}));
    REQUIRE(owner_client.receive(owner_registry, packet_for(packets, 1)));
    REQUIRE(other_client.receive(other_registry, packet_for(packets, 2)));

    const ashiato::Entity owner_local = owner_client.local_entity(first_allocated_client_entity_network_id(1));
    const ashiato::Entity other_local = other_client.local_entity(first_allocated_client_entity_network_id(2));
    REQUIRE(owner_registry.contains<CuePlayback>(owner_local));
    REQUIRE(owner_registry.get<CuePlayback>(owner_local).last_id == 11);
    REQUIRE_FALSE(other_registry.contains<CuePlayback>(other_local));
}

TEST_CASE("default cues still replicate to all clients") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato_sync_tests::define_position_archetype(server_registry);
    ashiato::sync::register_sync_cue<TestCue>(server_registry);
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& packet) {
        packets.push_back({client, packet});
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    REQUIRE(ashiato_sync_tests::emit_test_cue(server_registry, server_entity, 1, TestCue{12}, 1.0f));
    server.tick(server_registry, server.options().fixed_dt_seconds);

    ashiato::Registry first_registry;
    REQUIRE(ashiato_sync_tests::define_position_archetype(first_registry) == server_archetype);
    first_registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(first_registry);
    ashiato_sync_tests::configure_test_client_registry(first_registry, 1);

    ashiato::Registry second_registry;
    REQUIRE(ashiato_sync_tests::define_position_archetype(second_registry) == server_archetype);
    second_registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(second_registry);
    ashiato_sync_tests::configure_test_client_registry(second_registry, 2);

    ashiato::sync::ReplicationClient first_client(first_registry, ashiato_sync_tests::make_test_client_options(first_registry, {}));
    ashiato::sync::ReplicationClient second_client(second_registry, ashiato_sync_tests::make_test_client_options(second_registry, {}));
    REQUIRE(first_client.receive(first_registry, packet_for(packets, 1)));
    REQUIRE(second_client.receive(second_registry, packet_for(packets, 2)));

    REQUIRE(first_registry.get<CuePlayback>(first_client.local_entity(first_allocated_client_entity_network_id(1))).last_id == 12);
    REQUIRE(second_registry.get<CuePlayback>(second_client.local_entity(first_allocated_client_entity_network_id(2))).last_id == 12);
}

TEST_CASE("cue entity references resolve to client-local entities") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato_sync_tests::define_position_archetype(server_registry);
    ashiato::sync::register_sync_cue<ReferenceCue>(server_registry);
    const ashiato::Entity target = server_registry.create();
    const ashiato::Entity source = server_registry.create();
    REQUIRE(server_registry.add<Position>(target, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add<Position>(source, Position{3.0f, 4.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, source, 1));

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, target, server_archetype));
    REQUIRE(start_sync(server_registry, source, server_archetype));
    REQUIRE(ashiato_sync_tests::emit_test_cue(
        server_registry,
        source,
        1,
        ReferenceCue{ashiato::sync::EntityReference{target}},
        1.0f,
        true));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);

    ashiato::Registry client_registry;
    REQUIRE(ashiato_sync_tests::define_position_archetype(client_registry) == server_archetype);
    client_registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<ReferenceCue>(client_registry);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    ashiato::sync::ClientEntityNetworkId target_network_id = ashiato::sync::invalid_client_entity_network_id;
    ashiato::sync::ClientEntityNetworkId source_network_id = ashiato::sync::invalid_client_entity_network_id;
    ashiato::sync::ReplicationClientOptions client_options;
    client_options.entities.mode_selector = [&](const ashiato::sync::ReplicatedEntityUpdateView& update) {
        Position position{};
        if (update.try_get(client_registry, position) && position.x == 1.0f) {
            target_network_id = update.client_entity_network_id;
        } else {
            source_network_id = update.client_entity_network_id;
        }
        return ashiato::sync::ReplicationClientMode::Snap;
    };
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, client_options));
    REQUIRE(client.receive(client_registry, packets[0]));

    const ashiato::Entity local_source = client.local_entity(source_network_id);
    const ashiato::Entity local_target = client.local_entity(target_network_id);
    REQUIRE(local_source);
    REQUIRE(local_target);
    const CuePlayback& playback = client_registry.get<CuePlayback>(local_source);
    REQUIRE(playback.plays == 1);
    REQUIRE(playback.last_target == local_target);
    REQUIRE(playback.last_target_network_id != ashiato::sync::invalid_client_entity_network_id);
}

TEST_CASE("entity references serialize as client-local network ids") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_position =
        ashiato::sync::register_sync_component<Position>(server_registry, "Position");
    const ashiato::Entity server_reference =
        ashiato::sync::register_sync_component<TargetReference>(server_registry, "TargetReference");
    const ashiato::sync::SyncArchetypeId position_archetype = ashiato::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, ashiato::sync::ReplicationAudience::All}});
    const ashiato::sync::SyncArchetypeId reference_archetype = ashiato::sync::define_archetype(
        server_registry,
        "ReferenceActor",
        {{server_reference, ashiato::sync::ReplicationAudience::All}});

    const ashiato::Entity target = server_registry.create();
    const ashiato::Entity source = server_registry.create();
    REQUIRE(server_registry.add<Position>(target, Position{4.0f, 5.0f}) != nullptr);
    REQUIRE(server_registry.add<TargetReference>(
                source,
                TargetReference{ashiato::sync::EntityReference{target}}) != nullptr);
    REQUIRE(start_sync(server_registry, target, position_archetype));
    REQUIRE(start_sync(server_registry, source, reference_archetype));

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);

    ashiato::Registry client_registry;
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<Position>(client_registry, "Position");
    const ashiato::Entity client_reference =
        ashiato::sync::register_sync_component<TargetReference>(client_registry, "TargetReference");
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "PositionActor",
                {{client_position, ashiato::sync::ReplicationAudience::All}}) == position_archetype);
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "ReferenceActor",
                {{client_reference, ashiato::sync::ReplicationAudience::All}}) == reference_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ClientEntityNetworkId target_network_id =
        ashiato::sync::invalid_client_entity_network_id;
    ashiato::sync::ClientEntityNetworkId source_network_id =
        ashiato::sync::invalid_client_entity_network_id;
    ashiato::sync::ReplicationClientOptions options;
    options.entities.mode_selector = [&](const ashiato::sync::ReplicatedEntityUpdateView& update) {
        if (update.archetype == position_archetype) {
            target_network_id = update.client_entity_network_id;
        } else if (update.archetype == reference_archetype) {
            source_network_id = update.client_entity_network_id;
        }
        return ashiato::sync::ReplicationClientMode::Snap;
    };
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));
    REQUIRE(client.receive(client_registry, packets[0]));

    const ashiato::Entity local_target = client.local_entity(target_network_id);
    const ashiato::Entity local_source = client.local_entity(source_network_id);
    REQUIRE(local_target);
    REQUIRE(local_source);
    REQUIRE(client_registry.alive(local_target));
    REQUIRE(client_registry.alive(local_source));

    const TargetReference& reference = client_registry.get<TargetReference>(local_source);
    REQUIRE(reference.target.client_entity_network_id != ashiato::sync::invalid_client_entity_network_id);
    REQUIRE(reference.target.client_entity_network_id == target_network_id);
    REQUIRE(reference.target.entity == local_target);
    REQUIRE(client.local_entity(reference.target.client_entity_network_id) == local_target);
}

TEST_CASE("entity references remain resolvable when the referenced entity arrives later in the packet") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_position =
        ashiato::sync::register_sync_component<Position>(server_registry, "Position");
    const ashiato::Entity server_reference =
        ashiato::sync::register_sync_component<TargetReference>(server_registry, "TargetReference");
    const ashiato::sync::SyncArchetypeId position_archetype = ashiato::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, ashiato::sync::ReplicationAudience::All}});
    const ashiato::sync::SyncArchetypeId reference_archetype = ashiato::sync::define_archetype(
        server_registry,
        "ReferenceActor",
        {{server_reference, ashiato::sync::ReplicationAudience::All}});

    const ashiato::Entity source = server_registry.create();
    const ashiato::Entity target = server_registry.create();
    REQUIRE(server_registry.add<TargetReference>(
                source,
                TargetReference{ashiato::sync::EntityReference{target}}) != nullptr);
    REQUIRE(server_registry.add<Position>(target, Position{4.0f, 5.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, source, reference_archetype));
    REQUIRE(start_sync(server_registry, target, position_archetype));

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);

    ashiato::Registry client_registry;
    const ashiato::Entity client_position =
        ashiato::sync::register_sync_component<Position>(client_registry, "Position");
    const ashiato::Entity client_reference =
        ashiato::sync::register_sync_component<TargetReference>(client_registry, "TargetReference");
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "PositionActor",
                {{client_position, ashiato::sync::ReplicationAudience::All}}) == position_archetype);
    REQUIRE(ashiato::sync::define_archetype(
                client_registry,
                "ReferenceActor",
                {{client_reference, ashiato::sync::ReplicationAudience::All}}) == reference_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ClientEntityNetworkId target_network_id =
        ashiato::sync::invalid_client_entity_network_id;
    ashiato::sync::ClientEntityNetworkId source_network_id =
        ashiato::sync::invalid_client_entity_network_id;
    ashiato::sync::ReplicationClientOptions options;
    options.entities.mode_selector = [&](const ashiato::sync::ReplicatedEntityUpdateView& update) {
        if (update.archetype == position_archetype) {
            target_network_id = update.client_entity_network_id;
        } else if (update.archetype == reference_archetype) {
            source_network_id = update.client_entity_network_id;
        }
        return ashiato::sync::ReplicationClientMode::Snap;
    };
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));
    REQUIRE(client.receive(client_registry, packets[0]));

    const ashiato::Entity local_target = client.local_entity(target_network_id);
    const ashiato::Entity local_source = client.local_entity(source_network_id);
    REQUIRE(local_target);
    REQUIRE(local_source);

    const TargetReference& reference = client_registry.get<TargetReference>(local_source);
    REQUIRE(reference.target.client_entity_network_id != ashiato::sync::invalid_client_entity_network_id);
    REQUIRE(reference.target.client_entity_network_id == target_network_id);
    REQUIRE(reference.target.entity == ashiato::Entity{});
    REQUIRE(client.local_entity(reference.target.client_entity_network_id) == local_target);

    ashiato::sync::EntityReference resolved = reference.target;
    REQUIRE(client.resolve_entity_reference(resolved) == ashiato::sync::EntityReferenceStatus::Alive);
    REQUIRE(resolved.client_entity_network_id == target_network_id);
    REQUIRE(resolved.entity == local_target);
}

TEST_CASE("resolve entity reference preserves pending references") {
    ashiato::Registry client_registry;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));

    ashiato::sync::EntityReference invalid;
    invalid.entity = ashiato::Entity{77};
    REQUIRE(client.resolve_entity_reference(invalid) == ashiato::sync::EntityReferenceStatus::Invalid);
    REQUIRE_FALSE(invalid.entity);
    REQUIRE(invalid.client_entity_network_id == ashiato::sync::invalid_client_entity_network_id);

    const ashiato::sync::ClientEntityNetworkId pending_id = test_client_entity_network_id(1, 42U);
    ashiato::sync::EntityReference pending;
    pending.entity = ashiato::Entity{77};
    pending.client_entity_network_id = pending_id;
    REQUIRE(client.resolve_entity_reference(pending) == ashiato::sync::EntityReferenceStatus::Pending);
    REQUIRE_FALSE(pending.entity);
    REQUIRE(pending.client_entity_network_id == pending_id);
}

TEST_CASE("resolve entity reference clears destroyed and reused network ids") {
    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    ashiato::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(client_registry));

    const ashiato::Entity server_entity{42};
    const std::uint32_t wire_id = test_network_id(server_entity);
    const ashiato::sync::ClientEntityNetworkId first_id = test_client_entity_network_id(1, wire_id, 1U);
    const ashiato::sync::ClientEntityNetworkId second_id = test_client_entity_network_id(1, wire_id, 2U);
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    const ashiato::Entity first_local = client.local_entity(first_id);
    REQUIRE(first_local);

    ashiato::sync::EntityReference alive{first_local, first_id};
    REQUIRE(client.resolve_entity_reference(alive) == ashiato::sync::EntityReferenceStatus::Alive);
    REQUIRE(alive.entity == first_local);
    REQUIRE(alive.client_entity_network_id == first_id);

    ashiato::sync::EntityReference destroyed{first_local, first_id};
    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client.resolve_entity_reference(destroyed) == ashiato::sync::EntityReferenceStatus::Destroyed);
    REQUIRE_FALSE(destroyed.entity);
    REQUIRE(destroyed.client_entity_network_id == ashiato::sync::invalid_client_entity_network_id);

    REQUIRE(client.receive(client_registry, make_position_packet(3, {{server_entity, Position{5.0f, 6.0f}}})));
    const ashiato::Entity second_local = client.local_entity(second_id);
    REQUIRE(second_local);
    REQUIRE(second_local != first_local);

    ashiato::sync::EntityReference stale{first_local, first_id};
    REQUIRE(client.resolve_entity_reference(stale) == ashiato::sync::EntityReferenceStatus::Destroyed);
    REQUIRE_FALSE(stale.entity);
    REQUIRE(stale.client_entity_network_id == ashiato::sync::invalid_client_entity_network_id);

    ashiato::sync::EntityReference reused{ashiato::Entity{}, second_id};
    REQUIRE(client.resolve_entity_reference(reused) == ashiato::sync::EntityReferenceStatus::Alive);
    REQUIRE(reused.entity == second_local);
    REQUIRE(reused.client_entity_network_id == second_id);
}
