#include "test_protocol.hpp"

#include "ashiato/sync/simulated_link.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ashiato_sync_tests;

namespace {

class TestFrameConsumer final : public ashiato::sync::ServerRegistryDirtyFrameListener {
public:
    explicit TestFrameConsumer(std::function<void(const ashiato::sync::ServerRegistryDirtyFrame&)> on_delta)
        : on_delta_(std::move(on_delta)) {}

    void on_server_registry_dirty_frame(const ashiato::sync::ServerRegistryDirtyFrame& frame) override {
        on_delta_(frame);
    }

private:
    std::function<void(const ashiato::sync::ServerRegistryDirtyFrame&)> on_delta_;
};

}  // namespace

TEST_CASE("replication server tracks clients and replicated component changes") {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ashiato::Entity entity = registry.create();

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, server_options);

    REQUIRE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(ashiato::sync::invalid_client_id));
    REQUIRE(server.has_client(7));
    REQUIRE(server.client_count() == 1);

    REQUIRE_FALSE(start_sync(registry, ashiato::Entity{}, archetype));
    REQUIRE(start_sync(registry, entity, ashiato::sync::SyncArchetypeId{999}));
    server.rediscover_all_replicated_entities(registry);
    REQUIRE_FALSE(server.is_replicated(entity));
    REQUIRE(start_sync(registry, entity, archetype));
    server.rediscover_all_replicated_entities(registry);
    REQUIRE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 1);
    REQUIRE(registry.contains<ashiato::sync::Replicated>(entity));

    REQUIRE(registry.remove<ashiato::sync::Replicated>(entity));
    server.rediscover_all_replicated_entities(registry);
    REQUIRE_FALSE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 0);
    REQUIRE_FALSE(registry.contains<ashiato::sync::Replicated>(entity));
    REQUIRE_FALSE(registry.remove<ashiato::sync::Replicated>(entity));

    REQUIRE(server.remove_client(registry, 7));
    REQUIRE_FALSE(server.has_client(7));
}

TEST_CASE("replication server discovers replicated entities added after initialization") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = define_position_archetype(registry);

    ashiato::sync::ReplicationServer server(registry);
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    REQUIRE(server.replicated_count() == 0U);

    const ashiato::Entity entity = registry.create();
    registry.add<Position>(entity, Position{1.0f, 2.0f});
    REQUIRE(start_sync(registry, entity, archetype));

    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    REQUIRE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 1U);
}

TEST_CASE("replication server allocates one local client and skips remote id collisions") {
    ashiato::Registry registry;
    ashiato::sync::ReplicationServerOptions options;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);

    REQUIRE(server.add_client(1));
    const ashiato::sync::ClientId local = server.add_local_client(registry);
    REQUIRE(local == 2);
    REQUIRE(server.local_client() == local);
    REQUIRE(registry.get<ashiato::sync::SyncSettings>().local_client == local);
    REQUIRE(server.is_local_client(local));
    REQUIRE_FALSE(server.is_local_client(1));
    REQUIRE(server.has_client(local));
    REQUIRE(server.client_count() == 2);
    REQUIRE(server.add_local_client(registry) == ashiato::sync::invalid_client_id);
    REQUIRE(registry.get<ashiato::sync::SyncSettings>().local_client == local);
    REQUIRE(server.remove_client(registry, local));
    REQUIRE(server.local_client() == ashiato::sync::invalid_client_id);
    REQUIRE(registry.get<ashiato::sync::SyncSettings>().local_client == ashiato::sync::invalid_client_id);
}

TEST_CASE("local-only replication server tick does not require transport and ignores idle timeout") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 1.0;
    ashiato::sync::ReplicationServer server(registry, options);
    const ashiato::sync::ClientId local = server.add_local_client(registry);
    REQUIRE(local != ashiato::sync::invalid_client_id);
    REQUIRE(registry.get<ashiato::sync::SyncSettings>().local_client == local);

    server.tick(registry, server.options().fixed_dt_seconds);
    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(server.has_client(local));
    REQUIRE(server.client_count() == 1);
}

