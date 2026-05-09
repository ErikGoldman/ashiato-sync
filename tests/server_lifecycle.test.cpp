#include "test_protocol.hpp"

#include "kage/sync/simulated_link.hpp"

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

using namespace kage_sync_tests;

namespace {

class TestFrameConsumer final : public kage::sync::ServerRegistryDirtyFrameListener {
public:
    explicit TestFrameConsumer(std::function<void(const kage::sync::ServerRegistryDirtyFrame&)> on_delta)
        : on_delta_(std::move(on_delta)) {}

    void on_server_registry_dirty_frame(const kage::sync::ServerRegistryDirtyFrame& frame) override {
        on_delta_(frame);
    }

private:
    std::function<void(const kage::sync::ServerRegistryDirtyFrame&)> on_delta_;
};

}  // namespace

TEST_CASE("replication server tracks clients and replicated component changes") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();

    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(server_options);

    REQUIRE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(kage::sync::invalid_client_id));
    REQUIRE(server.has_client(7));
    REQUIRE(server.client_count() == 1);

    REQUIRE_FALSE(start_sync(registry, ecs::Entity{}, archetype));
    REQUIRE(start_sync(registry, entity, kage::sync::SyncArchetypeId{999}));
    server.rediscover_all_replicated_entities(registry);
    REQUIRE_FALSE(server.is_replicated(entity));
    REQUIRE(start_sync(registry, entity, archetype));
    server.rediscover_all_replicated_entities(registry);
    REQUIRE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 1);
    REQUIRE(registry.contains<kage::sync::Replicated>(entity));

    REQUIRE(registry.remove<kage::sync::Replicated>(entity));
    server.rediscover_all_replicated_entities(registry);
    REQUIRE_FALSE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 0);
    REQUIRE_FALSE(registry.contains<kage::sync::Replicated>(entity));
    REQUIRE_FALSE(registry.remove<kage::sync::Replicated>(entity));

    REQUIRE(server.remove_client(7));
    REQUIRE_FALSE(server.has_client(7));
}

TEST_CASE("replication server allocates one local client and skips remote id collisions") {
    kage::sync::ReplicationServerOptions options;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);

    REQUIRE(server.add_client(1));
    const kage::sync::ClientId local = server.add_local_client();
    REQUIRE(local == 2);
    REQUIRE(server.local_client() == local);
    REQUIRE(server.is_local_client(local));
    REQUIRE_FALSE(server.is_local_client(1));
    REQUIRE(server.has_client(local));
    REQUIRE(server.client_count() == 2);
    REQUIRE(server.add_local_client() == kage::sync::invalid_client_id);
}

TEST_CASE("local-only replication server tick does not require transport and ignores idle timeout") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 1.0;
    kage::sync::ReplicationServer server(options);
    const kage::sync::ClientId local = server.add_local_client();
    REQUIRE(local != kage::sync::invalid_client_id);

    server.tick(registry, server.options().fixed_dt_seconds);
    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(server.has_client(local));
    REQUIRE(server.client_count() == 1);
}

TEST_CASE("replication server disconnects clients after configured idle timeout") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 2.0;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(server.has_client(1));
    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE_FALSE(server.has_client(1));
}

TEST_CASE("replication server idle timeout uses elapsed tick dt") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 0.25;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    REQUIRE(server.tick(registry, 0.125));
    REQUIRE(server.has_client(1));
    REQUIRE(server.frame() == 0);

    REQUIRE(server.tick(registry, 0.125));
    REQUIRE_FALSE(server.has_client(1));
    REQUIRE(server.frame() == 0);
}

TEST_CASE("replication server resets idle timeout when a client sends packets") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 2.0;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(server.has_client(1));

    ecs::BitBuffer ack;
    ack.push_bits(kage::sync::protocol::client_ack_message, 8U);
    ack.push_bits(0, 16U);
    REQUIRE(server.process_packet(registry, 1, ack));

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(server.has_client(1));
    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE_FALSE(server.has_client(1));
}

TEST_CASE("replication server tick requires a transport callback") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServer server;
    REQUIRE(server.add_client(1));

    REQUIRE_THROWS_AS(server.tick(registry, server.options().fixed_dt_seconds), std::logic_error);
}

