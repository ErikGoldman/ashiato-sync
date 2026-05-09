#include "kage/sync/client_clock.hpp"

#include "detail/options_validation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kage::sync {

namespace {

struct FixedStepAdvance {
    SyncFrame steps = 0;
    std::uint64_t dropped_steps = 0;
};

float dilation_from_error(float error, float gain, float min_dilation, float max_dilation) noexcept {
    constexpr float deadband_frames = 0.25f;
    if (std::fabs(error) <= deadband_frames) {
        return 1.0f;
    }
    const float unclamped = 1.0f + error * gain;
    return std::min(max_dilation, std::max(min_dilation, unclamped));
}

SyncFrame rounded_frame_count(double frames) noexcept {
    if (!std::isfinite(frames) || frames <= 0.0) {
        return 0;
    }
    return static_cast<SyncFrame>(std::round(frames));
}

FixedStepAdvance consume_fixed_steps(
    double& accumulator_seconds,
    double dt_seconds,
    double fixed_dt_seconds,
    std::uint32_t max_fixed_steps_per_tick) noexcept {
    accumulator_seconds += dt_seconds;
    if (accumulator_seconds < fixed_dt_seconds) {
        return {};
    }

    const double whole_step_count = std::floor(accumulator_seconds / fixed_dt_seconds);
    if (!std::isfinite(whole_step_count) || whole_step_count <= 0.0) {
        accumulator_seconds = 0.0;
        return {};
    }

    const double remainder = accumulator_seconds - whole_step_count * fixed_dt_seconds;
    const double clamped_remainder = remainder > 0.0 && std::isfinite(remainder) ? remainder : 0.0;
    const std::uint64_t whole_steps = whole_step_count >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(whole_step_count);
    const std::uint64_t max_steps = max_fixed_steps_per_tick == 0U
        ? whole_steps
        : std::min<std::uint64_t>(whole_steps, max_fixed_steps_per_tick);
    accumulator_seconds = clamped_remainder;
    return FixedStepAdvance{
        static_cast<SyncFrame>(std::min<std::uint64_t>(max_steps, std::numeric_limits<SyncFrame>::max())),
        whole_steps - max_steps};
}

}  // namespace

ReplicationClientClock::ReplicationClientClock(ReplicationClientClockConfig config)
    : config_(detail::validate_client_clock_config(config)),
      interpolation_buffer_frames_(config.interpolation_buffer_frames) {
    stats_.desired_interpolation_buffer_frames = config_.interpolation_buffer_frames;
    stats_.target_interpolation_buffer_frames = config_.interpolation_buffer_frames;
    stats_.current_interpolation_buffer_frames = config_.interpolation_buffer_frames;
    stats_.desired_prediction_lead_frames = config_.prediction_lead_frames;
    stats_.target_prediction_lead_frames = config_.prediction_lead_frames;
    stats_.current_prediction_lead_frames = config_.prediction_lead_frames;
}

ReplicationClientClock::AdvanceResult ReplicationClientClock::advance(double dt_seconds) noexcept {
    AdvanceResult result;
    result.receive = advance_receive(dt_seconds);
    const AdvanceResult playback_input = advance_playback_input(dt_seconds);
    result.playback = playback_input.playback;
    result.input = playback_input.input;
    return result;
}

ReplicationClientClock::FrameRange ReplicationClientClock::advance_receive(double dt_seconds) noexcept {
    FrameRange result;
    if (dt_seconds < 0.0 || !std::isfinite(dt_seconds)) {
        return result;
    }

    const SyncFrame previous_receive_frame = receive_frame_;
    const FixedStepAdvance advance = consume_fixed_steps(
        receive_accumulator_seconds_,
        dt_seconds,
        config_.fixed_dt_seconds,
        config_.max_fixed_steps_per_tick);
    receive_frame_ += advance.steps;
    stats_.dropped_receive_frames += advance.dropped_steps;
    if (receive_frame_ != previous_receive_frame) {
        result.first = previous_receive_frame + 1U;
        result.last = receive_frame_;
    }

    display_accumulator_seconds_ += dt_seconds;
    return result;
}

