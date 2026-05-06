#include "detail/options_validation.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace kage::sync::detail {
namespace {

bool is_power_of_two(std::size_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
}

void require_positive_bytes(std::size_t value, const char* message) {
    if (value == 0U) {
        throw std::invalid_argument(message);
    }
}

void require_finite_positive(double value, const char* message) {
    if (value <= 0.0 || !std::isfinite(value)) {
        throw std::invalid_argument(message);
    }
}

void require_finite_non_negative(double value, const char* message) {
    if (value < 0.0 || !std::isfinite(value)) {
        throw std::invalid_argument(message);
    }
}

void require_finite_non_negative(float value, const char* message) {
    if (value < 0.0f || !std::isfinite(value)) {
        throw std::invalid_argument(message);
    }
}

void require_finite_unit_min(float value, const char* message) {
    if (value <= 0.0f || value > 1.0f || !std::isfinite(value)) {
        throw std::invalid_argument(message);
    }
}

void require_finite_at_least_one(float value, const char* message) {
    if (value < 1.0f || !std::isfinite(value)) {
        throw std::invalid_argument(message);
    }
}

void require_finite_positive(float value, const char* message) {
    if (value <= 0.0f || !std::isfinite(value)) {
        throw std::invalid_argument(message);
    }
}

void require_valid_max_pending_packet_acks(std::size_t value) {
    if (value == 0U || value > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument("max pending packet ACKs per client must be in [1, 2^32 - 1]");
    }
}

void validate_protocol_descriptor(const protocol::Descriptor& descriptor) {
    require_valid_max_pending_packet_acks(descriptor.max_pending_packet_acks_per_client);
    if (!protocol::valid_network_entity_id_tier0_bits(descriptor.network_entity_id_tier0_bits)) {
        throw std::invalid_argument("network entity id tier0 bits must be in [1, 22]");
    }
    if (descriptor.baseline_frame_delta_bits != protocol::baseline_frame_delta_bits) {
        throw std::invalid_argument("protocol descriptor baseline frame delta bits must match this build");
    }
}

void validate_client_timing_options(
    double fixed_dt_seconds,
    std::size_t interpolation_buffer_capacity_frames,
    SyncFrame interpolation_buffer_frames,
    SyncFrame auto_interpolation_min_frames,
    float auto_interpolation_jitter_multiplier,
    float auto_interpolation_smoothing,
    float auto_interpolation_time_dilation_min,
    float auto_interpolation_time_dilation_max,
    float auto_interpolation_time_dilation_gain,
    std::size_t input_buffer_capacity_frames,
    SyncFrame prediction_lead_frames,
    SyncFrame auto_prediction_min_frames,
    float auto_prediction_jitter_multiplier,
    float auto_prediction_time_dilation_min,
    float auto_prediction_time_dilation_max,
    float auto_prediction_time_dilation_gain,
    SyncFrame auto_timing_fast_recovery_min_frame_gap) {
    require_finite_positive(fixed_dt_seconds, "fixed dt seconds must be finite and positive");
    if (!is_power_of_two(interpolation_buffer_capacity_frames)) {
        throw std::invalid_argument("interpolation buffer capacity must be a nonzero power of two");
    }
    if (!is_power_of_two(input_buffer_capacity_frames)) {
        throw std::invalid_argument("input buffer capacity must be a nonzero power of two");
    }
    if (interpolation_buffer_frames >= interpolation_buffer_capacity_frames) {
        throw std::invalid_argument("interpolation buffer amount must be smaller than capacity");
    }
    if (auto_interpolation_min_frames >= interpolation_buffer_capacity_frames) {
        throw std::invalid_argument("auto interpolation buffer minimum must be smaller than capacity");
    }
    require_finite_non_negative(
        auto_interpolation_jitter_multiplier,
        "auto interpolation jitter multiplier must be finite and non-negative");
    if (auto_interpolation_smoothing <= 0.0f ||
        auto_interpolation_smoothing > 1.0f ||
        !std::isfinite(auto_interpolation_smoothing)) {
        throw std::invalid_argument("auto interpolation smoothing must be finite and in the range (0, 1]");
    }
    require_finite_unit_min(
        auto_interpolation_time_dilation_min,
        "auto interpolation time dilation minimum must be finite and in the range (0, 1]");
    require_finite_at_least_one(
        auto_interpolation_time_dilation_max,
        "auto interpolation time dilation maximum must be finite and at least 1");
    require_finite_non_negative(
        auto_interpolation_time_dilation_gain,
        "auto interpolation time dilation gain must be finite and non-negative");
    if (prediction_lead_frames >= input_buffer_capacity_frames) {
        throw std::invalid_argument("prediction lead frames must be smaller than input buffer capacity");
    }
    if (auto_prediction_min_frames >= input_buffer_capacity_frames) {
        throw std::invalid_argument("auto prediction lead minimum must be smaller than input buffer capacity");
    }
    require_finite_non_negative(
        auto_prediction_jitter_multiplier,
        "auto prediction jitter multiplier must be finite and non-negative");
    require_finite_unit_min(
        auto_prediction_time_dilation_min,
        "auto prediction time dilation minimum must be finite and in the range (0, 1]");
    require_finite_at_least_one(
        auto_prediction_time_dilation_max,
        "auto prediction time dilation maximum must be finite and at least 1");
    require_finite_non_negative(
        auto_prediction_time_dilation_gain,
        "auto prediction time dilation gain must be finite and non-negative");
    if (auto_timing_fast_recovery_min_frame_gap == 0U) {
        throw std::invalid_argument("auto timing fast recovery min frame gap must be greater than zero");
    }
}

}  // namespace

