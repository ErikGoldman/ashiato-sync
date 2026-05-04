#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

TEST_CASE("frame-aware receive records latency and emits dilation without jumping the buffer") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    const ecs::Entity server_entity{42};

    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().sample_count == 1);
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(client.timing_stats().jitter_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(0.90f));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{3.0f, 4.0f}}}), 4));
    REQUIRE(client.timing_stats().sample_count == 1);
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(client.timing_stats().jitter_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 4);
    REQUIRE(client.current_interpolation_buffer_frames() == 1);
}

TEST_CASE("ping samples compute conservative prediction lead and prediction dilation") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_capacity_frames = 8;
    options.input_buffer_capacity_frames = 16;
    options.auto_interpolation_smoothing = 1.0f;
    options.auto_prediction_time_dilation_min = 0.90f;
    options.auto_prediction_time_dilation_max = 1.10f;
    options.auto_prediction_time_dilation_gain = 0.10f;
    options.auto_timing_fast_recovery = false;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};
    REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 0.0f}));

    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(client.timing_stats().desired_prediction_lead_frames == 9);
    REQUIRE(client.timing_stats().target_prediction_lead_frames == 9);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().measured_prediction_lead_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().current_prediction_lead_frames == 0);
    REQUIRE(client.timing_stats().prediction_time_dilation == Catch::Approx(1.10f));

    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{3.0f, 4.0f}}}), 6));
    REQUIRE(client.timing_stats().measured_prediction_lead_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().prediction_time_dilation == Catch::Approx(1.10f));
}

TEST_CASE("server update lag fast recovery pre-fills future input frames") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions options;
    options.input_buffer_capacity_frames = 32;
    options.auto_timing_warmup_samples = 3;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};
    REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 0.0f}));

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 8));

    REQUIRE(client.timing_stats().target_prediction_lead_frames == 15);
    REQUIRE(client.timing_stats().current_prediction_lead_frames == 15);
    REQUIRE(client.timing_stats().measured_prediction_lead_frames == Catch::Approx(15.0f));
    std::vector<kage::sync::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());
    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.first_input_frame == 1);
    REQUIRE(input.input_count >= 16);
}

TEST_CASE("ping-derived prediction target jump pre-fills future input frames") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions options;
    options.input_buffer_capacity_frames = 32;
    options.auto_interpolation_smoothing = 1.0f;
    options.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};
    REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 0.0f}));

    PingPacket ping;
    REQUIRE(drain_ping(client, client_registry, 0.0, ping));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(99, {{server_entity, Position{1.0f, 2.0f}}}),
        72));
    REQUIRE(client.timing_stats().target_prediction_lead_frames == 2);

    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 14));

    REQUIRE(client.timing_stats().target_prediction_lead_frames == 15);
    REQUIRE(client.timing_stats().current_prediction_lead_frames == 15);
    std::vector<kage::sync::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());
    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.first_input_frame <= 114);
    REQUIRE(input.first_input_frame + input.input_count - 1U >= 114);
}

TEST_CASE("auto interpolation buffer moves one frame when dilation creates headroom") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

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
    const ecs::Entity server_entity{42};

    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(6, {{server_entity, Position{3.0f, 4.0f}}}), 6));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames > 1);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames >= 1.0f);
    REQUIRE(client.current_interpolation_buffer_frames() == 2);
}

TEST_CASE("auto interpolation target uses downstream update lag as a floor") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    const ecs::Entity server_entity{42};

    REQUIRE(record_ping_sample(client, client_registry, 4));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 2);
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 2);

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}),
        4));

    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 2);
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 3);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(0.90f));
    REQUIRE(client.current_interpolation_buffer_frames() == 2);
}

TEST_CASE("auto interpolation emits speedup when buffered data exceeds the desired depth") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(10, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 1);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames == Catch::Approx(6.0f));
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.10f));
}

TEST_CASE("auto interpolation buffer shrinks one frame at a time and returns to neutral dilation") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        4,
        8,
        true,
        1,
        0.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    const ecs::Entity server_entity{42};

    REQUIRE(record_ping_sample(client, client_registry, 0));
    REQUIRE(client.receive(client_registry, make_position_packet(10, {{server_entity, Position{1.0f, 2.0f}}}), 11));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 1);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames == Catch::Approx(3.0f));
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.10f));
    REQUIRE(client.current_interpolation_buffer_frames() == 3);

    REQUIRE(client.receive(client_registry, make_position_packet(11, {{server_entity, Position{2.0f, 3.0f}}}), 12));
    REQUIRE(client.current_interpolation_buffer_frames() == 2);

    REQUIRE(client.receive(client_registry, make_position_packet(12, {{server_entity, Position{3.0f, 4.0f}}}), 13));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(13, {{server_entity, Position{4.0f, 5.0f}}}), 13));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 1);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames == Catch::Approx(1.0f));
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);
}

