#include "test_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

std::vector<kage::sync::ReplicationReplayFrame> record_frames(ecs::Registry& registry, kage::sync::ReplicationServer& server) {
    std::vector<kage::sync::ReplicationReplayFrame> frames;
    kage::sync::ReplicationReplayWriter writer({60, [&](kage::sync::ReplicationReplayFrame frame) {
        frames.push_back(std::move(frame));
    }});
    writer.attach(server);
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    writer.detach();
    return frames;
}

ecs::Entity single_replayed_entity(ecs::Registry& registry) {
    ecs::Entity found;
    registry.view<const kage::sync::Replicated>().each([&found](ecs::Entity entity, const kage::sync::Replicated&) {
        found = entity;
    });
    return found;
}

}  // namespace

TEST_CASE("replication replay streamer restores full replicated state") {
    ecs::Registry source;
    kage_sync_tests::configure_test_server_registry(source);
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(source);
    const ecs::Entity entity = source.create();
    source.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(kage_sync_tests::start_sync(source, entity, archetype));

    kage::sync::ReplicationServer source_server(source);
    std::vector<kage::sync::ReplicationReplayFrame> frames = record_frames(source, source_server);
    REQUIRE(frames.size() == 1U);

    ecs::Registry playback;
    kage_sync_tests::configure_test_server_registry(playback);
    (void)kage_sync_tests::define_position_archetype(playback);

    kage::sync::ReplicationReplayStreamer streamer;
    kage::sync::ReplicationReplayStreamSession session;
    streamer.push_frame(frames[0]);
    REQUIRE(streamer.apply_frame(frames[0], playback, session));

    const ecs::Entity replayed = single_replayed_entity(playback);
    REQUIRE(replayed);
    REQUIRE(playback.get<kage_sync_tests::Position>(replayed).x == 1.0f);
    REQUIRE(playback.get<kage_sync_tests::Position>(replayed).y == 2.0f);
}

TEST_CASE("replication replay streamer applies deltas and destroys") {
    ecs::Registry source;
    kage_sync_tests::configure_test_server_registry(source);
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(source);
    const ecs::Entity entity = source.create();
    source.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(kage_sync_tests::start_sync(source, entity, archetype));

    std::vector<kage::sync::ReplicationReplayFrame> frames;
    kage::sync::ReplicationReplayWriter writer({60, [&](kage::sync::ReplicationReplayFrame frame) {
        frames.push_back(std::move(frame));
    }});
    kage::sync::ReplicationServer source_server(source);
    writer.attach(source_server);
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    source.write<kage_sync_tests::Position>(entity).x = 5.0f;
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(source.destroy(entity));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    writer.detach();
    REQUIRE(frames.size() == 3U);

    ecs::Registry playback;
    kage_sync_tests::configure_test_server_registry(playback);
    (void)kage_sync_tests::define_position_archetype(playback);

    kage::sync::ReplicationReplayStreamer streamer;
    kage::sync::ReplicationReplayStreamSession session;
    REQUIRE(streamer.apply_frame(frames[0], playback, session));
    ecs::Entity replayed = single_replayed_entity(playback);
    REQUIRE(replayed);

    REQUIRE(streamer.apply_frame(frames[1], playback, session));
    REQUIRE(playback.get<kage_sync_tests::Position>(replayed).x == 5.0f);

    REQUIRE(streamer.apply_frame(frames[2], playback, session));
    REQUIRE_FALSE(playback.alive(replayed));
}