ReplicationClientClock::AdvanceResult ReplicationClientClock::advance_playback_input(double dt_seconds) noexcept {
    AdvanceResult result;
    if (dt_seconds < 0.0 || !std::isfinite(dt_seconds)) {
        return result;
    }

    if (!bootstrapped_) {
        return result;
    }

    const SyncFrame previous_playback_frame = playback_frame_;
    const FixedStepAdvance playback_advance = consume_fixed_steps(
        playback_accumulator_seconds_,
        dt_seconds * static_cast<double>(stats_.time_dilation),
        config_.fixed_dt_seconds,
        config_.max_fixed_steps_per_tick);
    playback_frame_ += playback_advance.steps;
    stats_.dropped_playback_frames += playback_advance.dropped_steps;
    if (playback_frame_ != previous_playback_frame) {
        result.playback.first = previous_playback_frame + 1U;
        result.playback.last = playback_frame_;
    }

    const SyncFrame previous_input_frame = input_frame_;
    const FixedStepAdvance input_advance = consume_fixed_steps(
        input_accumulator_seconds_,
        dt_seconds * static_cast<double>(stats_.prediction_time_dilation),
        config_.fixed_dt_seconds,
        config_.max_fixed_steps_per_tick);
    input_frame_ += input_advance.steps;
    stats_.dropped_input_frames += input_advance.dropped_steps;
    if (input_frame_ != previous_input_frame) {
        result.input.first = previous_input_frame + 1U;
        result.input.last = input_frame_;
    }
    return result;
}

void ReplicationClientClock::advance_receive_frame_to(SyncFrame receive_frame) noexcept {
    if (receive_frame > receive_frame_) {
        receive_frame_ = receive_frame;
        receive_accumulator_seconds_ = 0.0;
    }
}

void ReplicationClientClock::advance_input_frame_to(SyncFrame input_frame) noexcept {
    if (input_frame > input_frame_) {
        input_frame_ = input_frame;
        input_accumulator_seconds_ = 0.0;
    }
}

bool ReplicationClientClock::bootstrap_from_server_update(
    SyncFrame server_frame,
    bool seed_prediction_lead,
    bool complete_warmup) noexcept {
    if (bootstrapped_) {
        return false;
    }
    playback_frame_ = server_frame;
    input_frame_ = seed_prediction_lead ? server_frame + config_.prediction_lead_frames : server_frame;
    playback_accumulator_seconds_ = 0.0;
    input_accumulator_seconds_ = 0.0;
    display_accumulator_seconds_ = 0.0;
    display_target_frame_ = static_cast<double>(server_frame) - static_cast<double>(interpolation_buffer_frames_);
    has_display_target_frame_ = true;
    stats_.current_prediction_lead_frames = seed_prediction_lead && input_frame_ >= server_frame
        ? input_frame_ - server_frame
        : config_.prediction_lead_frames;
    if (complete_warmup || !seed_prediction_lead) {
        warmup_samples_ = config_.auto_timing_warmup_samples;
    }
    bootstrapped_ = true;
    return true;
}

void ReplicationClientClock::record_pong(SyncFrame send_frame, SyncFrame receive_frame) noexcept {
    if (receive_frame < send_frame) {
        return;
    }
    record_ping_sample(static_cast<float>(receive_frame - send_frame) * 0.5f);
}

void ReplicationClientClock::record_continuous_pong(double send_frame, double receive_frame) noexcept {
    if (!std::isfinite(send_frame) || !std::isfinite(receive_frame) || receive_frame < send_frame) {
        return;
    }
    record_ping_sample(static_cast<float>((receive_frame - send_frame) * 0.5));
}

SyncFrame ReplicationClientClock::record_server_update(
    SyncFrame server_frame,
    bool has_buffered_entities,
    SyncFrame last_recorded_input_frame) noexcept {
    return record_server_update(
        server_frame,
        static_cast<double>(receive_frame_),
        static_cast<double>(playback_frame_),
        static_cast<double>(last_recorded_input_frame),
        has_buffered_entities);
}

SyncFrame ReplicationClientClock::record_server_update(
    SyncFrame server_frame,
    SyncFrame observed_receive_frame,
    SyncFrame observed_playback_frame,
    bool has_buffered_entities,
    SyncFrame last_recorded_input_frame) noexcept {
    if (!bootstrapped_) {
        return 0;
    }
    return record_server_update(
        server_frame,
        static_cast<double>(observed_receive_frame),
        static_cast<double>(observed_playback_frame),
        static_cast<double>(last_recorded_input_frame),
        has_buffered_entities);
}