TEST_CASE("auto interpolation buffer clamps and can be disabled") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient clamped(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        4,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    REQUIRE(record_ping_sample(clamped, client_registry, 20));
    REQUIRE(clamped.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 20));
    REQUIRE(clamped.timing_stats().target_interpolation_buffer_frames == 3);
    REQUIRE(clamped.timing_stats().time_dilation == Catch::Approx(0.90f));
    REQUIRE(clamped.current_interpolation_buffer_frames() == 1);

    ecs::Registry manual_registry;
    kage_sync_tests::define_position_archetype(manual_registry);
    kage::sync::configure_client(manual_registry, 1);
    kage::sync::ReplicationClient manual(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8,
        false,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    REQUIRE(record_ping_sample(manual, manual_registry, 8));
    REQUIRE(manual.receive(manual_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(manual.timing_stats().target_interpolation_buffer_frames == 4);
    REQUIRE(manual.timing_stats().current_interpolation_buffer_frames == 2);
    REQUIRE(manual.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(manual.current_interpolation_buffer_frames() == 2);
}

TEST_CASE("frame-aware receive failures do not update timing stats") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    const kage::sync::BitBuffer valid = make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}});
    REQUIRE(client.receive(client_registry, valid, 3));
    const kage::sync::ReplicationClientTimingStats baseline = client.timing_stats();

    kage::sync::BitBuffer truncated_header;
    truncated_header.push_bits(kage::sync::protocol::server_update_message, 8U);
    truncated_header.push_bits(2, 32U);
    REQUIRE_FALSE(client.receive(client_registry, truncated_header, 4));
    REQUIRE(client.timing_stats().sample_count == baseline.sample_count);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(baseline.time_dilation));

    REQUIRE_FALSE(client.receive(client_registry, valid, 3));
    REQUIRE(client.timing_stats().sample_count == baseline.sample_count);

    ecs::Registry invalid_interpolation_registry;
    const ecs::Entity position =
        kage::sync::register_sync_component<Position>(invalid_interpolation_registry, "Position");
    REQUIRE(kage::sync::define_archetype(
                invalid_interpolation_registry,
                "PositionActor",
                {{position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}})
        == client_archetype);
    kage::sync::configure_client(invalid_interpolation_registry, 1);
    kage::sync::ReplicationClient invalid_interpolation_client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    REQUIRE_FALSE(invalid_interpolation_client.receive(
        invalid_interpolation_registry,
        make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}),
        3));
    REQUIRE(invalid_interpolation_client.timing_stats().sample_count == 0);
    REQUIRE(invalid_interpolation_client.timing_stats().time_dilation == Catch::Approx(1.0f));
}

TEST_CASE("snap mode records timing without emitting playback dilation") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::Snap,
        2,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));

    REQUIRE(client.timing_stats().sample_count == 1);
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.current_interpolation_buffer_frames() == 2);

    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
}

TEST_CASE("zero dilation gain keeps playback speed neutral while tracking desired buffer") {
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
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.0f});
    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);
}

TEST_CASE("manual buffer override resets auto timing target and dilation") {
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
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(0.90f));

    REQUIRE(client.set_interpolation_buffer_frames(3));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 3);
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 3);
    REQUIRE(client.timing_stats().current_interpolation_buffer_frames == 3);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.current_interpolation_buffer_frames() == 3);
}

TEST_CASE("normal receive records timing from the client-owned clock without moving an idle clock buffer") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.timing_stats().sample_count == 0);
    REQUIRE(client.current_interpolation_buffer_frames() == 2);
}

TEST_CASE("adaptive ping interval samples frequently until latency stabilizes") {
    ecs::Registry client_registry;
    kage_sync_tests::define_position_archetype(client_registry);

    kage::sync::ReplicationClientOptions options;
    options.ping_interval_seconds = 3.0;
    options.adaptive_ping_interval_seconds = 0.25;
    options.adaptive_ping_stable_samples = 3;
    options.adaptive_ping_stable_threshold_frames = 0.5f;
    kage::sync::ReplicationClient client(options);

    PingPacket ping;
    REQUIRE(drain_ping(client, client_registry, 0.0, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 8));

    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 8));

    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 8));

    REQUIRE_FALSE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE_FALSE(drain_ping(client, client_registry, 2.50, ping));
    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
}

TEST_CASE("adaptive ping interval re-enters fast mode after latency jumps") {
    ecs::Registry client_registry;
    kage_sync_tests::define_position_archetype(client_registry);

    kage::sync::ReplicationClientOptions options;
    options.ping_interval_seconds = 3.0;
    options.adaptive_ping_interval_seconds = 0.25;
    options.adaptive_ping_stable_samples = 2;
    options.adaptive_ping_stable_threshold_frames = 0.5f;
    options.adaptive_ping_jump_threshold_frames = 3.0f;
    kage::sync::ReplicationClient client(options);

    PingPacket ping;
    REQUIRE(drain_ping(client, client_registry, 0.0, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 8));
    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 8));

    REQUIRE_FALSE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(drain_ping(client, client_registry, 2.75, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 20));

    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 20));
    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
}

TEST_CASE("adaptive ping interval can be disabled") {
    ecs::Registry client_registry;
    kage_sync_tests::define_position_archetype(client_registry);

    kage::sync::ReplicationClientOptions options;
    options.ping_interval_seconds = 3.0;
    options.adaptive_ping_interval = false;
    options.adaptive_ping_interval_seconds = 0.25;
    kage::sync::ReplicationClient client(options);

    PingPacket ping;
    REQUIRE(drain_ping(client, client_registry, 0.0, ping));
    REQUIRE_FALSE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(drain_ping(client, client_registry, 2.75, ping));
}
