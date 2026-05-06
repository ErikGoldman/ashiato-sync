#include "kage/sync/client_clock.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

namespace {

kage::sync::ReplicationClientClockConfig test_config() {
    kage::sync::ReplicationClientClockConfig config;
    config.fixed_dt_seconds = 1.0 / 60.0;
    config.interpolation_buffer_frames = 2;
    config.interpolation_buffer_capacity_frames = 8;
    config.prediction_lead_frames = 2;
    config.input_buffer_capacity_frames = 16;
    config.auto_interpolation_smoothing = 1.0f;
    config.auto_interpolation_time_dilation_min = 0.90f;
    config.auto_interpolation_time_dilation_max = 1.10f;
    config.auto_interpolation_time_dilation_gain = 0.10f;
    config.auto_prediction_time_dilation_min = 0.90f;
    config.auto_prediction_time_dilation_max = 1.10f;
    config.auto_prediction_time_dilation_gain = 0.10f;
    config.auto_timing_warmup_samples = 3;
    return config;
}

}  // namespace

TEST_CASE("client clock initializes stats from startup timing config") {
    const kage::sync::ReplicationClientClock clock(test_config());

    REQUIRE_FALSE(clock.bootstrapped());
    REQUIRE(clock.receive_frame() == 0);
    REQUIRE(clock.playback_frame() == 0);
    REQUIRE(clock.input_frame() == 0);
    REQUIRE(clock.interpolation_buffer_frames() == 2);
    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 2);
    REQUIRE(clock.stats().target_interpolation_buffer_frames == 2);
    REQUIRE(clock.stats().current_interpolation_buffer_frames == 2);
    REQUIRE(clock.stats().desired_prediction_lead_frames == 2);
    REQUIRE(clock.stats().target_prediction_lead_frames == 2);
    REQUIRE(clock.stats().current_prediction_lead_frames == 2);
    REQUIRE(clock.stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(clock.stats().prediction_time_dilation == Catch::Approx(1.0f));
}

TEST_CASE("client clock rejects invalid timing config") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.fixed_dt_seconds = 0.0;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClientClock(config), std::invalid_argument);

    config = test_config();
    config.interpolation_buffer_capacity_frames = 7;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClientClock(config), std::invalid_argument);

    config = test_config();
    config.interpolation_buffer_frames = 8;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClientClock(config), std::invalid_argument);

    config = test_config();
    config.auto_interpolation_smoothing = 0.0f;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClientClock(config), std::invalid_argument);

    config = test_config();
    config.prediction_lead_frames = 16;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClientClock(config), std::invalid_argument);

    config = test_config();
    config.auto_timing_fast_recovery_min_frame_gap = 0;
    REQUIRE_THROWS_AS(kage::sync::ReplicationClientClock(config), std::invalid_argument);
}

TEST_CASE("client clock advances only receive frames before bootstrap") {
    kage::sync::ReplicationClientClock clock(test_config());

    const kage::sync::ReplicationClientClock::AdvanceResult advanced =
        clock.advance(3.0 * clock.config().fixed_dt_seconds);

    REQUIRE(advanced.receive.first == 1);
    REQUIRE(advanced.receive.last == 3);
    REQUIRE(advanced.playback.empty());
    REQUIRE(advanced.input.empty());
    REQUIRE(clock.receive_frame() == 3);
    REQUIRE(clock.playback_frame() == 0);
    REQUIRE(clock.input_frame() == 0);
}

TEST_CASE("client clock bootstrap anchors playback and input without catchup burst") {
    kage::sync::ReplicationClientClock clock(test_config());
    (void)clock.advance(20.0 * clock.config().fixed_dt_seconds);

    REQUIRE(clock.bootstrap_from_server_update(10));
    REQUIRE(clock.bootstrapped());
    REQUIRE(clock.playback_frame() == 10);
    REQUIRE(clock.input_frame() == 12);
    REQUIRE(clock.stats().current_prediction_lead_frames == 2);

    const kage::sync::ReplicationClientClock::AdvanceResult none = clock.advance(0.0);
    REQUIRE(none.playback.empty());
    REQUIRE(none.input.empty());

    const kage::sync::ReplicationClientClock::AdvanceResult one =
        clock.advance(clock.config().fixed_dt_seconds);
    REQUIRE(one.playback.first == 11);
    REQUIRE(one.playback.last == 11);
    REQUIRE(one.input.first == 13);
    REQUIRE(one.input.last == 13);
}

