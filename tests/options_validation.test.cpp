#include "kage/sync/client.hpp"
#include "kage/sync/client_clock.hpp"
#include "kage/sync/server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {

template <typename Mutate>
void require_invalid_client_options(Mutate mutate) {
    kage::sync::ReplicationClientOptions options;
    mutate(options);
    REQUIRE_THROWS_AS(kage::sync::ReplicationClient(options), std::invalid_argument);
}

template <typename Mutate>
void require_invalid_server_options(Mutate mutate) {
    kage::sync::ReplicationServerOptions options;
    mutate(options);
    REQUIRE_THROWS_AS(kage::sync::ReplicationServer(options), std::invalid_argument);
}

template <typename Mutate>
void require_invalid_clock_config(Mutate mutate) {
    kage::sync::ReplicationClientClockConfig config;
    mutate(config);
    REQUIRE_THROWS_AS(kage::sync::ReplicationClientClock(config), std::invalid_argument);
}

}  // namespace

static_assert(!std::is_copy_constructible<kage::sync::ReplicationClient>::value,
              "ReplicationClient must remain move-only");
static_assert(!std::is_copy_assignable<kage::sync::ReplicationClient>::value,
              "ReplicationClient must remain move-only");
static_assert(std::is_move_constructible<kage::sync::ReplicationClient>::value,
              "ReplicationClient must remain movable");
static_assert(std::is_move_assignable<kage::sync::ReplicationClient>::value,
              "ReplicationClient must remain movable");
static_assert(!std::is_copy_constructible<kage::sync::ReplicationServer>::value,
              "ReplicationServer must remain move-only");
static_assert(!std::is_copy_assignable<kage::sync::ReplicationServer>::value,
              "ReplicationServer must remain move-only");
static_assert(std::is_move_constructible<kage::sync::ReplicationServer>::value,
              "ReplicationServer must remain movable");
static_assert(std::is_move_assignable<kage::sync::ReplicationServer>::value,
              "ReplicationServer must remain movable");

TEST_CASE("replication server rejects invalid option values") {
    require_invalid_server_options([](auto& options) { options.mtu_bytes = 0; });
    require_invalid_server_options([](auto& options) { options.bandwidth_limit_bytes_per_tick = 0; });
    require_invalid_server_options([](auto& options) { options.fixed_entity_replication_cost_bytes = 0; });
    require_invalid_server_options([](auto& options) { options.serialized_worker_threads = 0; });
    require_invalid_server_options([](auto& options) { options.protocol.max_pending_packet_acks_per_client = 0; });
    require_invalid_server_options([](auto& options) {
        options.protocol.max_pending_packet_acks_per_client =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
    });
    require_invalid_server_options([](auto& options) {
        options.protocol.baseline_frame_delta_bits = kage::sync::protocol::baseline_frame_delta_bits + 1U;
    });
    require_invalid_server_options([](auto& options) {
        options.fixed_dt_seconds = std::numeric_limits<double>::infinity();
    });
    require_invalid_server_options([](auto& options) {
        options.idle_client_timeout_seconds = std::numeric_limits<double>::quiet_NaN();
    });

    kage::sync::ReplicationServerOptions valid;
    valid.idle_client_timeout_seconds = 0.0;
    valid.prioritizer_interval_frames = 0;
    REQUIRE_NOTHROW(kage::sync::ReplicationServer(valid));
}

TEST_CASE("replication client rejects invalid option values") {
    require_invalid_client_options([](auto& options) { options.mtu_bytes = 0; });
    require_invalid_client_options([](auto& options) { options.protocol.max_pending_packet_acks_per_client = 0; });
    require_invalid_client_options([](auto& options) {
        options.protocol.max_pending_packet_acks_per_client =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
    });
    require_invalid_client_options([](auto& options) {
        options.protocol.baseline_frame_delta_bits = kage::sync::protocol::baseline_frame_delta_bits + 1U;
    });
    require_invalid_client_options([](auto& options) {
        options.auto_interpolation_jitter_multiplier = std::numeric_limits<float>::quiet_NaN();
    });
    require_invalid_client_options([](auto& options) {
        options.auto_prediction_time_dilation_max = std::numeric_limits<float>::infinity();
    });
    require_invalid_client_options([](auto& options) {
        options.adaptive_ping_jump_threshold_frames = std::numeric_limits<float>::quiet_NaN();
    });
    require_invalid_client_options([](auto& options) { options.prediction_buffer_capacity_frames = 3; });
    require_invalid_client_options([](auto& options) { options.interpolation_buffer_capacity_frames = 0; });
    require_invalid_client_options([](auto& options) { options.input_buffer_capacity_frames = 3; });
    require_invalid_client_options([](auto& options) {
        options.interpolation_buffer_capacity_frames = 8;
        options.interpolation_buffer_frames = 8;
    });
    require_invalid_client_options([](auto& options) {
        options.input_buffer_capacity_frames = 8;
        options.prediction_lead_frames = 8;
    });
}

TEST_CASE("client clock rejects non-finite timing config values") {
    require_invalid_clock_config([](auto& config) {
        config.auto_interpolation_jitter_multiplier = std::numeric_limits<float>::quiet_NaN();
    });
    require_invalid_clock_config([](auto& config) {
        config.auto_interpolation_smoothing = std::numeric_limits<float>::infinity();
    });
    require_invalid_clock_config([](auto& config) {
        config.auto_prediction_time_dilation_gain = std::numeric_limits<float>::quiet_NaN();
    });
}
