#include "test_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

void skip_bits(ashiato::BitBuffer& buffer, std::size_t bits) {
    while (bits != 0U) {
        const std::size_t chunk = std::min<std::size_t>(bits, 63U);
        (void)buffer.read_bits(chunk);
        bits -= chunk;
    }
}

std::uint16_t skip_replay_records(ashiato::BitBuffer& payload) {
    const auto record_count = static_cast<std::uint16_t>(payload.read_bits(16U));
    for (std::uint16_t index = 0; index < record_count; ++index) {
        const bool destroy = payload.read_bool();
        (void)payload.read_bits(32U);
        if (destroy) {
            continue;
        }
        (void)payload.read_bits(32U);
        (void)payload.read_bits(64U);
        (void)payload.read_bits(64U);
        const auto component_count = static_cast<std::uint16_t>(payload.read_bits(16U));
        for (std::uint16_t component = 0; component < component_count; ++component) {
            (void)payload.read_bits(16U);
            const auto component_bits = static_cast<std::uint16_t>(payload.read_bits(16U));
            skip_bits(payload, component_bits);
        }
    }
    return record_count;
}

}  // namespace

TEST_CASE("replication replay writer records full and dirty replicated state") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(registry);

    const ashiato::Entity entity = registry.create();
    registry.add<ashiato_sync_tests::Position>(entity, ashiato_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(ashiato_sync_tests::start_sync(registry, entity, archetype));

    std::vector<ashiato::sync::ReplicationReplayFrame> frames;
    ashiato::sync::ReplicationReplayWriter writer({60, [&](const ashiato::sync::ReplicationReplayFrame& frame) {
        frames.push_back(frame);
    }});

    ashiato::sync::ReplicationServer server(registry);
    writer.attach(server);

    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    REQUIRE(frames.size() == 1U);
    REQUIRE(frames[0].kind == ashiato::sync::ReplicationReplayFrameKind::Full);
    REQUIRE(frames[0].frame == 1U);

    ashiato::BitBuffer full_payload = frames[0].payload;
    REQUIRE(static_cast<std::uint16_t>(full_payload.read_bits(16U)) == 1U);
    REQUIRE_FALSE(full_payload.read_bool());
    REQUIRE(static_cast<std::uint32_t>(full_payload.read_bits(32U)) == 1U);
    REQUIRE(static_cast<std::uint32_t>(full_payload.read_bits(32U)) == archetype.value);
    REQUIRE(static_cast<std::uint64_t>(full_payload.read_bits(64U)) == 0U);
    REQUIRE(static_cast<std::uint64_t>(full_payload.read_bits(64U)) == 1U);
    REQUIRE(static_cast<std::uint16_t>(full_payload.read_bits(16U)) == 1U);
    REQUIRE(static_cast<std::uint16_t>(full_payload.read_bits(16U)) == 0U);
    REQUIRE(static_cast<std::uint16_t>(full_payload.read_bits(16U)) == sizeof(ashiato_sync_tests::Position) * 8U);

    registry.write<ashiato_sync_tests::Position>(entity).x = 3.0f;
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    REQUIRE(frames.size() == 2U);
    REQUIRE(frames[1].kind == ashiato::sync::ReplicationReplayFrameKind::Delta);
    ashiato::BitBuffer delta_payload = frames[1].payload;
    REQUIRE(static_cast<std::uint16_t>(delta_payload.read_bits(16U)) == 1U);
    REQUIRE_FALSE(delta_payload.read_bool());
    REQUIRE(static_cast<std::uint32_t>(delta_payload.read_bits(32U)) == 1U);
}