TEST_CASE("client clock clamps bootstrap playback at zero") {
    kage::sync::ReplicationClientClock clock(test_config());

    REQUIRE(clock.bootstrap_from_server_update(1));
    REQUIRE(clock.playback_frame() == 1);
    REQUIRE(clock.input_frame() == 3);
}

TEST_CASE("client clock records pre-bootstrap pongs for startup timing") {
    kage::sync::ReplicationClientClock clock(test_config());

    clock.record_pong(0, 8);
    clock.record_pong(8, 16);
    clock.record_pong(16, 24);

    REQUIRE(clock.stats().sample_count == 3);
    REQUIRE(clock.stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(clock.stats().target_interpolation_buffer_frames == 4);
    REQUIRE(clock.stats().desired_prediction_lead_frames == 9);
}

TEST_CASE("client clock holds startup targets during warmup samples") {
    kage::sync::ReplicationClientClock clock(test_config());
    REQUIRE(clock.bootstrap_from_server_update(10));

    clock.record_pong(10, 18);
    clock.record_pong(18, 26);

    REQUIRE(clock.stats().sample_count == 2);
    REQUIRE(clock.stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 2);
    REQUIRE(clock.stats().target_interpolation_buffer_frames == 2);
    REQUIRE(clock.stats().desired_prediction_lead_frames == 2);
    REQUIRE(clock.stats().target_prediction_lead_frames == 2);
}

TEST_CASE("client clock computes auto targets after warmup") {
    kage::sync::ReplicationClientClock clock(test_config());
    REQUIRE(clock.bootstrap_from_server_update(10));

    clock.record_pong(10, 18);
    clock.record_pong(18, 26);
    clock.record_pong(26, 34);

    REQUIRE(clock.stats().sample_count == 3);
    REQUIRE(clock.stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(clock.stats().jitter_frames == Catch::Approx(0.0f));
    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(clock.stats().target_interpolation_buffer_frames == 4);
    REQUIRE(clock.stats().desired_prediction_lead_frames == 9);
    REQUIRE(clock.stats().target_prediction_lead_frames == 9);
}

TEST_CASE("client clock fast recovery uses downstream lag before warmup") {
    kage::sync::ReplicationClientClock clock(test_config());
    clock.advance_receive_frame_to(20);
    REQUIRE(clock.bootstrap_from_server_update(10));

    const kage::sync::SyncFrame prefill = clock.record_server_update(10, true, 12);
    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 2);
    REQUIRE(clock.stats().target_interpolation_buffer_frames == 7);
    REQUIRE(clock.interpolation_buffer_frames() == 7);
    REQUIRE(clock.stats().target_prediction_lead_frames == 15);
    REQUIRE(prefill == 25);
    REQUIRE(clock.stats().time_dilation == Catch::Approx(0.90f));
}

TEST_CASE("client clock fast recovery snaps large interpolation gaps to target") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.interpolation_buffer_capacity_frames = 64;
    config.input_buffer_capacity_frames = 64;
    kage::sync::ReplicationClientClock clock(config);
    clock.advance_receive_frame_to(40);
    REQUIRE(clock.bootstrap_from_server_update(10));

    const kage::sync::SyncFrame prefill = clock.record_server_update(10, true, 12);

    REQUIRE(clock.stats().target_interpolation_buffer_frames == 30);
    REQUIRE(clock.interpolation_buffer_frames() == 30);
    REQUIRE(clock.stats().target_prediction_lead_frames == 61);
    REQUIRE(prefill == 71);
}

