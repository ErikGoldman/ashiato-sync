#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

namespace {

bool receive_pong_with_server_frame(
    kage::sync::ReplicationClient& client,
    ecs::Registry& registry,
    const PingPacket& ping,
    kage::sync::SyncFrame local_frame,
    kage::sync::SyncFrame server_frame) {
    ecs::BitBuffer pong;
    pong.push_bits(kage::sync::protocol::server_pong_message, 8U);
    pong.push_bits(ping.sequence, 32U);
    pong.push_bits(server_frame, 32U);
    pong.push_bits(0U, kage::sync::protocol::frame_subframe_bits);
    pong.push_bits(server_frame, 32U);
    pong.push_bits(0U, kage::sync::protocol::frame_subframe_bits);
    return receive_at_local_frame(client, registry, std::move(pong), local_frame);
}

void require_client_clocks_stable(const kage::sync::ReplicationClient& client) {
    REQUIRE(std::isfinite(client.estimated_server_frame()));
    REQUIRE(std::isfinite(client.continuous_buffered_frame()));
    REQUIRE(std::isfinite(client.continuous_predicted_frame()));
    REQUIRE(std::isfinite(client.continuous_buffered_frames_behind()));
    REQUIRE(std::isfinite(client.continuous_prediction_frames_ahead()));
    REQUIRE(client.current_buffered_frame_lag() < kage::sync::ReplicationClient::buffered_frame_capacity);
    REQUIRE(client.timing_stats().target_buffered_frame_lag < kage::sync::ReplicationClient::buffered_frame_capacity);
    REQUIRE(client.timing_stats().target_prediction_lead_frames < client.options().prediction.input_buffer_capacity_frames);
    REQUIRE(client.timing_stats().buffered_time_dilation >= client.options().buffered.auto_buffered_time_dilation_min);
    REQUIRE(client.timing_stats().buffered_time_dilation <= client.options().buffered.auto_buffered_time_dilation_max);
    REQUIRE(client.timing_stats().predicted_time_dilation >= client.options().prediction.auto_predicted_time_dilation_min);
    REQUIRE(client.timing_stats().predicted_time_dilation <= client.options().prediction.auto_predicted_time_dilation_max);
}

}  // namespace

TEST_CASE("server update lag fast recovery pre-fills future input frames") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions options;
    options.prediction.input_buffer_capacity_frames = 32;
    options.clock.auto_timing_warmup_samples = 3;
    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, options));
    const ecs::Entity server_entity{42};
    REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 0.0f}));

    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 8));

    REQUIRE(client.timing_stats().target_prediction_lead_frames == 15);
    REQUIRE(client.timing_stats().current_prediction_lead_frames == 15);
    REQUIRE(client.timing_stats().measured_prediction_lead_frames == Catch::Approx(15.0f));
    std::vector<ecs::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());
    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.baseline_frame == 0);
    REQUIRE(input.first_input_frame == 2);
    REQUIRE(input.first_input_full);
    REQUIRE(input.input_count >= 15);
}

TEST_CASE("client prediction and buffer clocks stay stable across server clock hitch and recovery") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage_sync_tests::configure_test_client_registry(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.entities.default_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.auto_buffered_frame_lag_smoothing = 1.0f;
    options.prediction.input_buffer_capacity_frames = 64;
    options.clock.auto_timing_warmup_samples = 1;
    options.session.ping_interval_seconds = options.clock.fixed_dt_seconds;
    options.session.adaptive_ping_interval_seconds = options.clock.fixed_dt_seconds;
    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, options));
    const ecs::Entity server_entity{42};

    PingPacket ping;
    REQUIRE(drain_ping(client, client_registry, 0.0, ping));
    REQUIRE(receive_pong_with_server_frame(client, client_registry, ping, 2, 1));
    REQUIRE(receive_at_local_frame(
        client,
        client_registry,
        make_position_packet(2, {{server_entity, Position{1.0f, 2.0f}}}),
        2));
    require_client_clocks_stable(client);
    REQUIRE(client.estimated_server_frame() == Catch::Approx(2.0));

    REQUIRE(drain_ping(client, client_registry, client.fixed_dt_seconds(), ping));
    REQUIRE(receive_pong_with_server_frame(client, client_registry, ping, 7, 4));
    REQUIRE(receive_at_local_frame(
        client,
        client_registry,
        make_position_packet(4, {{server_entity, Position{3.0f, 4.0f}}}),
        7));
    require_client_clocks_stable(client);
    REQUIRE(client.estimated_server_frame() > 4.0);
    REQUIRE(client.estimated_server_frame() < 7.0);
    REQUIRE(client.continuous_prediction_frames_ahead() < 64.0);
    REQUIRE(client.continuous_buffered_frames_behind() < 64.0);

    REQUIRE(drain_ping(client, client_registry, client.fixed_dt_seconds(), ping));
    REQUIRE(receive_pong_with_server_frame(client, client_registry, ping, 10, 9));
    REQUIRE(receive_at_local_frame(
        client,
        client_registry,
        make_position_packet(9, {{server_entity, Position{5.0f, 6.0f}}}),
        10));
    require_client_clocks_stable(client);
    REQUIRE(client.estimated_server_frame() > 9.0);
    REQUIRE(client.estimated_server_frame() < 10.5);
    REQUIRE(client.continuous_prediction_frames_ahead() < 64.0);
    REQUIRE(client.continuous_buffered_frames_behind() < 64.0);
}