SyncFrame ReplicationClientClock::record_server_update(
    SyncFrame server_frame,
    double observed_receive_frame,
    double observed_playback_frame,
    double observed_input_frame,
    bool has_buffered_entities) noexcept {
    return record_server_update(
        server_frame,
        observed_receive_frame,
        observed_playback_frame,
        observed_input_frame,
        has_buffered_entities,
        rounded_frame_count(observed_input_frame));
}

SyncFrame ReplicationClientClock::record_server_update(
    SyncFrame server_frame,
    double observed_receive_frame,
    double observed_playback_frame,
    double observed_input_frame,
    bool has_buffered_entities,
    SyncFrame last_recorded_input_frame) noexcept {
    if (!bootstrapped_ ||
        !std::isfinite(observed_receive_frame) ||
        !std::isfinite(observed_playback_frame) ||
        !std::isfinite(observed_input_frame)) {
        return 0;
    }
    const SyncFrame observed_receive_whole = observed_receive_frame > 0.0
        ? static_cast<SyncFrame>(std::floor(observed_receive_frame))
        : 0U;
    const SyncFrame observed_downstream = observed_downstream_frames(server_frame, observed_receive_whole);
    record_interpolation_timing(server_frame, observed_receive_frame, observed_playback_frame, has_buffered_entities);
    record_prediction_timing(server_frame, observed_downstream, observed_input_frame, last_recorded_input_frame);
    if (!config_.auto_timing_fast_recovery ||
        !config_.auto_prediction_lead_frames ||
        observed_downstream == 0U) {
        return 0;
    }
    const SyncFrame target = prediction_target_from_observed(observed_downstream);
    const SyncFrame min_gap = config_.auto_timing_fast_recovery_min_frame_gap;
    const SyncFrame available_lead = last_recorded_input_frame >= server_frame
        ? last_recorded_input_frame - server_frame
        : 0U;
    if (observed_downstream < min_gap ||
        target <= available_lead) {
        return 0;
    }
    return server_frame + target;
}

void ReplicationClientClock::record_prediction_lead(SyncFrame server_frame, SyncFrame input_frame) noexcept {
    const SyncFrame measured = input_frame >= server_frame ? input_frame - server_frame : 0U;
    stats_.measured_prediction_lead_frames = static_cast<float>(measured);
    stats_.current_prediction_lead_frames = measured;
    const SyncFrame target = config_.auto_prediction_lead_frames
        ? stats_.target_prediction_lead_frames
        : config_.prediction_lead_frames;
    if (!config_.auto_prediction_lead_frames || warmup_samples_ < config_.auto_timing_warmup_samples) {
        stats_.prediction_time_dilation = measured == target ? 1.0f : stats_.prediction_time_dilation;
        return;
    }
    const float error = static_cast<float>(target) - static_cast<float>(measured);
    const float unclamped = 1.0f + error * config_.auto_prediction_time_dilation_gain;
    stats_.prediction_time_dilation = std::min(
        config_.auto_prediction_time_dilation_max,
        std::max(config_.auto_prediction_time_dilation_min, unclamped));
}

bool ReplicationClientClock::set_interpolation_buffer_frames(SyncFrame frames) noexcept {
    if (frames >= config_.interpolation_buffer_capacity_frames) {
        return false;
    }
    config_.interpolation_buffer_frames = frames;
    interpolation_buffer_frames_ = frames;
    stats_.desired_interpolation_buffer_frames = frames;
    stats_.target_interpolation_buffer_frames = frames;
    stats_.current_interpolation_buffer_frames = frames;
    stats_.time_dilation = 1.0f;
    return true;
}

void ReplicationClientClock::update_display_target(double dt_seconds) noexcept {
    if (!bootstrapped_) {
        return;
    }
    const double playback_frame =
        static_cast<double>(playback_frame_) + playback_accumulator_seconds_ / config_.fixed_dt_seconds;
    const double desired = playback_frame - static_cast<double>(interpolation_buffer_frames_);
    if (!has_display_target_frame_ || desired < 0.0) {
        display_target_frame_ = desired;
        has_display_target_frame_ = true;
        return;
    }
    if (desired <= display_target_frame_) {
        return;
    }

    const double max_advance_frames =
        dt_seconds / config_.fixed_dt_seconds * static_cast<double>(config_.auto_interpolation_time_dilation_max);
    display_target_frame_ = std::min(desired, display_target_frame_ + max_advance_frames);
}

double ReplicationClientClock::consume_display_accumulator_seconds() noexcept {
    const double value = display_accumulator_seconds_;
    display_accumulator_seconds_ = 0.0;
    return value;
}