TEST_CASE("replication replay streamer restores reference cues") {
    ecs::Registry source;
    kage_sync_tests::configure_test_server_registry(source);
    const ecs::Entity position_component =
        kage::sync::register_sync_component<kage_sync_tests::Position>(source, "Position");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        source,
        "PositionActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    (void)kage::sync::register_sync_cue<kage_sync_tests::ReferenceCue>(source);

    const ecs::Entity owner = source.create();
    source.add<kage_sync_tests::Position>(owner, kage_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(kage_sync_tests::start_sync(source, owner, archetype));
    const ecs::Entity target = source.create();
    source.add<kage_sync_tests::Position>(target, kage_sync_tests::Position{3.0f, 4.0f});
    REQUIRE(kage_sync_tests::start_sync(source, target, archetype));

    std::vector<kage::sync::ReplicationReplayFrame> frames;
    kage::sync::ReplicationReplayWriter writer({60, [&](kage::sync::ReplicationReplayFrame frame) {
        frames.push_back(std::move(frame));
    }});
    kage::sync::ReplicationServer source_server(source);
    writer.attach(source_server);
    REQUIRE(kage_sync_tests::emit_test_cue(
        source,
        owner,
        kage::sync::SyncFrame{1},
        kage_sync_tests::ReferenceCue{kage::sync::EntityReference{target}},
        0.5f));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    writer.detach();
    REQUIRE(frames.size() == 1U);

    ecs::Registry playback;
    kage_sync_tests::configure_test_server_registry(playback);
    playback.register_component<kage_sync_tests::CuePlayback>("CuePlayback");
    const ecs::Entity playback_position =
        kage::sync::register_sync_component<kage_sync_tests::Position>(playback, "Position");
    (void)kage::sync::define_archetype(
        playback,
        "PositionActor",
        {{playback_position, kage::sync::ReplicationAudience::All}});
    (void)kage::sync::register_sync_cue<kage_sync_tests::ReferenceCue>(playback);

    kage::sync::ReplicationReplayStreamer streamer;
    kage::sync::ReplicationReplayStreamSession session;
    REQUIRE(streamer.apply_frame(frames[0], playback, session));

    kage::sync::ReplicationServer playback_server(playback);
    REQUIRE(playback_server.add_local_client(playback) != kage::sync::invalid_client_id);
    REQUIRE(playback_server.advance_frame_without_simulating(playback, frames[0].frame));

    bool played = false;
    playback.view<const kage_sync_tests::CuePlayback>().each(
        [&played](ecs::Entity, const kage_sync_tests::CuePlayback& playback_state) {
            played = playback_state.plays == 1 && playback_state.last_target != ecs::Entity{};
        });
    REQUIRE(played);
}

TEST_CASE("replication replay streamer preserves cue frames when replay frames are flushed at a lower rate") {
    ecs::Registry source;
    kage_sync_tests::configure_test_server_registry(source);
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(source);
    (void)kage::sync::register_sync_cue<kage_sync_tests::TestCue>(source);

    const ecs::Entity entity = source.create();
    source.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(kage_sync_tests::start_sync(source, entity, archetype));

    kage::sync::ReplicationReplayStreamer streamer({3, 0, 8});
    std::vector<kage::sync::ReplicationReplayFrame> replay_frames;
    kage::sync::ReplicationReplayWriter writer({60, [&](kage::sync::ReplicationReplayFrame frame) {
        replay_frames.push_back(std::move(frame));
    }, 3});

    kage::sync::ReplicationServer source_server(source);
    writer.attach(source_server);
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(replay_frames.size() == 1U);
    REQUIRE(replay_frames[0].frame == 1U);

    REQUIRE(kage_sync_tests::emit_test_cue(
        source,
        entity,
        kage::sync::SyncFrame{2},
        kage_sync_tests::TestCue{99},
        0.5f));
    source.write<kage_sync_tests::Position>(entity).x = 3.0f;
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(replay_frames.size() == 1U);

    source.write<kage_sync_tests::Position>(entity).x = 4.0f;
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));

    REQUIRE(replay_frames.size() == 2U);
    REQUIRE(replay_frames[1].frame == 3U);
    for (kage::sync::ReplicationReplayFrame& frame : replay_frames) {
        streamer.push_frame(std::move(frame));
    }
    replay_frames.clear();

    ecs::Registry playback;
    kage_sync_tests::configure_test_server_registry(playback);
    playback.register_component<kage_sync_tests::CuePlayback>("CuePlayback");
    (void)kage_sync_tests::define_position_archetype(playback);
    (void)kage::sync::register_sync_cue<kage_sync_tests::TestCue>(playback);

    kage::sync::ReplicationServer playback_server(playback);
    REQUIRE(playback_server.add_local_client(playback) != kage::sync::invalid_client_id);

    kage::sync::ReplicationReplayStreamSession session;
    REQUIRE(streamer.begin_session(3U, playback, playback_server, session));

    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage::sync::FrameInfo>().frame == 1U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage::sync::FrameInfo>().frame == 2U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage::sync::FrameInfo>().frame == 3U);

    const ecs::Entity replayed = single_replayed_entity(playback);
    REQUIRE(replayed);
    REQUIRE(playback.contains<kage_sync_tests::CuePlayback>(replayed));
    const kage_sync_tests::CuePlayback& cue_playback = playback.get<kage_sync_tests::CuePlayback>(replayed);
    REQUIRE(cue_playback.plays == 1);
    REQUIRE(cue_playback.last_id == 99);
    REQUIRE(cue_playback.last_frame == 2U);

    source.write<kage_sync_tests::Position>(entity).x = 6.0f;
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(replay_frames.size() == 1U);
    REQUIRE(replay_frames[0].frame == 6U);
    streamer.push_frame(std::move(replay_frames[0]));
    replay_frames.clear();

    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage::sync::FrameInfo>().frame == 4U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage::sync::FrameInfo>().frame == 5U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage::sync::FrameInfo>().frame == 6U);

    source.write<kage_sync_tests::Position>(entity).x = 9.0f;
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    writer.detach();
    REQUIRE(replay_frames.size() == 1U);
    REQUIRE(replay_frames[0].frame == 9U);
    streamer.push_frame(std::move(replay_frames[0]));
    replay_frames.clear();

    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage::sync::FrameInfo>().frame == 7U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage::sync::FrameInfo>().frame == 8U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage::sync::FrameInfo>().frame == 9U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage::sync::FrameInfo>().frame == 10U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage::sync::FrameInfo>().frame == 11U);
    REQUIRE_FALSE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<kage_sync_tests::CuePlayback>(replayed).plays == 1);
    REQUIRE(playback.get<kage_sync_tests::Position>(replayed).x == 9.0f);
}