TEST_CASE("replication replay writer records destroyed replicated slots") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(registry);

    const ashiato::Entity entity = registry.create();
    registry.add<ashiato_sync_tests::Position>(entity, ashiato_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(ashiato_sync_tests::start_sync(registry, entity, archetype));

    std::vector<ashiato::sync::ReplicationReplayFrame> frames;
    ashiato::sync::ReplicationReplayWriter writer({60, [&](const ashiato::sync::ReplicationReplayFrame& frame) {
        frames.push_back(frame);
    }});

    ashiato::sync::ReplicationServer server(registry);
    writer.attach(server);
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    frames.clear();

    REQUIRE(registry.destroy(entity));
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));

    REQUIRE(frames.size() == 1U);
    REQUIRE(frames[0].kind == ashiato::sync::ReplicationReplayFrameKind::Delta);
    ashiato::BitBuffer payload = frames[0].payload;
    REQUIRE(static_cast<std::uint16_t>(payload.read_bits(16U)) == 1U);
    REQUIRE(payload.read_bool());
    REQUIRE(static_cast<std::uint32_t>(payload.read_bits(32U)) == 1U);
}

TEST_CASE("replication replay writer records replicated slots added after initialization") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(registry);

    std::vector<ashiato::sync::ReplicationReplayFrame> frames;
    ashiato::sync::ReplicationReplayWriter writer({60, [&](const ashiato::sync::ReplicationReplayFrame& frame) {
        frames.push_back(frame);
    }});

    ashiato::sync::ReplicationServer server(registry);
    writer.attach(server);
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    REQUIRE(frames.size() == 1U);
    frames.clear();

    const ashiato::Entity entity = registry.create();
    registry.add<ashiato_sync_tests::Position>(entity, ashiato_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(ashiato_sync_tests::start_sync(registry, entity, archetype));
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));

    REQUIRE(frames.size() == 1U);
    REQUIRE(frames[0].kind == ashiato::sync::ReplicationReplayFrameKind::Delta);
    ashiato::BitBuffer payload = frames[0].payload;
    REQUIRE(static_cast<std::uint16_t>(payload.read_bits(16U)) == 1U);
    REQUIRE_FALSE(payload.read_bool());
    REQUIRE(static_cast<std::uint32_t>(payload.read_bits(32U)) == 1U);
    REQUIRE(static_cast<std::uint32_t>(payload.read_bits(32U)) == archetype.value);
}

TEST_CASE("replication replay writer records queued cues in the replay payload") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = ashiato_sync_tests::define_position_archetype(registry);
    const ashiato::sync::SyncCueTypeId cue_type = ashiato::sync::register_sync_cue<ashiato_sync_tests::TestCue>(registry);

    const ashiato::Entity entity = registry.create();
    registry.add<ashiato_sync_tests::Position>(entity, ashiato_sync_tests::Position{1.0f, 2.0f});
    REQUIRE(ashiato_sync_tests::start_sync(registry, entity, archetype));

    std::vector<ashiato::sync::ReplicationReplayFrame> frames;
    ashiato::sync::ReplicationReplayWriter writer({60, [&](const ashiato::sync::ReplicationReplayFrame& frame) {
        frames.push_back(frame);
    }});

    ashiato::sync::ReplicationServer server(registry);
    writer.attach(server);

    REQUIRE(ashiato_sync_tests::emit_test_cue(
        registry,
        entity,
        ashiato::sync::SyncFrame{1},
        ashiato_sync_tests::TestCue{42},
        0.5f));
    REQUIRE(server.tick(registry, server.options().fixed_dt_seconds));
    REQUIRE(frames.size() == 1U);

    ashiato::BitBuffer payload = frames[0].payload;
    REQUIRE(skip_replay_records(payload) == 1U);
    REQUIRE(payload.read_bool());
    REQUIRE(static_cast<std::uint32_t>(payload.read_bits(32U)) == 1U);
    ashiato::sync::SyncFrame cue_frame = 0;
    REQUIRE(ashiato::sync::protocol::read_cue_frame(payload, frames[0].frame, cue_frame));
    REQUIRE(cue_frame == 1U);
    REQUIRE(static_cast<ashiato::sync::SyncCueTypeId>(payload.read_bits(16U)) == cue_type);
    const float relevance_seconds = payload.read_float32_le();
    REQUIRE(relevance_seconds == 0.5f);
    REQUIRE_FALSE(payload.read_bool());
    REQUIRE(static_cast<std::uint16_t>(payload.read_bits(16U)) == 16U);
    REQUIRE(static_cast<std::int32_t>(payload.read_bits(16U)) == 42);
    REQUIRE_FALSE(payload.read_bool());
}