void ReplicationClientClock::record_ping_sample(float sample) noexcept {
    if (stats_.sample_count == 0) {
        stats_.latency_frames = sample;
        stats_.jitter_frames = 0.0f;
    } else {
        const float smoothing = config_.auto_interpolation_smoothing;
        const float previous_latency = stats_.latency_frames;
        stats_.latency_frames += (sample - stats_.latency_frames) * smoothing;
        const float deviation = std::fabs(sample - previous_latency);
        stats_.jitter_frames += (deviation - stats_.jitter_frames) * smoothing;
    }
    ++stats_.sample_count;
    if (warmup_samples_ < config_.auto_timing_warmup_samples) {
        ++warmup_samples_;
    }
    if (warmup_samples_ >= config_.auto_timing_warmup_samples) {
        compute_auto_targets();
    }
}

void ReplicationClientClock::compute_auto_targets() noexcept {
    const float wanted = stats_.latency_frames +
        config_.auto_interpolation_jitter_multiplier * stats_.jitter_frames;
    const SyncFrame target = clamp_interpolation_target(
        static_cast<SyncFrame>(std::ceil(std::max(0.0f, wanted))));
    stats_.desired_interpolation_buffer_frames = target;
    stats_.target_interpolation_buffer_frames = target;

    const float prediction_wanted = 2.0f * (
        stats_.latency_frames + config_.auto_prediction_jitter_multiplier * stats_.jitter_frames) +
        static_cast<float>(config_.auto_prediction_safety_frames);
    const SyncFrame prediction_target = clamp_prediction_target(
        static_cast<SyncFrame>(std::ceil(std::max(0.0f, prediction_wanted))));
    stats_.desired_prediction_lead_frames = prediction_target;
    stats_.target_prediction_lead_frames = prediction_target;
}

SyncFrame ReplicationClientClock::observed_downstream_frames(
    SyncFrame server_frame,
    SyncFrame observed_receive_frame) const noexcept {
    return observed_receive_frame > server_frame ? observed_receive_frame - server_frame : 0U;
}

SyncFrame ReplicationClientClock::fast_recovery_step(SyncFrame current, SyncFrame target) const noexcept {
    if (current == target) {
        return current;
    }
    if (!config_.auto_timing_fast_recovery) {
        return current < target ? current + 1U : current - 1U;
    }
    const SyncFrame delta = current < target ? target - current : current - target;
    if (delta < config_.auto_timing_fast_recovery_min_frame_gap) {
        return current < target ? current + 1U : current - 1U;
    }
    return target;
}

SyncFrame ReplicationClientClock::prediction_target_from_observed(SyncFrame observed_downstream) const noexcept {
    const float ping_upstream = warmup_samples_ >= config_.auto_timing_warmup_samples
        ? stats_.latency_frames + config_.auto_prediction_jitter_multiplier * stats_.jitter_frames
        : static_cast<float>(config_.prediction_lead_frames);
    const float upstream = std::max(static_cast<float>(observed_downstream), ping_upstream);
    const float wanted = static_cast<float>(observed_downstream) + upstream +
        static_cast<float>(config_.auto_prediction_safety_frames);
    return clamp_prediction_target(static_cast<SyncFrame>(std::ceil(std::max(0.0f, wanted))));
}