TEST_CASE("auto buffered frame lag default minimum preserves two buffered frames") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage_sync_tests::configure_test_client_registry(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.entities.default_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.buffered.buffered_frame_lag = 2;
    options.buffered.auto_buffered_frame_lag_jitter_multiplier = 0.0f;
    options.buffered.auto_buffered_frame_lag_smoothing = 1.0f;
    options.clock.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, options));

    const ecs::Entity server_entity{42};
    REQUIRE(record_ping_sample(client, client_registry, 0));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(10, {{server_entity, Position{1.0f, 2.0f}}}), 11));

    REQUIRE(client.timing_stats().desired_buffered_frame_lag == 2);
    REQUIRE(client.timing_stats().target_buffered_frame_lag == 2);
    REQUIRE(client.current_buffered_frame_lag() == 2);
}

TEST_CASE("auto buffered frame lag target uses downstream update lag as a floor") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage_sync_tests::configure_test_client_registry(client_registry, 1);

    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, kage::sync::ReplicationClientOptions{
        kage::sync::ReplicationClientNetworkOptions{1200},
        kage::sync::ReplicationClientEntityOptions{kage::sync::ReplicationClientMode::BufferedInterpolation},
        kage::sync::ReplicationClientBufferedOptions{2,

        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f}}));
    const ecs::Entity server_entity{42};

    REQUIRE(record_ping_sample(client, client_registry, 4));
    REQUIRE(client.timing_stats().desired_buffered_frame_lag == 2);
    REQUIRE(client.timing_stats().target_buffered_frame_lag == 2);

    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 4));

    REQUIRE(client.timing_stats().desired_buffered_frame_lag == 2);
    REQUIRE(client.timing_stats().target_buffered_frame_lag == 3);
    REQUIRE(client.timing_stats().buffered_time_dilation == Catch::Approx(0.90f));
    REQUIRE(client.current_buffered_frame_lag() == 3);
}

TEST_CASE("auto buffered frame lag emits speedup when buffered data exceeds the desired depth") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage_sync_tests::configure_test_client_registry(client_registry, 1);

    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, kage::sync::ReplicationClientOptions{
        kage::sync::ReplicationClientNetworkOptions{1200},
        kage::sync::ReplicationClientEntityOptions{kage::sync::ReplicationClientMode::BufferedInterpolation},
        kage::sync::ReplicationClientBufferedOptions{1,

        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f}}));
    const ecs::Entity server_entity{42};

    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(10, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().desired_buffered_frame_lag == 1);
    REQUIRE(client.timing_stats().measured_buffered_frame_lag == Catch::Approx(6.0f));
    REQUIRE(client.timing_stats().buffered_time_dilation == Catch::Approx(1.10f));
}