TEST_CASE("replication server continuous tick owns the fixed-step accumulator") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    struct TickCounter {
        int frames = 0;
    };
    registry.register_component<TickCounter>("TickCounter");
    const ecs::Entity counter = registry.create();
    REQUIRE(registry.add<TickCounter>(counter, TickCounter{}) != nullptr);
    registry.job<TickCounter>(0).each([](ecs::Entity, TickCounter& state) {
        ++state.frames;
    });

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 0.25;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);

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
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    struct TickCounter {
        int frames = 0;
    };
    registry.register_component<TickCounter>("TickCounter");
    const ecs::Entity counter = registry.create();
    REQUIRE(registry.add<TickCounter>(counter, TickCounter{}) != nullptr);
    registry.job<TickCounter>(0).each([](ecs::Entity, TickCounter& state) {
        ++state.frames;
    });

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 0.25;
    options.max_fixed_steps_per_tick = 2;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);

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
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    std::vector<kage::sync::SyncFrame> frames;
    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 0.25;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);
    TestFrameConsumer consumer([&frames, &server](const kage::sync::ServerRegistryDirtyFrame& frame) {
        REQUIRE(frame.registry.get<kage::sync::SyncSettings>().role == kage::sync::SyncRole::Server);
        REQUIRE(frame.cues.empty());
        frames.push_back(server.frame());
    });
    auto subscription = server.subscribe_registry_dirty_frame_listener(consumer);

    REQUIRE(server.tick(registry, 0.125));
    REQUIRE(frames.empty());

    REQUIRE(server.tick(registry, 0.5));
    REQUIRE(frames == std::vector<kage::sync::SyncFrame>{1, 2});

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(frames == std::vector<kage::sync::SyncFrame>{1, 2, 3});

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(frames == std::vector<kage::sync::SyncFrame>{1, 2, 3, 4});

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(frames == std::vector<kage::sync::SyncFrame>{1, 2, 3, 4, 5});
}