TEST_CASE("client clock downstream lag floor can use legacy warmup behavior") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.auto_timing_fast_recovery = false;
    kage::sync::ReplicationClientClock clock(config);
    clock.advance_receive_frame_to(20);
    REQUIRE(clock.bootstrap_from_server_update(10));

    REQUIRE(clock.record_server_update(10, true, 12) == 0);
    REQUIRE(clock.stats().target_interpolation_buffer_frames == 2);
    REQUIRE(clock.stats().time_dilation == Catch::Approx(1.0f));

    clock.record_pong(10, 18);
    clock.record_pong(18, 26);
    clock.record_pong(26, 34);
    clock.record_server_update(11, true, 13);

    REQUIRE(clock.stats().target_interpolation_buffer_frames == 7);
    REQUIRE(clock.stats().time_dilation == Catch::Approx(0.90f));
    REQUIRE(clock.interpolation_buffer_frames() == 3);
}

TEST_CASE("client clock interpolation buffer shrinks one frame at a time") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.interpolation_buffer_frames = 4;
    config.auto_interpolation_min_frames = 1;
    config.auto_interpolation_jitter_multiplier = 0.0f;
    config.auto_timing_warmup_samples = 1;
    config.auto_timing_fast_recovery = false;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(4));
    clock.record_pong(10, 10);
    clock.record_server_update(4, true, 6);
    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 1);
    REQUIRE(clock.interpolation_buffer_frames() == 3);

    clock.record_server_update(3, true, 7);
    REQUIRE(clock.interpolation_buffer_frames() == 2);

    clock.record_server_update(2, true, 8);
    REQUIRE(clock.interpolation_buffer_frames() == 1);
}

TEST_CASE("client clock manual interpolation override resets target and dilation") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(10));
    clock.record_pong(10, 18);
    clock.record_server_update(11, true, 13);
    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(clock.stats().time_dilation != Catch::Approx(1.0f));

    REQUIRE(clock.set_interpolation_buffer_frames(3));
    REQUIRE(clock.interpolation_buffer_frames() == 3);
    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 3);
    REQUIRE(clock.stats().target_interpolation_buffer_frames == 3);
    REQUIRE(clock.stats().current_interpolation_buffer_frames == 3);
    REQUIRE(clock.stats().time_dilation == Catch::Approx(1.0f));
}

TEST_CASE("client clock leaves playback neutral when auto interpolation is disabled") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.auto_interpolation_buffer_frames = false;
    config.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(10));
    clock.record_pong(10, 18);
    clock.record_server_update(10, true, 12);

    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(clock.stats().target_interpolation_buffer_frames == 4);
    REQUIRE(clock.interpolation_buffer_frames() == 2);
    REQUIRE(clock.stats().time_dilation == Catch::Approx(1.0f));
}

TEST_CASE("client clock prediction dilation follows measured input lead") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(10));
    clock.record_pong(10, 18);
    clock.record_server_update(10, false, 10);

    REQUIRE(clock.stats().desired_prediction_lead_frames == 9);
    REQUIRE(clock.stats().measured_prediction_lead_frames == Catch::Approx(0.0f));
    REQUIRE(clock.stats().prediction_time_dilation == Catch::Approx(1.10f));

    clock.record_server_update(11, false, 21);
    REQUIRE(clock.stats().measured_prediction_lead_frames == Catch::Approx(10.0f));
    REQUIRE(clock.stats().prediction_time_dilation == Catch::Approx(0.90f));
}

TEST_CASE("client clock small prediction lead changes keep time dilation nudging") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(10));
    clock.record_pong(10, 18);
    const kage::sync::SyncFrame prefill = clock.record_server_update(11, 14, 9, false, 18);

    REQUIRE(prefill == 0);
    REQUIRE(clock.stats().target_prediction_lead_frames == 9);
    REQUIRE(clock.stats().measured_prediction_lead_frames == Catch::Approx(7.0f));
    REQUIRE(clock.stats().prediction_time_dilation == Catch::Approx(1.10f));
}

