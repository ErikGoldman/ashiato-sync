#include "ashiato/sync/client.hpp"
#include "ashiato/sync/client_clock.hpp"
#include "ashiato/sync/server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {

template <typename Mutate>
void require_invalid_client_options(Mutate mutate) {
    ashiato::Registry registry;
    ashiato::sync::ReplicationClientOptions options;
    mutate(options);
    REQUIRE_THROWS_AS(ashiato::sync::ReplicationClient(registry, options), std::invalid_argument);
}

template <typename Mutate>
void require_invalid_server_options(Mutate mutate) {
    ashiato::Registry registry;
    ashiato::sync::ReplicationServerOptions options;
    mutate(options);
    REQUIRE_THROWS_AS(ashiato::sync::ReplicationServer(registry, options), std::invalid_argument);
}

template <typename Mutate>
void require_invalid_clock_config(Mutate mutate) {
    ashiato::sync::ReplicationClientClockConfig config;
    mutate(config);
    REQUIRE_THROWS_AS(ashiato::sync::ReplicationClientClock(config), std::invalid_argument);
}

}  // namespace

static_assert(!std::is_copy_constructible<ashiato::sync::ReplicationClient>::value,
              "ReplicationClient must remain move-only");
static_assert(!std::is_copy_assignable<ashiato::sync::ReplicationClient>::value,
              "ReplicationClient must remain move-only");
static_assert(std::is_move_constructible<ashiato::sync::ReplicationClient>::value,
              "ReplicationClient must remain movable");
static_assert(std::is_move_assignable<ashiato::sync::ReplicationClient>::value,
              "ReplicationClient must remain movable");
static_assert(!std::is_copy_constructible<ashiato::sync::ReplicationServer>::value,
              "ReplicationServer must remain non-copyable");
static_assert(!std::is_copy_assignable<ashiato::sync::ReplicationServer>::value,
              "ReplicationServer must remain non-copyable");
static_assert(!std::is_move_constructible<ashiato::sync::ReplicationServer>::value,
              "ReplicationServer must remain non-movable");
static_assert(!std::is_move_assignable<ashiato::sync::ReplicationServer>::value,
              "ReplicationServer must remain non-movable");

TEST_CASE("replication server rejects invalid option values") {
    require_invalid_server_options([](auto& options) { options.mtu_bytes = 0; });
    require_invalid_server_options([](auto& options) { options.bandwidth_limit_bytes_per_tick = 0; });
    require_invalid_server_options([](auto& options) { options.fixed_entity_replication_cost_bytes = 0; });
    require_invalid_server_options([](auto& options) { options.bandwidth.min_bytes_per_second = 0; });
    require_invalid_server_options([](auto& options) { options.bandwidth.initial_bytes_per_second = 0; });
    require_invalid_server_options([](auto& options) { options.bandwidth.max_bytes_per_second = 0; });
    require_invalid_server_options([](auto& options) { options.bandwidth.initial_bytes_per_second = options.bandwidth.min_bytes_per_second - 1U; });
    require_invalid_server_options([](auto& options) { options.bandwidth.max_bytes_per_second = options.bandwidth.initial_bytes_per_second - 1U; });
    require_invalid_server_options([](auto& options) { options.bandwidth.sample_window_frames = 0; });
    require_invalid_server_options([](auto& options) { options.bandwidth.loss_decrease_threshold = std::numeric_limits<float>::quiet_NaN(); });
    require_invalid_server_options([](auto& options) { options.bandwidth.rtt_inflation_decrease_threshold = 0.5f; });
    require_invalid_server_options([](auto& options) { options.bandwidth.multiplicative_decrease = 0.0f; });
    require_invalid_server_options([](auto& options) { options.bandwidth.additive_increase_bytes_per_second = std::numeric_limits<float>::infinity(); });
    require_invalid_server_options([](auto& options) { options.serialized_worker_threads = 0; });
    require_invalid_server_options([](auto& options) { options.protocol.max_pending_packet_acks_per_client = 0; });
    require_invalid_server_options([](auto& options) {
        options.protocol.max_pending_packet_acks_per_client =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
    });
    require_invalid_server_options([](auto& options) {
        options.protocol.baseline_frame_delta_bits = ashiato::sync::protocol::baseline_frame_delta_bits + 1U;
    });
    require_invalid_server_options([](auto& options) {
        options.fixed_dt_seconds = std::numeric_limits<double>::infinity();
    });
    require_invalid_server_options([](auto& options) {
        options.idle_client_timeout_seconds = std::numeric_limits<double>::quiet_NaN();
    });

    ashiato::sync::ReplicationServerOptions valid;
    valid.idle_client_timeout_seconds = 0.0;
    valid.prioritizer_interval_frames = 0;
    ashiato::Registry registry;
    REQUIRE_NOTHROW(ashiato::sync::ReplicationServer(registry, valid));
}

TEST_CASE("replication client rejects invalid option values") {
    require_invalid_client_options([](auto& options) { options.network.mtu_bytes = 0; });
    require_invalid_client_options([](auto& options) { options.network.protocol.max_pending_packet_acks_per_client = 0; });
    require_invalid_client_options([](auto& options) {
        options.network.protocol.max_pending_packet_acks_per_client =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
    });
    require_invalid_client_options([](auto& options) {
        options.network.protocol.baseline_frame_delta_bits = ashiato::sync::protocol::baseline_frame_delta_bits + 1U;
    });
    require_invalid_client_options([](auto& options) {
        options.buffered.auto_buffered_frame_lag_jitter_multiplier = std::numeric_limits<float>::quiet_NaN();
    });
    require_invalid_client_options([](auto& options) {
        options.prediction.auto_predicted_time_dilation_max = std::numeric_limits<float>::infinity();
    });
    require_invalid_client_options([](auto& options) {
        options.session.adaptive_ping_jump_threshold_frames = std::numeric_limits<float>::quiet_NaN();
    });
    require_invalid_client_options([](auto& options) { options.prediction.input_buffer_capacity_frames = 3; });
    require_invalid_client_options([](auto& options) {
        options.prediction.input_buffer_capacity_frames = 8;
        options.prediction.lead_frames = 8;
    });
}

TEST_CASE("client clock rejects non-finite timing config values") {
    require_invalid_clock_config([](auto& config) {
        config.auto_buffered_frame_lag_jitter_multiplier = std::numeric_limits<float>::quiet_NaN();
    });
    require_invalid_clock_config([](auto& config) {
        config.auto_buffered_frame_lag_smoothing = std::numeric_limits<float>::infinity();
    });
    require_invalid_clock_config([](auto& config) {
        config.auto_predicted_time_dilation_gain = std::numeric_limits<float>::quiet_NaN();
    });
}