TEST_CASE("replication server disconnects clients after configured idle timeout") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 2.0;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(server.has_client(1));
    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE_FALSE(server.has_client(1));
}

TEST_CASE("replication server idle timeout uses elapsed tick dt") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 0.25;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));

    REQUIRE(server.tick(registry, 0.125));
    REQUIRE(server.has_client(1));
    REQUIRE(server.frame() == 0);

    REQUIRE(server.tick(registry, 0.125));
    REQUIRE_FALSE(server.has_client(1));
    REQUIRE(server.frame() == 0);
}

TEST_CASE("replication server resets idle timeout when a client sends packets") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 2.0;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(server.has_client(1));

    ashiato::BitBuffer ack;
    ack.write_bits(ashiato::sync::protocol::client_ack_message, ashiato::sync::protocol::message_bits);
    ack.write_bits(0, ashiato::sync::protocol::ack_count_bits);
    REQUIRE(server.process_packet(registry, 1, ack));

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(server.has_client(1));
    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE_FALSE(server.has_client(1));
}

TEST_CASE("replication server tick requires a transport callback") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    ashiato::sync::ReplicationServer server(registry);
    REQUIRE(server.add_client(1));

    REQUIRE_THROWS_AS(server.tick(registry, server.options().fixed_dt_seconds), std::logic_error);
}

TEST_CASE("replication server continuous tick owns the fixed-step accumulator") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    struct TickCounter {
        int frames = 0;
    };
    registry.register_component<TickCounter>("TickCounter");
    const ashiato::Entity counter = registry.create();
    REQUIRE(registry.add<TickCounter>(counter, TickCounter{}) != nullptr);
    registry.job<TickCounter>(0).each([](ashiato::Entity, TickCounter& state) {
        ++state.frames;
    });

    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 0.25;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);

    REQUIRE(server.tick(registry, 0.125));
    REQUIRE(registry.get<TickCounter>(counter).frames == 0);
    REQUIRE(server.frame() == 0);
    REQUIRE(server.accumulator_seconds() == 0.125);
    REQUIRE(server.continuous_frame() == 0.5);

    REQUIRE(server.tick(registry, 0.5));
    REQUIRE(registry.get<TickCounter>(counter).frames == 2);
    REQUIRE(server.frame() == 2);
    REQUIRE(server.accumulator_seconds() == 0.125);
    REQUIRE(server.continuous_frame() == 2.5);
}

TEST_CASE("replication server caps and drops continuous tick backlog when configured") {
    ashiato::Registry registry;

    struct TickCounter {
        int frames = 0;
    };
    registry.register_component<TickCounter>("TickCounter");
    const ashiato::Entity counter = registry.create();
    REQUIRE(registry.add<TickCounter>(counter, TickCounter{}) != nullptr);
    registry.job<TickCounter>(0).each([](ashiato::Entity, TickCounter& state) {
        ++state.frames;
    });

    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 0.25;
    options.max_fixed_steps_per_tick = 2;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);

    REQUIRE(server.tick(registry, 5.5 * server.options().fixed_dt_seconds));
    REQUIRE(registry.get<TickCounter>(counter).frames == 2);
    REQUIRE(server.frame() == 2);
    REQUIRE(server.accumulator_seconds() == Catch::Approx(0.125));
    REQUIRE(server.observability_stats().dropped_fixed_step_frames == 3);
    REQUIRE(server.observability_stats().fixed_step_overflow_events == 1);

    REQUIRE(server.tick(registry, 0.5 * server.options().fixed_dt_seconds));
    REQUIRE(registry.get<TickCounter>(counter).frames == 3);
    REQUIRE(server.frame() == 3);
    REQUIRE(server.observability_stats().dropped_fixed_step_frames == 3);
}

