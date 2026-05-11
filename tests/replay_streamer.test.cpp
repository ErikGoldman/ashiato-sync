#include "test_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

std::vector<ashiato::sync::ReplicationReplayFrame> record_frames(ashiato::Registry& registry, ashiato::sync::ReplicationServer& server) {
    std::vector<ashiato::sync::ReplicationReplayFrame> frames;
    ashiato::sync::ReplicationReplayWriter writer({60, [&](ashiato::sync::ReplicationReplayFrame frame) {
        frames.push_back(std::move(frame));
    }});
    writer.attach(server);
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    writer.detach();
    return frames;
}

ashiato::Entity single_replayed_entity(ashiato::Registry& registry) {
    ashiato::Entity found;
    registry.view<const ashiato::sync::Replicated>().each([&found](ashiato::Entity entity, const ashiato::sync::Replicated&) {
        found = entity;
    });
    return found;
}

}  // namespace

TEST_CASE("replication replay streamer restores full replicated state") {
    ashiato::Registry source;
    ashiato_sync_tests::configure_test_server_registry(source);
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(source);
    const ashiato::Entity entity = source.create();
    source.add<ashiato_sync_tests::Position>(entity, ashiato_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(ashiato_sync_tests::start_sync(source, entity, archetype));

    ashiato::sync::ReplicationServer source_server(source);
    std::vector<ashiato::sync::ReplicationReplayFrame> frames = record_frames(source, source_server);
    REQUIRE(frames.size() == 1U);

    ashiato::Registry playback;
    ashiato_sync_tests::configure_test_server_registry(playback);
    (void)ashiato_sync_tests::define_position_archetype(playback);

    ashiato::sync::ReplicationReplayStreamer streamer;
    ashiato::sync::ReplicationReplayStreamSession session;
    streamer.push_frame(frames[0]);
    REQUIRE(streamer.apply_frame(frames[0], playback, session));

    const ashiato::Entity replayed = single_replayed_entity(playback);
    REQUIRE(replayed);
    REQUIRE(playback.get<ashiato_sync_tests::Position>(replayed).x == 1.0f);
    REQUIRE(playback.get<ashiato_sync_tests::Position>(replayed).y == 2.0f);
}

TEST_CASE("replication replay streamer applies deltas and destroys") {
    ashiato::Registry source;
    ashiato_sync_tests::configure_test_server_registry(source);
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(source);
    const ashiato::Entity entity = source.create();
    source.add<ashiato_sync_tests::Position>(entity, ashiato_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(ashiato_sync_tests::start_sync(source, entity, archetype));

    std::vector<ashiato::sync::ReplicationReplayFrame> frames;
    ashiato::sync::ReplicationReplayWriter writer({60, [&](ashiato::sync::ReplicationReplayFrame frame) {
        frames.push_back(std::move(frame));
    }});
    ashiato::sync::ReplicationServer source_server(source);
    writer.attach(source_server);
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    source.write<ashiato_sync_tests::Position>(entity).x = 5.0f;
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(source.destroy(entity));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    writer.detach();
    REQUIRE(frames.size() == 3U);

    ashiato::Registry playback;
    ashiato_sync_tests::configure_test_server_registry(playback);
    (void)ashiato_sync_tests::define_position_archetype(playback);

    ashiato::sync::ReplicationReplayStreamer streamer;
    ashiato::sync::ReplicationReplayStreamSession session;
    REQUIRE(streamer.apply_frame(frames[0], playback, session));
    ashiato::Entity replayed = single_replayed_entity(playback);
    REQUIRE(replayed);

    REQUIRE(streamer.apply_frame(frames[1], playback, session));
    REQUIRE(playback.get<ashiato_sync_tests::Position>(replayed).x == 5.0f);

    REQUIRE(streamer.apply_frame(frames[2], playback, session));
    REQUIRE_FALSE(playback.alive(replayed));
}

TEST_CASE("replication replay streamer restores reference cues") {
    ashiato::Registry source;
    ashiato_sync_tests::configure_test_server_registry(source);
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<ashiato_sync_tests::Position>(source, "Position");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        source,
        "PositionActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    (void)ashiato::sync::register_sync_cue<ashiato_sync_tests::ReferenceCue>(source);

    const ashiato::Entity owner = source.create();
    source.add<ashiato_sync_tests::Position>(owner, ashiato_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(ashiato_sync_tests::start_sync(source, owner, archetype));
    const ashiato::Entity target = source.create();
    source.add<ashiato_sync_tests::Position>(target, ashiato_sync_tests::Position{3.0f, 4.0f});
    REQUIRE(ashiato_sync_tests::start_sync(source, target, archetype));

    std::vector<ashiato::sync::ReplicationReplayFrame> frames;
    ashiato::sync::ReplicationReplayWriter writer({60, [&](ashiato::sync::ReplicationReplayFrame frame) {
        frames.push_back(std::move(frame));
    }});
    ashiato::sync::ReplicationServer source_server(source);
    writer.attach(source_server);
    REQUIRE(ashiato_sync_tests::emit_test_cue(
        source,
        owner,
        ashiato::sync::SyncFrame{1},
        ashiato_sync_tests::ReferenceCue{ashiato::sync::EntityReference{target}},
        0.5f));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    writer.detach();
    REQUIRE(frames.size() == 1U);

    ashiato::Registry playback;
    ashiato_sync_tests::configure_test_server_registry(playback);
    playback.register_component<ashiato_sync_tests::CuePlayback>("CuePlayback");
    const ashiato::Entity playback_position =
        ashiato::sync::register_sync_component<ashiato_sync_tests::Position>(playback, "Position");
    (void)ashiato::sync::define_archetype(
        playback,
        "PositionActor",
        {{playback_position, ashiato::sync::ReplicationAudience::All}});
    (void)ashiato::sync::register_sync_cue<ashiato_sync_tests::ReferenceCue>(playback);

    ashiato::sync::ReplicationReplayStreamer streamer;
    ashiato::sync::ReplicationReplayStreamSession session;
    REQUIRE(streamer.apply_frame(frames[0], playback, session));

    ashiato::sync::ReplicationServer playback_server(playback);
    REQUIRE(playback_server.add_local_client(playback) != ashiato::sync::invalid_client_id);
    REQUIRE(playback_server.advance_frame_without_simulating(playback, frames[0].frame));

    bool played = false;
    playback.view<const ashiato_sync_tests::CuePlayback>().each(
        [&played](ashiato::Entity, const ashiato_sync_tests::CuePlayback& playback_state) {
            played = playback_state.plays == 1 && playback_state.last_target != ashiato::Entity{};
        });
    REQUIRE(played);
}

TEST_CASE("replication replay streamer preserves cue frames when replay frames are flushed at a lower rate") {
    ashiato::Registry source;
    ashiato_sync_tests::configure_test_server_registry(source);
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(source);
    (void)ashiato::sync::register_sync_cue<ashiato_sync_tests::TestCue>(source);

    const ashiato::Entity entity = source.create();
    source.add<ashiato_sync_tests::Position>(entity, ashiato_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(ashiato_sync_tests::start_sync(source, entity, archetype));

    ashiato::sync::ReplicationReplayStreamer streamer({3, 0, 8});
    std::vector<ashiato::sync::ReplicationReplayFrame> replay_frames;
    ashiato::sync::ReplicationReplayWriter writer({60, [&](ashiato::sync::ReplicationReplayFrame frame) {
        replay_frames.push_back(std::move(frame));
    }, 3});

    ashiato::sync::ReplicationServer source_server(source);
    writer.attach(source_server);
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(replay_frames.size() == 1U);
    REQUIRE(replay_frames[0].frame == 1U);

    REQUIRE(ashiato_sync_tests::emit_test_cue(
        source,
        entity,
        ashiato::sync::SyncFrame{2},
        ashiato_sync_tests::TestCue{99},
        0.5f));
    source.write<ashiato_sync_tests::Position>(entity).x = 3.0f;
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(replay_frames.size() == 1U);

    source.write<ashiato_sync_tests::Position>(entity).x = 4.0f;
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));

    REQUIRE(replay_frames.size() == 2U);
    REQUIRE(replay_frames[1].frame == 3U);
    for (ashiato::sync::ReplicationReplayFrame& frame : replay_frames) {
        streamer.push_frame(std::move(frame));
    }
    replay_frames.clear();

    ashiato::Registry playback;
    ashiato_sync_tests::configure_test_server_registry(playback);
    playback.register_component<ashiato_sync_tests::CuePlayback>("CuePlayback");
    (void)ashiato_sync_tests::define_position_archetype(playback);
    (void)ashiato::sync::register_sync_cue<ashiato_sync_tests::TestCue>(playback);

    ashiato::sync::ReplicationServer playback_server(playback);
    REQUIRE(playback_server.add_local_client(playback) != ashiato::sync::invalid_client_id);

    ashiato::sync::ReplicationReplayStreamSession session;
    REQUIRE(streamer.begin_session(3U, playback, playback_server, session));

    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato::sync::FrameInfo>().frame == 1U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato::sync::FrameInfo>().frame == 2U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato::sync::FrameInfo>().frame == 3U);

    const ashiato::Entity replayed = single_replayed_entity(playback);
    REQUIRE(replayed);
    REQUIRE(playback.contains<ashiato_sync_tests::CuePlayback>(replayed));
    const ashiato_sync_tests::CuePlayback& cue_playback = playback.get<ashiato_sync_tests::CuePlayback>(replayed);
    REQUIRE(cue_playback.plays == 1);
    REQUIRE(cue_playback.last_id == 99);
    REQUIRE(cue_playback.last_frame == 2U);

    source.write<ashiato_sync_tests::Position>(entity).x = 6.0f;
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(replay_frames.size() == 1U);
    REQUIRE(replay_frames[0].frame == 6U);
    streamer.push_frame(std::move(replay_frames[0]));
    replay_frames.clear();

    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato::sync::FrameInfo>().frame == 4U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato::sync::FrameInfo>().frame == 5U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato::sync::FrameInfo>().frame == 6U);

    source.write<ashiato_sync_tests::Position>(entity).x = 9.0f;
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    REQUIRE(source_server.tick(source, source_server.options().fixed_dt_seconds));
    writer.detach();
    REQUIRE(replay_frames.size() == 1U);
    REQUIRE(replay_frames[0].frame == 9U);
    streamer.push_frame(std::move(replay_frames[0]));
    replay_frames.clear();

    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato::sync::FrameInfo>().frame == 7U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato::sync::FrameInfo>().frame == 8U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato::sync::FrameInfo>().frame == 9U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato::sync::FrameInfo>().frame == 10U);
    REQUIRE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato::sync::FrameInfo>().frame == 11U);
    REQUIRE_FALSE(streamer.tick_session(session, playback, playback_server));
    REQUIRE(playback.get<ashiato_sync_tests::CuePlayback>(replayed).plays == 1);
    REQUIRE(playback.get<ashiato_sync_tests::Position>(replayed).x == 9.0f);
}