TEST_CASE("client clock prediction dilation uses continuous input lead") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(10));
    clock.record_pong(10, 18);
    const kage::sync::SyncFrame prefill = clock.record_server_update(
        11,
        14.0,
        9.0,
        19.5,
        false);

    REQUIRE(prefill == 0);
    REQUIRE(clock.stats().target_prediction_lead_frames == 9);
    REQUIRE(clock.stats().measured_prediction_lead_frames == Catch::Approx(8.5f));
    REQUIRE(clock.stats().prediction_time_dilation == Catch::Approx(1.05f));
}

TEST_CASE("client clock prediction dilation has a subframe deadband") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(10));
    clock.record_pong(10, 18);
    (void)clock.record_server_update(
        11,
        14.0,
        9.0,
        19.8,
        false);

    REQUIRE(clock.stats().target_prediction_lead_frames == 9);
    REQUIRE(clock.stats().measured_prediction_lead_frames == Catch::Approx(8.8f));
    REQUIRE(clock.stats().prediction_time_dilation == Catch::Approx(1.0f));
}

TEST_CASE("client clock small interpolation changes keep one-frame movement") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.interpolation_buffer_frames = 2;
    config.auto_interpolation_jitter_multiplier = 0.0f;
    config.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(10));
    clock.record_pong(10, 16);
    clock.record_server_update(11, 14, 11, true, 13);

    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 3);
    REQUIRE(clock.stats().target_interpolation_buffer_frames == 3);
    REQUIRE(clock.interpolation_buffer_frames() == 3);
    REQUIRE(clock.stats().time_dilation == Catch::Approx(0.90f));

    clock.record_pong(16, 20);
    clock.record_server_update(12, 12, 12, true, 14);

    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 2);
    REQUIRE(clock.interpolation_buffer_frames() == 2);
    REQUIRE(clock.stats().time_dilation == Catch::Approx(1.10f));
}

TEST_CASE("client clock interpolation dilation uses continuous playback delay") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(10));
    clock.record_pong(10, 16);
    clock.record_server_update(
        11,
        14.0,
        10.5,
        13.0,
        true);

    REQUIRE(clock.stats().target_interpolation_buffer_frames == 3);
    REQUIRE(clock.stats().measured_interpolation_buffer_frames == Catch::Approx(2.5f));
    REQUIRE(clock.stats().time_dilation == Catch::Approx(0.95f));
}

TEST_CASE("client clock interpolation dilation has a subframe deadband") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(10));
    clock.record_pong(10, 16);
    clock.record_server_update(
        11,
        14.0,
        9.8,
        13.0,
        true);

    REQUIRE(clock.stats().target_interpolation_buffer_frames == 3);
    REQUIRE(clock.stats().measured_interpolation_buffer_frames == Catch::Approx(3.2f));
    REQUIRE(clock.stats().time_dilation == Catch::Approx(1.0f));
}

TEST_CASE("client clock prediction lead budgets for both network legs") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.input_buffer_capacity_frames = 32;
    config.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(10));
    clock.record_pong(10, 22);

    REQUIRE(clock.stats().latency_frames == Catch::Approx(6.0f));
    REQUIRE(clock.stats().desired_interpolation_buffer_frames == 6);
    REQUIRE(clock.stats().desired_prediction_lead_frames == 13);
    REQUIRE(clock.stats().target_prediction_lead_frames == 13);
}

TEST_CASE("client clock keeps prediction neutral when auto prediction is disabled") {
    kage::sync::ReplicationClientClockConfig config = test_config();
    config.auto_prediction_lead_frames = false;
    config.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClientClock clock(config);

    REQUIRE(clock.bootstrap_from_server_update(10));
    clock.record_pong(10, 18);
    clock.record_server_update(10, false, 10);

    REQUIRE(clock.stats().target_prediction_lead_frames == 2);
    REQUIRE(clock.stats().prediction_time_dilation == Catch::Approx(1.0f));
}