TEST_CASE("replication server frame consumer receives once per completed fixed step") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    std::vector<ashiato::sync::SyncFrame> frames;
    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 0.25;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);
    TestFrameConsumer consumer([&frames, &server](const ashiato::sync::ServerRegistryDirtyFrame& frame) {
        REQUIRE(frame.registry.get<ashiato::sync::SyncSettings>().role == ashiato::sync::SyncRole::Server);
        REQUIRE(frame.cues.empty());
        frames.push_back(server.frame());
    });
    auto subscription = server.subscribe_registry_dirty_frame_listener(consumer);

    REQUIRE(server.tick(registry, 0.125));
    REQUIRE(frames.empty());

    REQUIRE(server.tick(registry, 0.5));
    REQUIRE(frames == std::vector<ashiato::sync::SyncFrame>{1, 2});

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(frames == std::vector<ashiato::sync::SyncFrame>{1, 2, 3});

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(frames == std::vector<ashiato::sync::SyncFrame>{1, 2, 3, 4});

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(frames == std::vector<ashiato::sync::SyncFrame>{1, 2, 3, 4, 5});
}

TEST_CASE("replication server frame consumer receives cues drained for the frame") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ashiato::sync::SyncCueTypeId cue_type = ashiato::sync::register_sync_cue<TestCue>(registry);
    const ashiato::Entity entity = registry.create();
    registry.add<Position>(entity, Position{1.0f, 2.0f});
    REQUIRE(start_sync(registry, entity, archetype));

    struct SeenCue {
        ashiato::Entity entity;
        ashiato::sync::SyncFrame frame = 0;
        ashiato::sync::SyncCueTypeId type = 0;
        float relevance_seconds = 0.0f;
        bool only_owner = false;
        std::int32_t id = 0;
    };
    std::vector<SeenCue> seen;

    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 0.25;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);
    TestFrameConsumer consumer([&seen](const ashiato::sync::ServerRegistryDirtyFrame& frame) {
        for (const ashiato::sync::QueuedSyncCue& cue : frame.cues) {
            const TestCue* value = cue.value.has_value() ? static_cast<const TestCue*>(cue.value.data()) : nullptr;
            seen.push_back(SeenCue{
                cue.entity,
                cue.frame,
                cue.type,
                cue.relevance_seconds,
                cue.only_replicate_to_owner,
                value != nullptr ? value->id : 0});
        }
    });
    auto subscription = server.subscribe_registry_dirty_frame_listener(consumer);

    REQUIRE(ashiato_sync_tests::emit_test_cue(registry, entity, ashiato::sync::SyncFrame{1}, TestCue{42}, 0.5f, true));
    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(seen.size() == 1U);
    REQUIRE(seen[0].entity == entity);
    REQUIRE(seen[0].frame == 1U);
    REQUIRE(seen[0].type == cue_type);
    REQUIRE(seen[0].relevance_seconds == 0.5f);
    REQUIRE(seen[0].only_owner);
    REQUIRE(seen[0].id == 42);
}

TEST_CASE("replication server dirty frame exposes destroyed replicated slots after server bookkeeping") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ashiato::Entity entity = registry.create();
    registry.add<Position>(entity, Position{1.0f, 2.0f});
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<ashiato::sync::ServerDestroyedReplicatedSlot> destroyed;
    ashiato::sync::ReplicationServer server(registry);
    TestFrameConsumer consumer([&](const ashiato::sync::ServerRegistryDirtyFrame& frame) {
        destroyed.assign(frame.destroyed_slots.begin(), frame.destroyed_slots.end());
    });
    auto subscription = server.subscribe_registry_dirty_frame_listener(consumer);

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(destroyed.empty());

    REQUIRE(registry.destroy(entity));
    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(destroyed.size() == 1U);
    REQUIRE(destroyed[0].slot == 0U);
    REQUIRE(destroyed[0].entity == entity);
}