TEST_CASE("auto buffered frame lag clamps and can be disabled") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage_sync_tests::configure_test_client_registry(client_registry, 1);
    const ecs::Entity server_entity{42};

    using SmallBufferedClient = kage::sync::ReplicationClientT<
        kage::sync::protocol::default_network_entity_id_tier0_bits,
        4,
        64>;
    SmallBufferedClient clamped(client_registry, kage::sync::ReplicationClientOptions{
        kage::sync::ReplicationClientNetworkOptions{1200},
        kage::sync::ReplicationClientEntityOptions{kage::sync::ReplicationClientMode::BufferedInterpolation},
        kage::sync::ReplicationClientBufferedOptions{1,

        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f}});
    REQUIRE(record_ping_sample(clamped, client_registry, 20));
    REQUIRE(receive_at_local_frame(clamped, client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 20));
    REQUIRE(clamped.timing_stats().target_buffered_frame_lag == 3);
    REQUIRE(clamped.timing_stats().buffered_time_dilation == Catch::Approx(0.90f));
    REQUIRE(clamped.current_buffered_frame_lag() == 2);

    ecs::Registry manual_registry;
    kage_sync_tests::define_position_archetype(manual_registry);
    kage_sync_tests::configure_test_client_registry(manual_registry, 1);
    kage::sync::ReplicationClient manual(manual_registry, kage_sync_tests::make_test_client_options(manual_registry, kage::sync::ReplicationClientOptions{
        kage::sync::ReplicationClientNetworkOptions{1200},
        kage::sync::ReplicationClientEntityOptions{kage::sync::ReplicationClientMode::BufferedInterpolation},
        kage::sync::ReplicationClientBufferedOptions{2,

        false,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f}}));
    REQUIRE(record_ping_sample(manual, manual_registry, 8));
    REQUIRE(receive_at_local_frame(manual, manual_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(manual.timing_stats().target_buffered_frame_lag == 4);
    REQUIRE(manual.timing_stats().current_buffered_frame_lag == 2);
    REQUIRE(manual.timing_stats().buffered_time_dilation == Catch::Approx(1.0f));
    REQUIRE(manual.current_buffered_frame_lag() == 2);
}

TEST_CASE("frame-aware receive failures do not update timing stats") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage_sync_tests::configure_test_client_registry(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, kage::sync::ReplicationClientOptions{
        kage::sync::ReplicationClientNetworkOptions{1200},
        kage::sync::ReplicationClientEntityOptions{kage::sync::ReplicationClientMode::BufferedInterpolation},
        kage::sync::ReplicationClientBufferedOptions{2,

        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f}}));
    const ecs::BitBuffer valid = make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}});
    REQUIRE(receive_at_local_frame(client, client_registry, valid, 3));
    const kage::sync::ReplicationClientTimingStats baseline = client.timing_stats();

    ecs::BitBuffer truncated_header;
    truncated_header.push_bits(kage::sync::protocol::server_update_message, 8U);
    truncated_header.push_bits(2, 32U);
    REQUIRE_FALSE(receive_at_local_frame(client, client_registry, truncated_header, 4));
    REQUIRE(client.timing_stats().sample_count == baseline.sample_count);
    REQUIRE(client.timing_stats().buffered_time_dilation == Catch::Approx(baseline.buffered_time_dilation));

    REQUIRE_FALSE(receive_at_local_frame(client, client_registry, valid, 3));
    REQUIRE(client.timing_stats().sample_count == baseline.sample_count);

    ecs::Registry invalid_interpolation_registry;
    const ecs::Entity position =
        kage::sync::register_sync_component<Position>(invalid_interpolation_registry, "Position");
    REQUIRE(kage::sync::define_archetype(
                invalid_interpolation_registry,
                "PositionActor",
                {{position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}})
        == client_archetype);
    kage_sync_tests::configure_test_client_registry(invalid_interpolation_registry, 1);
    kage::sync::ReplicationClient invalid_interpolation_client(invalid_interpolation_registry, kage_sync_tests::make_test_client_options(invalid_interpolation_registry, kage::sync::ReplicationClientOptions{
        kage::sync::ReplicationClientNetworkOptions{1200},
        kage::sync::ReplicationClientEntityOptions{kage::sync::ReplicationClientMode::BufferedInterpolation},
        kage::sync::ReplicationClientBufferedOptions{2}}));
    REQUIRE_FALSE(receive_at_local_frame(invalid_interpolation_client, invalid_interpolation_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 3));
    REQUIRE(invalid_interpolation_client.timing_stats().sample_count == 0);
    REQUIRE(invalid_interpolation_client.timing_stats().buffered_time_dilation == Catch::Approx(1.0f));
}

TEST_CASE("snap mode records timing without emitting buffered dilation") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage_sync_tests::configure_test_client_registry(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, kage::sync::ReplicationClientOptions{
        kage::sync::ReplicationClientNetworkOptions{1200},
        kage::sync::ReplicationClientEntityOptions{kage::sync::ReplicationClientMode::Snap},
        kage::sync::ReplicationClientBufferedOptions{2,

        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f}}));
    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));

    REQUIRE(client.timing_stats().sample_count == 1);
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(client.timing_stats().desired_buffered_frame_lag == 4);
    REQUIRE(client.timing_stats().buffered_time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.current_buffered_frame_lag() == 2);

    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
}

