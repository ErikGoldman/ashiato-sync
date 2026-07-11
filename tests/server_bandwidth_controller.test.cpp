#include "server/bandwidth_controller.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("bandwidth controller starts with burst budget and accrues tokens") {
    ashiato::sync::ReplicationBandwidthOptions options;
    options.min_bytes_per_second = 1000;
    options.initial_bytes_per_second = 6000;
    options.max_bytes_per_second = 6000;
    options.max_burst_bytes = 600;

    ashiato::sync::server_detail::BandwidthController controller(options);

    REQUIRE(controller.begin_tick(options, 1.0 / 60.0) == 600);
    controller.spend(550);
    REQUIRE(controller.begin_tick(options, 1.0 / 60.0) == 150);
    controller.spend(1000);
    REQUIRE(controller.available_bytes() == Catch::Approx(0.0));
}

TEST_CASE("bandwidth controller charges ACKed bytes and tracks in-flight bytes") {
    ashiato::sync::ReplicationBandwidthOptions options;
    options.min_bytes_per_second = 1000;
    options.initial_bytes_per_second = 6000;
    options.max_bytes_per_second = 6000;
    options.max_burst_bytes = 600;

    ashiato::sync::server_detail::BandwidthController controller(options);
    controller.packet_sent(300);
    controller.packet_sent(200);
    REQUIRE(controller.in_flight_bytes() == 500);

    controller.packet_acked(options, 10, 14, 300);
    REQUIRE(controller.in_flight_bytes() == 200);
    REQUIRE(controller.delivered_bytes() == 300);
    REQUIRE(controller.loss_rate() == Catch::Approx(0.0f));
}

TEST_CASE("bandwidth controller decreases target on packet loss") {
    ashiato::sync::ReplicationBandwidthOptions options;
    options.min_bytes_per_second = 1000;
    options.initial_bytes_per_second = 8000;
    options.max_bytes_per_second = 16000;
    options.max_burst_bytes = 800;
    options.loss_decrease_threshold = 0.1f;
    options.multiplicative_decrease = 0.5f;

    ashiato::sync::server_detail::BandwidthController controller(options);
    controller.packet_sent(300);
    controller.packet_lost(options, 1, 300);

    (void)controller.begin_tick(options, 1.0 / 60.0);
    REQUIRE(controller.target_bytes_per_second() == Catch::Approx(4000.0));
}

TEST_CASE("bandwidth controller increases target after clean delivery") {
    ashiato::sync::ReplicationBandwidthOptions options;
    options.min_bytes_per_second = 1000;
    options.initial_bytes_per_second = 4000;
    options.max_bytes_per_second = 5000;
    options.max_burst_bytes = 500;
    options.additive_increase_bytes_per_second = 600.0f;

    ashiato::sync::server_detail::BandwidthController controller(options);
    controller.packet_sent(100);
    controller.packet_acked(options, 1, 2, 100);

    (void)controller.begin_tick(options, 1.0 / 60.0);
    REQUIRE(controller.target_bytes_per_second() == Catch::Approx(4010.0));
}

TEST_CASE("bandwidth controller additive increase follows the configured tick duration") {
    ashiato::sync::ReplicationBandwidthOptions options;
    options.min_bytes_per_second = 1000;
    options.initial_bytes_per_second = 4000;
    options.max_bytes_per_second = 5000;
    options.max_burst_bytes = 500;
    options.additive_increase_bytes_per_second = 600.0f;

    ashiato::sync::server_detail::BandwidthController thirty_hz(options);
    thirty_hz.packet_sent(100);
    thirty_hz.packet_acked(options, 1, 2, 100);
    (void)thirty_hz.begin_tick(options, 1.0 / 30.0);
    REQUIRE(thirty_hz.target_bytes_per_second() == Catch::Approx(4020.0));

    ashiato::sync::server_detail::BandwidthController one_twenty_hz(options);
    one_twenty_hz.packet_sent(100);
    one_twenty_hz.packet_acked(options, 1, 2, 100);
    (void)one_twenty_hz.begin_tick(options, 1.0 / 120.0);
    REQUIRE(one_twenty_hz.target_bytes_per_second() == Catch::Approx(4005.0));
}

TEST_CASE("bandwidth controller keeps sample loss in the configured frame window") {
    ashiato::sync::ReplicationBandwidthOptions options;
    options.min_bytes_per_second = 1000;
    options.sample_window_frames = 2;
    options.initial_bytes_per_second = 6000;
    options.max_bytes_per_second = 6000;
    options.max_burst_bytes = 600;

    ashiato::sync::server_detail::BandwidthController controller(options);
    controller.packet_lost(options, 1, 100);
    REQUIRE(controller.loss_rate() == Catch::Approx(1.0f));

    controller.packet_acked(options, 2, 4, 100);
    REQUIRE(controller.loss_rate() == Catch::Approx(0.0f));
}