TEST_CASE("replication server frame consumers share one dirty frame") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    (void)define_position_archetype(registry);

    const ashiato::Entity entity = registry.create();
    registry.add<Position>(entity, Position{1.0f, 2.0f});

    ashiato::sync::ReplicationServer server(registry);
    std::vector<int> first_subscriber_counts;
    std::vector<int> second_subscriber_counts;
    TestFrameConsumer first_consumer([&](const ashiato::sync::ServerRegistryDirtyFrame& frame) {
        int count = 0;
        frame.dirty.each_dirty<Position>([&](ashiato::Entity, const void*) {
            ++count;
        });
        first_subscriber_counts.push_back(count);
    });
    TestFrameConsumer second_consumer([&](const ashiato::sync::ServerRegistryDirtyFrame& frame) {
        int count = 0;
        frame.dirty.each_dirty<Position>([&](ashiato::Entity, const void*) {
            ++count;
        });
        second_subscriber_counts.push_back(count);
    });
    auto first = server.subscribe_registry_dirty_frame_listener(first_consumer);
    auto second = server.subscribe_registry_dirty_frame_listener(second_consumer);

    server.tick(registry, server.options().fixed_dt_seconds);
    server.tick(registry, server.options().fixed_dt_seconds);
    registry.write<Position>(entity).x = 3.0f;
    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(first_subscriber_counts == std::vector<int>{1, 0, 1});
    REQUIRE(second_subscriber_counts == first_subscriber_counts);
}

TEST_CASE("replication server frame consumer subscription detaches") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    int frames = 0;
    ashiato::sync::ReplicationServerOptions options;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);
    TestFrameConsumer consumer([&frames](const ashiato::sync::ServerRegistryDirtyFrame&) {
        ++frames;
    });

    {
        auto subscription = server.subscribe_registry_dirty_frame_listener(consumer);
        REQUIRE(subscription.active());
        server.tick(registry, server.options().fixed_dt_seconds);
        REQUIRE(frames == 1);
        subscription.reset();
        REQUIRE_FALSE(subscription.active());
        server.tick(registry, server.options().fixed_dt_seconds);
        REQUIRE(frames == 1);
    }

    auto subscription = server.subscribe_registry_dirty_frame_listener(consumer);
    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(frames == 2);
}

TEST_CASE("replication server frame consumer can detach while receiving a frame") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    int frames = 0;
    ashiato::sync::ReplicationServerOptions options;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);
    ashiato::sync::ServerRegistryDirtyFrameSubscription* active_subscription = nullptr;
    TestFrameConsumer consumer([&](const ashiato::sync::ServerRegistryDirtyFrame&) {
        ++frames;
        active_subscription->reset();
    });
    auto subscription = server.subscribe_registry_dirty_frame_listener(consumer);
    active_subscription = &subscription;

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(frames == 1);
    REQUIRE_FALSE(subscription.active());

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(frames == 1);
}

TEST_CASE("replication server flushes client replication once after fixed-step catch-up") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ashiato::Entity entity = registry.create();
    registry.add<Position>(entity, Position{1.0f, 2.0f});
    REQUIRE(start_sync(registry, entity, archetype));

    registry.job<Position>(0).each([](ashiato::Entity, Position& position) {
        position.x += 1.0f;
    });

    std::vector<ashiato::BitBuffer> payloads;
    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 0.25;
    options.transport = [&](ashiato::sync::ClientId client, const ashiato::BitBuffer& payload) {
        REQUIRE(client == 1);
        payloads.push_back(payload);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));

    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds * 3.0));

    REQUIRE(server.frame() == 3U);
    REQUIRE(payloads.size() == 1U);
    ashiato::BitBuffer update = payloads.back();
    REQUIRE(static_cast<std::uint8_t>(update.read_bits(ashiato::sync::protocol::message_bits)) == ashiato::sync::protocol::server_update_message);
    REQUIRE(static_cast<ashiato::sync::SyncFrame>(update.read_bits(32U)) == 3U);
}