TEST_CASE("manual buffer override resets auto timing target and dilation") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage_sync_tests::configure_test_client_registry(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, kage::sync::ReplicationClientOptions{
        kage::sync::ReplicationClientNetworkOptions{1200},
        kage::sync::ReplicationClientEntityOptions{kage::sync::ReplicationClientMode::BufferedInterpolation},
        kage::sync::ReplicationClientBufferedOptions{1,

        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f}}));
    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(receive_at_local_frame(client, client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().desired_buffered_frame_lag == 4);
    REQUIRE(client.timing_stats().buffered_time_dilation == Catch::Approx(0.90f));

    REQUIRE(client.set_buffered_frame_lag(3));
    REQUIRE(client.timing_stats().desired_buffered_frame_lag == 3);
    REQUIRE(client.timing_stats().target_buffered_frame_lag == 3);
    REQUIRE(client.timing_stats().current_buffered_frame_lag == 3);
    REQUIRE(client.timing_stats().buffered_time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.current_buffered_frame_lag() == 3);
}

TEST_CASE("normal receive records timing from the client-owned clock without moving an idle clock buffer") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage_sync_tests::configure_test_client_registry(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, kage::sync::ReplicationClientOptions{
        kage::sync::ReplicationClientNetworkOptions{1200},
        kage::sync::ReplicationClientEntityOptions{kage::sync::ReplicationClientMode::BufferedInterpolation},
        kage::sync::ReplicationClientBufferedOptions{2}}));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.timing_stats().sample_count == 0);
    REQUIRE(client.current_buffered_frame_lag() == 2);
}

TEST_CASE("client tick caps predicted backlog when configured") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);

    kage::sync::ReplicationClientOptions options;
    options.session.local_client = 1;
    options.session.connect_token = "token";
    options.clock.max_fixed_steps_per_tick = 2;
    kage::sync::ReplicationClient client(client_registry, options);
    const ecs::Entity server_entity{42};

    ecs::BitBuffer accepted;
    accepted.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    accepted.push_bool(true);
    accepted.push_unsigned_bits(1, 64U);
    REQUIRE(client.receive(client_registry, accepted));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Accepted);

    REQUIRE(client.receive(client_registry, make_position_packet(10, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.buffered_frame() == 8);
    REQUIRE(client.predicted_frame() == 12);

    REQUIRE(client.tick(client_registry, 5.5 * client.options().clock.fixed_dt_seconds));

    REQUIRE(client.local_time_seconds() == Catch::Approx(5.5 * client.options().clock.fixed_dt_seconds));
    REQUIRE(client.buffered_frame() == 13);
    REQUIRE(client.predicted_frame() == 14);
    REQUIRE(client.timing_stats().dropped_playback_frames == 0);
    REQUIRE(client.timing_stats().dropped_input_frames == 3);
}

TEST_CASE("adaptive ping interval samples frequently until latency stabilizes") {
    ecs::Registry client_registry;
    kage_sync_tests::define_position_archetype(client_registry);

    kage::sync::ReplicationClientOptions options;
    options.session.ping_interval_seconds = 3.0;
    options.session.adaptive_ping_interval_seconds = 0.25;
    options.session.adaptive_ping_stable_samples = 3;
    options.session.adaptive_ping_stable_threshold_frames = 0.5f;
    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, options));

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
    options.session.ping_interval_seconds = 3.0;
    options.session.adaptive_ping_interval_seconds = 0.25;
    options.session.adaptive_ping_stable_samples = 2;
    options.session.adaptive_ping_stable_threshold_frames = 0.5f;
    options.session.adaptive_ping_jump_threshold_frames = 3.0f;
    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, options));

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

TEST_CASE("adaptive ping interval can match steady ping cadence") {
    ecs::Registry client_registry;
    kage_sync_tests::define_position_archetype(client_registry);

    kage::sync::ReplicationClientOptions options;
    options.session.ping_interval_seconds = 3.0;
    options.session.adaptive_ping_interval_seconds = options.session.ping_interval_seconds;
    kage::sync::ReplicationClient client(client_registry, kage_sync_tests::make_test_client_options(client_registry, options));

    PingPacket ping;
    REQUIRE(drain_ping(client, client_registry, 0.0, ping));
    REQUIRE_FALSE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(drain_ping(client, client_registry, 2.75, ping));
}