TEST_CASE("replication server frame consumer receives cues drained for the frame") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const kage::sync::SyncCueTypeId cue_type = kage::sync::register_sync_cue<TestCue>(registry);
    const ecs::Entity entity = registry.create();
    registry.add<Position>(entity, Position{1.0f, 2.0f});
    REQUIRE(start_sync(registry, entity, archetype));

    struct SeenCue {
        ecs::Entity entity;
        kage::sync::SyncFrame frame = 0;
        kage::sync::SyncCueTypeId type = 0;
        float relevance_seconds = 0.0f;
        bool only_owner = false;
        std::int32_t id = 0;
    };
    std::vector<SeenCue> seen;

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 0.25;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);
    TestFrameConsumer consumer([&seen](const kage::sync::ServerRegistryDirtyFrame& frame) {
        for (const kage::sync::QueuedSyncCue& cue : frame.cues) {
            ecs::BitBuffer payload = cue.payload;
            seen.push_back(SeenCue{
                cue.entity,
                cue.frame,
                cue.type,
                cue.relevance_seconds,
                cue.only_replicate_to_owner,
                static_cast<std::int32_t>(payload.read_bits(16U))});
        }
    });
    auto subscription = server.subscribe_registry_dirty_frame_listener(consumer);

    REQUIRE(kage_sync_tests::emit_test_cue(registry, entity, kage::sync::SyncFrame{1}, TestCue{42}, 0.5f, true));
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
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    registry.add<Position>(entity, Position{1.0f, 2.0f});
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<kage::sync::ServerDestroyedReplicatedSlot> destroyed;
    kage::sync::ReplicationServer server;
    TestFrameConsumer consumer([&](const kage::sync::ServerRegistryDirtyFrame& frame) {
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
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    (void)define_position_archetype(registry);

    const ecs::Entity entity = registry.create();
    registry.add<Position>(entity, Position{1.0f, 2.0f});

    kage::sync::ReplicationServer server;
    std::vector<int> first_subscriber_counts;
    std::vector<int> second_subscriber_counts;
    TestFrameConsumer first_consumer([&](const kage::sync::ServerRegistryDirtyFrame& frame) {
        int count = 0;
        frame.dirty.each_dirty<Position>([&](ecs::Entity, const void*) {
            ++count;
        });
        first_subscriber_counts.push_back(count);
    });
    TestFrameConsumer second_consumer([&](const kage::sync::ServerRegistryDirtyFrame& frame) {
        int count = 0;
        frame.dirty.each_dirty<Position>([&](ecs::Entity, const void*) {
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
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    int frames = 0;
    kage::sync::ReplicationServerOptions options;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);
    TestFrameConsumer consumer([&frames](const kage::sync::ServerRegistryDirtyFrame&) {
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
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    int frames = 0;
    kage::sync::ReplicationServerOptions options;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);
    kage::sync::ServerRegistryDirtyFrameSubscription* active_subscription = nullptr;
    TestFrameConsumer consumer([&](const kage::sync::ServerRegistryDirtyFrame&) {
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
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    registry.add<Position>(entity, Position{1.0f, 2.0f});
    REQUIRE(start_sync(registry, entity, archetype));

    registry.job<Position>(0).each([](ecs::Entity, Position& position) {
        position.x += 1.0f;
    });

    std::vector<ecs::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 0.25;
    options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& payload) {
        REQUIRE(client == 1);
        payloads.push_back(payload);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds * 3.0));

    REQUIRE(server.frame() == 3U);
    REQUIRE(payloads.size() == 1U);
    ecs::BitBuffer update = payloads.back();
    REQUIRE(static_cast<std::uint8_t>(update.read_bits(8U)) == kage::sync::protocol::server_update_message);
    REQUIRE(static_cast<kage::sync::SyncFrame>(update.read_bits(32U)) == 3U);
}

TEST_CASE("listen server plays queued cues locally") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    registry.register_component<CuePlayback>("CuePlayback");
    (void)kage::sync::register_sync_cue<TestCue>(registry);

    const ecs::Entity entity = registry.create();
    registry.add<Position>(entity, Position{1.0f, 2.0f});
    REQUIRE(start_sync(registry, entity, archetype));

    kage::sync::ReplicationServer server;
    REQUIRE(server.add_local_client() != kage::sync::invalid_client_id);
    REQUIRE(kage_sync_tests::emit_test_cue(registry, entity, kage::sync::SyncFrame{1}, TestCue{77}, 0.5f));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(registry.contains<CuePlayback>(entity));
    REQUIRE(registry.get<CuePlayback>(entity).plays == 1);
    REQUIRE(registry.get<CuePlayback>(entity).last_id == 77);
}

TEST_CASE("listen server resolves entity references while playing queued cues locally") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    registry.register_component<CuePlayback>("CuePlayback");
    (void)kage::sync::register_sync_cue<ReferenceCue>(registry);

    const ecs::Entity owner = registry.create();
    registry.add<Position>(owner, Position{1.0f, 2.0f});
    REQUIRE(start_sync(registry, owner, archetype));

    const ecs::Entity target = registry.create();
    registry.add<Position>(target, Position{3.0f, 4.0f});
    REQUIRE(start_sync(registry, target, archetype));

    kage::sync::ReplicationServer server;
    const kage::sync::ClientId local = server.add_local_client();
    REQUIRE(local != kage::sync::invalid_client_id);
    REQUIRE(kage_sync_tests::emit_test_cue(
        registry,
        owner,
        kage::sync::SyncFrame{1},
        ReferenceCue{kage::sync::EntityReference{target}},
        0.5f));

    server.tick(registry, server.options().fixed_dt_seconds);

    REQUIRE(registry.contains<CuePlayback>(owner));
    REQUIRE(registry.get<CuePlayback>(owner).plays == 1);
    REQUIRE(registry.get<CuePlayback>(owner).last_target == target);
    REQUIRE(kage::sync::client_entity_network_id_client(
                registry.get<CuePlayback>(owner).last_target_network_id) == local);
}

TEST_CASE("replication server frame advances once per completed fixed tick") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServerOptions options;
    options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.frame() == 0);

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(server.frame() == 1);

    server.tick(registry, server.options().fixed_dt_seconds);
    REQUIRE(server.frame() == 2);
}

TEST_CASE("replication server queued receive packets are processed after clock advancement") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    std::vector<ecs::BitBuffer> sent;
    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        sent.push_back(packet);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    ecs::BitBuffer ping;
    ping.push_bits(kage::sync::protocol::client_ping_message, 8U);
    ping.push_bits(7U, 32U);
    ping.push_bits(3U, 32U);
    ping.push_bits(0U, kage::sync::protocol::frame_subframe_bits);
    server.receive_packet(1, ping);

    REQUIRE(server.tick(registry, 0.5));
    REQUIRE(sent.size() == 1);
    ecs::BitBuffer pong = sent[0];
    REQUIRE(static_cast<std::uint8_t>(pong.read_bits(8U)) == kage::sync::protocol::server_pong_message);
    REQUIRE(static_cast<std::uint32_t>(pong.read_bits(32U)) == 7U);
    REQUIRE(static_cast<kage::sync::SyncFrame>(pong.read_bits(32U)) == 3U);
    REQUIRE(static_cast<std::uint16_t>(pong.read_bits(kage::sync::protocol::frame_subframe_bits)) == 0U);
    REQUIRE(static_cast<kage::sync::SyncFrame>(pong.read_bits(32U)) == 0U);
    const auto server_subframe = static_cast<std::uint16_t>(pong.read_bits(kage::sync::protocol::frame_subframe_bits));
    REQUIRE(server_subframe == kage::sync::protocol::frame_subframe_scale / 2U);
}