TEST_CASE("listen server plays queued cues locally") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    registry.register_component<CuePlayback>("CuePlayback");
    (void)ashiato::sync::register_sync_cue<TestCue>(registry);

    const ashiato::Entity entity = registry.create();
    registry.add<Position>(entity, Position{1.0f, 2.0f});
    REQUIRE(start_sync(registry, entity, archetype));

    ashiato::sync::ReplicationServer server(registry);
    REQUIRE(server.add_local_client(registry) != ashiato::sync::invalid_client_id);
    REQUIRE(ashiato_sync_tests::emit_test_cue(registry, entity, ashiato::sync::SyncFrame{1}, TestCue{77}, 0.5f));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(registry.contains<CuePlayback>(entity));
    REQUIRE(registry.get<CuePlayback>(entity).plays == 1);
    REQUIRE(registry.get<CuePlayback>(entity).last_id == 77);
}

TEST_CASE("listen server resolves entity references while playing queued cues locally") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    registry.register_component<CuePlayback>("CuePlayback");
    (void)ashiato::sync::register_sync_cue<ReferenceCue>(registry);

    const ashiato::Entity owner = registry.create();
    registry.add<Position>(owner, Position{1.0f, 2.0f});
    REQUIRE(start_sync(registry, owner, archetype));

    const ashiato::Entity target = registry.create();
    registry.add<Position>(target, Position{3.0f, 4.0f});
    REQUIRE(start_sync(registry, target, archetype));

    ashiato::sync::ReplicationServer server(registry);
    const ashiato::sync::ClientId local = server.add_local_client(registry);
    REQUIRE(local != ashiato::sync::invalid_client_id);
    REQUIRE(ashiato_sync_tests::emit_test_cue(
        registry,
        owner,
        ashiato::sync::SyncFrame{1},
        ReferenceCue{ashiato::sync::EntityReference{target}},
        0.5f));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(registry.contains<CuePlayback>(owner));
    REQUIRE(registry.get<CuePlayback>(owner).plays == 1);
    REQUIRE(registry.get<CuePlayback>(owner).last_target == target);
    REQUIRE(ashiato::sync::client_entity_network_id_client(
                registry.get<CuePlayback>(owner).last_target_network_id) == local);
}

TEST_CASE("replication server frame advances once per completed fixed tick") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    ashiato::sync::ReplicationServerOptions options;
    options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.frame() == 0);

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(server.frame() == 1);

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(server.frame() == 2);
}

TEST_CASE("replication server queued receive packets are processed after clock advancement") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);

    std::vector<ashiato::BitBuffer> sent;
    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        sent.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));

    ashiato::BitBuffer ping;
    ping.write_bits(ashiato::sync::protocol::client_ping_message, ashiato::sync::protocol::message_bits);
    ping.write_bits(7U, 32U);
    server.receive_packet(1, ping);

    REQUIRE(server.tick(registry, 0.5));
    REQUIRE(sent.size() == 1);
    ashiato::BitBuffer pong = sent[0];
    REQUIRE(static_cast<std::uint8_t>(pong.read_bits(ashiato::sync::protocol::message_bits)) == ashiato::sync::protocol::server_pong_message);
    REQUIRE(static_cast<std::uint32_t>(pong.read_bits(32U)) == 7U);
    REQUIRE(static_cast<ashiato::sync::SyncFrame>(pong.read_bits(32U)) == 0U);
    const auto server_receive_subframe =
        static_cast<std::uint16_t>(pong.read_bits(ashiato::sync::protocol::frame_subframe_bits));
    REQUIRE(server_receive_subframe == ashiato::sync::protocol::frame_subframe_scale / 2U);
    REQUIRE(static_cast<ashiato::sync::SyncFrame>(pong.read_bits(32U)) == 0U);
    const auto server_send_subframe =
        static_cast<std::uint16_t>(pong.read_bits(ashiato::sync::protocol::frame_subframe_bits));
    REQUIRE(server_send_subframe == ashiato::sync::protocol::frame_subframe_scale / 2U);
}
