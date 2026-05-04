#include "test_protocol.hpp"

#include "kage/sync/simulated_link.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

TEST_CASE("replication server tracks clients and replicated component changes") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();

    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [](kage::sync::ClientId, const kage::sync::BitBuffer&) {};
    kage::sync::ReplicationServer server(server_options);

    REQUIRE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(kage::sync::invalid_client_id));
    REQUIRE(server.has_client(7));
    REQUIRE(server.client_count() == 1);

    REQUIRE_FALSE(start_sync(registry, ecs::Entity{}, archetype));
    REQUIRE(start_sync(registry, entity, kage::sync::SyncArchetypeId{999}));
    server.refresh_replicated(registry);
    REQUIRE_FALSE(server.is_replicated(entity));
    REQUIRE(start_sync(registry, entity, archetype));
    server.refresh_replicated(registry);
    REQUIRE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 1);
    REQUIRE(registry.contains<kage::sync::Replicated>(entity));

    REQUIRE(registry.remove<kage::sync::Replicated>(entity));
    server.refresh_replicated(registry);
    REQUIRE_FALSE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 0);
    REQUIRE_FALSE(registry.contains<kage::sync::Replicated>(entity));
    REQUIRE_FALSE(registry.remove<kage::sync::Replicated>(entity));

    REQUIRE(server.remove_client(7));
    REQUIRE_FALSE(server.has_client(7));
}

TEST_CASE("replication server disconnects clients after configured idle timeout") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 2.0;
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    server.tick(registry);
    REQUIRE(server.has_client(1));
    server.tick(registry);
    REQUIRE_FALSE(server.has_client(1));
}

TEST_CASE("replication server resets idle timeout when a client sends packets") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.idle_client_timeout_seconds = 2.0;
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    server.tick(registry);
    REQUIRE(server.has_client(1));

    kage::sync::BitBuffer ack;
    ack.push_bits(kage::sync::protocol::client_ack_message, 8U);
    ack.push_bits(0, 16U);
    REQUIRE(server.process_packet(1, ack));

    server.tick(registry);
    REQUIRE(server.has_client(1));
    server.tick(registry);
    REQUIRE_FALSE(server.has_client(1));
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

TEST_CASE("replication server post tick callback fires once per completed fixed step") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    std::vector<kage::sync::SyncFrame> frames;
    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 0.25;
    options.post_tick = [&frames](
        const ecs::Registry& callback_registry,
        kage::sync::SyncFrame frame,
        kage::sync::QueuedSyncCueView cues) {
        REQUIRE(callback_registry.get<kage::sync::SyncSettings>().role == kage::sync::SyncRole::Server);
        REQUIRE(cues.empty());
        frames.push_back(frame);
    };
    kage::sync::ReplicationServer server(options);

    REQUIRE(server.tick(registry, 0.125));
    REQUIRE(frames.empty());

    REQUIRE(server.tick(registry, 0.5));
    REQUIRE(frames == std::vector<kage::sync::SyncFrame>{1, 2});

    server.tick(registry);
    REQUIRE(frames == std::vector<kage::sync::SyncFrame>{1, 2, 3});

    server.tick(registry, [](kage::sync::ClientId, ecs::Entity) {});
    REQUIRE(frames == std::vector<kage::sync::SyncFrame>{1, 2, 3, 4});

    server.begin_tick(registry);
    server.end_tick(registry);
    REQUIRE(frames == std::vector<kage::sync::SyncFrame>{1, 2, 3, 4});
}

TEST_CASE("replication server post tick callback receives cues drained for the frame") {
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
    options.post_tick = [&seen](const ecs::Registry&, kage::sync::SyncFrame, kage::sync::QueuedSyncCueView cues) {
        for (const kage::sync::QueuedSyncCue& cue : cues) {
            kage::sync::BitBuffer payload = cue.payload;
            seen.push_back(SeenCue{
                cue.entity,
                cue.frame,
                cue.type,
                cue.relevance_seconds,
                cue.only_replicate_to_owner,
                static_cast<std::int32_t>(payload.read_bits(16U))});
        }
    };
    kage::sync::ReplicationServer server(options);

    REQUIRE(kage::sync::emit_cue(registry, entity, TestCue{42}, 0.5f, true));
    server.tick(registry);

    REQUIRE(seen.size() == 1U);
    REQUIRE(seen[0].entity == entity);
    REQUIRE(seen[0].frame == 1U);
    REQUIRE(seen[0].type == cue_type);
    REQUIRE(seen[0].relevance_seconds == 0.5f);
    REQUIRE(seen[0].only_owner);
    REQUIRE(seen[0].id == 42);
}

TEST_CASE("replication server frame is the currently simulating frame") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    kage::sync::ReplicationServer server;
    REQUIRE(server.frame() == 0);

    server.begin_tick(registry);
    REQUIRE(server.frame() == 1);
    server.end_tick(registry);
    REQUIRE(server.frame() == 1);

    server.begin_tick(registry);
    REQUIRE(server.frame() == 2);
    server.end_tick(registry);
    REQUIRE(server.frame() == 2);
}

TEST_CASE("replication server queued receive packets are processed after clock advancement") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);

    std::vector<kage::sync::BitBuffer> sent;
    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        sent.push_back(packet);
    };
    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    kage::sync::BitBuffer ping;
    ping.push_bits(kage::sync::protocol::client_ping_message, 8U);
    ping.push_bits(7U, 32U);
    ping.push_bits(3U, 32U);
    ping.push_bits(0U, kage::sync::protocol::frame_subframe_bits);
    server.receive_packet(1, ping);

    REQUIRE(server.tick(registry, 0.5));
    REQUIRE(sent.size() == 1);
    kage::sync::BitBuffer pong = sent[0];
    REQUIRE(static_cast<std::uint8_t>(pong.read_bits(8U)) == kage::sync::protocol::server_pong_message);
    REQUIRE(static_cast<std::uint32_t>(pong.read_bits(32U)) == 7U);
    REQUIRE(static_cast<kage::sync::SyncFrame>(pong.read_bits(32U)) == 3U);
    REQUIRE(static_cast<std::uint16_t>(pong.read_bits(kage::sync::protocol::frame_subframe_bits)) == 0U);
    REQUIRE(static_cast<kage::sync::SyncFrame>(pong.read_bits(32U)) == 0U);
    const auto server_subframe = static_cast<std::uint16_t>(pong.read_bits(kage::sync::protocol::frame_subframe_bits));
    REQUIRE(server_subframe == kage::sync::protocol::frame_subframe_scale / 2U);
}