void ReplicationClientClock::record_interpolation_timing(
    SyncFrame server_frame,
    double observed_receive_frame,
    double observed_playback_frame,
    bool has_buffered_entities) noexcept {
    const SyncFrame observed_receive_whole = observed_receive_frame > 0.0
        ? static_cast<SyncFrame>(std::floor(observed_receive_frame))
        : 0U;
    const SyncFrame observed_downstream = observed_downstream_frames(server_frame, observed_receive_whole);
    if (config_.auto_interpolation_buffer_frames) {
        SyncFrame target = stats_.desired_interpolation_buffer_frames;
        if (config_.auto_timing_fast_recovery && observed_downstream != 0U) {
            target = std::max(target, clamp_interpolation_target(observed_downstream));
        } else if (warmup_samples_ >= config_.auto_timing_warmup_samples && observed_downstream != 0U) {
            target = std::max(stats_.target_interpolation_buffer_frames, clamp_interpolation_target(observed_downstream));
        }
        stats_.target_interpolation_buffer_frames = target;
    }
    const SyncFrame target = stats_.target_interpolation_buffer_frames;
    const SyncFrame current = interpolation_buffer_frames_;
    const double applied_frame = std::max(0.0, observed_playback_frame - static_cast<double>(current));
    const float measured = static_cast<float>(std::max(0.0, static_cast<double>(server_frame) - applied_frame));
    stats_.measured_interpolation_buffer_frames = measured;

    if (has_buffered_entities &&
        config_.auto_interpolation_buffer_frames &&
        (warmup_samples_ >= config_.auto_timing_warmup_samples ||
         (config_.auto_timing_fast_recovery && observed_downstream != 0U))) {
        const float error = measured - static_cast<float>(target);
        stats_.time_dilation = dilation_from_error(
            error,
            config_.auto_interpolation_time_dilation_gain,
            config_.auto_interpolation_time_dilation_min,
            config_.auto_interpolation_time_dilation_max);

        const SyncFrame min_gap = config_.auto_timing_fast_recovery_min_frame_gap;
        const bool large_fast_recovery_gap = config_.auto_timing_fast_recovery &&
            ((current < target && target - current >= min_gap) ||
             (current > target && current - target >= min_gap));
        if (current < target && (large_fast_recovery_gap || measured >= static_cast<float>(current))) {
            interpolation_buffer_frames_ = fast_recovery_step(current, target);
        } else if (current > target && (large_fast_recovery_gap || measured <= static_cast<float>(current))) {
            interpolation_buffer_frames_ = fast_recovery_step(current, target);
        }
        stats_.current_interpolation_buffer_frames = interpolation_buffer_frames_;
    } else {
        stats_.current_interpolation_buffer_frames = interpolation_buffer_frames_;
        stats_.time_dilation = 1.0f;
    }
}

void ReplicationClientClock::record_prediction_timing(
    SyncFrame server_frame,
    SyncFrame observed_downstream,
    double observed_input_frame,
    SyncFrame last_recorded_input_frame) noexcept {
    const double measured_double = std::max(0.0, observed_input_frame - static_cast<double>(server_frame));
    const float measured = static_cast<float>(measured_double);
    const SyncFrame available_lead = last_recorded_input_frame >= server_frame
        ? last_recorded_input_frame - server_frame
        : 0U;
    stats_.measured_prediction_lead_frames = measured;
    stats_.current_prediction_lead_frames = std::max(rounded_frame_count(measured_double), available_lead);
    if (config_.auto_prediction_lead_frames &&
        config_.auto_timing_fast_recovery) {
        const SyncFrame observed_target = observed_downstream != 0U
            ? prediction_target_from_observed(observed_downstream)
            : stats_.desired_prediction_lead_frames;
        stats_.target_prediction_lead_frames = std::max(stats_.desired_prediction_lead_frames, observed_target);
    }
    const SyncFrame target = config_.auto_prediction_lead_frames
        ? stats_.target_prediction_lead_frames
        : config_.prediction_lead_frames;
    if (warmup_samples_ < config_.auto_timing_warmup_samples) {
        stats_.prediction_time_dilation = 1.0f;
        return;
    }
    if (config_.auto_prediction_lead_frames) {
        const float error = static_cast<float>(target) - static_cast<float>(measured);
        stats_.prediction_time_dilation = dilation_from_error(
            error,
            config_.auto_prediction_time_dilation_gain,
            config_.auto_prediction_time_dilation_min,
            config_.auto_prediction_time_dilation_max);
    } else {
        stats_.desired_prediction_lead_frames = config_.prediction_lead_frames;
        stats_.target_prediction_lead_frames = config_.prediction_lead_frames;
        stats_.prediction_time_dilation = 1.0f;
    }
}

SyncFrame ReplicationClientClock::clamp_interpolation_target(SyncFrame frames) const noexcept {
    const SyncFrame min_frames = config_.auto_interpolation_min_frames;
    const SyncFrame max_frames = static_cast<SyncFrame>(config_.interpolation_buffer_capacity_frames - 1U);
    return std::min(max_frames, std::max(min_frames, frames));
}

SyncFrame ReplicationClientClock::clamp_prediction_target(SyncFrame frames) const noexcept {
    const SyncFrame min_frames = config_.auto_prediction_min_frames;
    const SyncFrame max_frames = static_cast<SyncFrame>(config_.input_buffer_capacity_frames - 1U);
    return std::min(max_frames, std::max(min_frames, frames));
}

}  // namespace kage::sync
