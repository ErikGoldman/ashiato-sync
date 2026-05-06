#include "server/bandwidth_controller.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("bandwidth controller starts with burst budget and accrues tokens") {
    kage::sync::ReplicationBandwidthOptions options;
    options.min_bytes_per_second = 1000;
    options.initial_bytes_per_second = 6000;
    options.max_bytes_per_second = 6000;
    options.max_burst_bytes = 600;

    kage::sync::server_detail::BandwidthController controller(options);

    REQUIRE(controller.begin_tick(options, 1.0 / 60.0) == 600);
    controller.spend(550);
    REQUIRE(controller.begin_tick(options, 1.0 / 60.0) == 150);
    controller.spend(1000);
    REQUIRE(controller.available_bytes() == Catch::Approx(0.0));
}

TEST_CASE("bandwidth controller charges ACKed bytes and tracks in-flight bytes") {
    kage::sync::ReplicationBandwidthOptions options;
    options.min_bytes_per_second = 1000;
    options.initial_bytes_per_second = 6000;
    options.max_bytes_per_second = 6000;
    options.max_burst_bytes = 600;

    kage::sync::server_detail::BandwidthController controller(options);
    controller.packet_sent(300);
    controller.packet_sent(200);
    REQUIRE(controller.in_flight_bytes() == 500);

    controller.packet_acked(options, 10, 14, 300);
    REQUIRE(controller.in_flight_bytes() == 200);
    REQUIRE(controller.delivered_bytes() == 300);
    REQUIRE(controller.loss_rate() == Catch::Approx(0.0f));
}

TEST_CASE("bandwidth controller decreases target on packet loss") {
    kage::sync::ReplicationBandwidthOptions options;
    options.min_bytes_per_second = 1000;
    options.initial_bytes_per_second = 8000;
    options.max_bytes_per_second = 16000;
    options.max_burst_bytes = 800;
    options.loss_decrease_threshold = 0.1f;
    options.multiplicative_decrease = 0.5f;

    kage::sync::server_detail::BandwidthController controller(options);
    controller.packet_sent(300);
    controller.packet_lost(options, 1, 300);

    (void)controller.begin_tick(options, 1.0 / 60.0);
    REQUIRE(controller.target_bytes_per_second() == Catch::Approx(4000.0));
}

TEST_CASE("bandwidth controller increases target after clean delivery") {
    kage::sync::ReplicationBandwidthOptions options;
    options.min_bytes_per_second = 1000;
    options.initial_bytes_per_second = 4000;
    options.max_bytes_per_second = 5000;
    options.max_burst_bytes = 500;
    options.additive_increase_bytes_per_second = 600.0f;

    kage::sync::server_detail::BandwidthController controller(options);
    controller.packet_sent(100);
    controller.packet_acked(options, 1, 2, 100);

    (void)controller.begin_tick(options, 1.0 / 60.0);
    REQUIRE(controller.target_bytes_per_second() == Catch::Approx(4010.0));
}

TEST_CASE("bandwidth controller keeps sample loss in the configured frame window") {
    kage::sync::ReplicationBandwidthOptions options;
    options.min_bytes_per_second = 1000;
    options.sample_window_frames = 2;
    options.initial_bytes_per_second = 6000;
    options.max_bytes_per_second = 6000;
    options.max_burst_bytes = 600;

    kage::sync::server_detail::BandwidthController controller(options);
    controller.packet_lost(options, 1, 100);
    REQUIRE(controller.loss_rate() == Catch::Approx(1.0f));

    controller.packet_acked(options, 2, 4, 100);
    REQUIRE(controller.loss_rate() == Catch::Approx(0.0f));
}