ReplicationClientOptions validate_client_options(ReplicationClientOptions options) {
    require_positive_bytes(options.mtu_bytes, "MTU bytes must be greater than zero");
    validate_protocol_descriptor(options.protocol);
    if (!is_power_of_two(options.prediction_buffer_capacity_frames)) {
        throw std::invalid_argument("prediction buffer capacity must be a nonzero power of two");
    }
    validate_client_timing_options(
        options.fixed_dt_seconds,
        options.interpolation_buffer_capacity_frames,
        options.interpolation_buffer_frames,
        options.auto_interpolation_min_frames,
        options.auto_interpolation_jitter_multiplier,
        options.auto_interpolation_smoothing,
        options.auto_interpolation_time_dilation_min,
        options.auto_interpolation_time_dilation_max,
        options.auto_interpolation_time_dilation_gain,
        options.input_buffer_capacity_frames,
        options.prediction_lead_frames,
        options.auto_prediction_min_frames,
        options.auto_prediction_jitter_multiplier,
        options.auto_prediction_time_dilation_min,
        options.auto_prediction_time_dilation_max,
        options.auto_prediction_time_dilation_gain,
        options.auto_timing_fast_recovery_min_frame_gap);
    require_finite_positive(
        options.connect_resend_interval_seconds,
        "connect resend interval seconds must be finite and positive");
    require_finite_positive(options.ping_interval_seconds, "ping interval seconds must be finite and positive");
    require_finite_positive(
        options.adaptive_ping_interval_seconds,
        "adaptive ping interval seconds must be finite and positive");
    if (options.adaptive_ping_stable_samples == 0U) {
        throw std::invalid_argument("adaptive ping stable samples must be greater than zero");
    }
    require_finite_non_negative(
        options.adaptive_ping_stable_threshold_frames,
        "adaptive ping stable threshold frames must be finite and non-negative");
    require_finite_non_negative(
        options.adaptive_ping_jump_threshold_frames,
        "adaptive ping jump threshold frames must be finite and non-negative");
    return options;
}

ReplicationClientClockConfig validate_client_clock_config(ReplicationClientClockConfig config) {
    validate_client_timing_options(
        config.fixed_dt_seconds,
        config.interpolation_buffer_capacity_frames,
        config.interpolation_buffer_frames,
        config.auto_interpolation_min_frames,
        config.auto_interpolation_jitter_multiplier,
        config.auto_interpolation_smoothing,
        config.auto_interpolation_time_dilation_min,
        config.auto_interpolation_time_dilation_max,
        config.auto_interpolation_time_dilation_gain,
        config.input_buffer_capacity_frames,
        config.prediction_lead_frames,
        config.auto_prediction_min_frames,
        config.auto_prediction_jitter_multiplier,
        config.auto_prediction_time_dilation_min,
        config.auto_prediction_time_dilation_max,
        config.auto_prediction_time_dilation_gain,
        config.auto_timing_fast_recovery_min_frame_gap);
    return config;
}

ReplicationServerOptions validate_server_options(ReplicationServerOptions options) {
    require_positive_bytes(options.bandwidth_limit_bytes_per_tick, "bandwidth limit bytes per tick must be greater than zero");
    require_positive_bytes(options.fixed_entity_replication_cost_bytes, "fixed entity replication cost bytes must be greater than zero");
    require_positive_bytes(options.mtu_bytes, "MTU bytes must be greater than zero");
    require_positive_bytes(options.bandwidth.min_bytes_per_second, "minimum bandwidth bytes per second must be greater than zero");
    require_positive_bytes(options.bandwidth.initial_bytes_per_second, "initial bandwidth bytes per second must be greater than zero");
    require_positive_bytes(options.bandwidth.max_bytes_per_second, "maximum bandwidth bytes per second must be greater than zero");
    if (options.bandwidth.min_bytes_per_second > options.bandwidth.initial_bytes_per_second ||
        options.bandwidth.initial_bytes_per_second > options.bandwidth.max_bytes_per_second) {
        throw std::invalid_argument("bandwidth byte rates must satisfy min <= initial <= max");
    }
    if (options.bandwidth.sample_window_frames == 0U) {
        throw std::invalid_argument("bandwidth sample window frames must be greater than zero");
    }
    require_finite_non_negative(
        options.bandwidth.loss_decrease_threshold,
        "bandwidth loss decrease threshold must be finite and non-negative");
    require_finite_at_least_one(
        options.bandwidth.rtt_inflation_decrease_threshold,
        "bandwidth RTT inflation decrease threshold must be finite and at least 1");
    require_finite_unit_min(
        options.bandwidth.multiplicative_decrease,
        "bandwidth multiplicative decrease must be finite and in the range (0, 1]");
    require_finite_positive(
        options.bandwidth.additive_increase_bytes_per_second,
        "bandwidth additive increase bytes per second must be finite and positive");
    if (options.serialized_worker_threads == 0U) {
        throw std::invalid_argument("serialized worker threads must be greater than zero");
    }
    validate_protocol_descriptor(options.protocol);
    require_finite_positive(options.fixed_dt_seconds, "fixed dt seconds must be finite and positive");
    require_finite_positive(
        options.connect_resend_interval_seconds,
        "connect resend interval seconds must be finite and positive");
    require_finite_non_negative(
        options.idle_client_timeout_seconds,
        "idle client timeout seconds must be finite and non-negative");
    if (!is_power_of_two(options.input_buffer_capacity_frames)) {
        throw std::invalid_argument("input buffer capacity must be a nonzero power of two");
    }
    return options;
}

}  // namespace kage::sync::detail
