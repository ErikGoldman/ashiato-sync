#pragma once

#include "kage/sync/types.hpp"

#include <cstddef>
#include <cstdint>

namespace kage::sync {

struct ReplicationClientTimingStats {
    std::uint64_t sample_count = 0;
    float latency_frames = 0.0f;
    float jitter_frames = 0.0f;
    float measured_interpolation_buffer_frames = 0.0f;
    SyncFrame desired_interpolation_buffer_frames = 0;
    SyncFrame target_interpolation_buffer_frames = 0;
    SyncFrame current_interpolation_buffer_frames = 0;
    float measured_prediction_lead_frames = 0.0f;
    SyncFrame desired_prediction_lead_frames = 0;
    SyncFrame target_prediction_lead_frames = 0;
    SyncFrame current_prediction_lead_frames = 0;
    float time_dilation = 1.0f;
    float prediction_time_dilation = 1.0f;
    std::uint64_t server_update_packets_received = 0;
    std::uint64_t server_update_packets_missing = 0;
    std::uint64_t server_update_packets_reordered_or_duplicate = 0;
    std::uint64_t dropped_receive_frames = 0;
    std::uint64_t dropped_playback_frames = 0;
    std::uint64_t dropped_input_frames = 0;
    float server_update_packet_loss = 0.0f;
};

struct ReplicationClientClockConfig {
    double fixed_dt_seconds = 1.0 / 60.0;
    SyncFrame interpolation_buffer_frames = 2;
    std::size_t interpolation_buffer_capacity_frames = 64;
    bool auto_interpolation_buffer_frames = true;
    SyncFrame auto_interpolation_min_frames = 2;
    float auto_interpolation_jitter_multiplier = 2.0f;
    float auto_interpolation_smoothing = 0.1f;
    float auto_interpolation_time_dilation_min = 0.95f;
    float auto_interpolation_time_dilation_max = 1.05f;
    float auto_interpolation_time_dilation_gain = 0.05f;
    bool auto_prediction_lead_frames = true;
    SyncFrame prediction_lead_frames = 2;
    std::size_t input_buffer_capacity_frames = 64;
    SyncFrame auto_prediction_min_frames = 1;
    SyncFrame auto_prediction_safety_frames = 1;
    float auto_prediction_jitter_multiplier = 2.0f;
    float auto_prediction_time_dilation_min = 0.95f;
    float auto_prediction_time_dilation_max = 1.10f;
    float auto_prediction_time_dilation_gain = 0.05f;
    std::uint32_t auto_timing_warmup_samples = 3;
    bool auto_timing_fast_recovery = true;
    SyncFrame auto_timing_fast_recovery_min_frame_gap = 4;
    std::uint32_t max_fixed_steps_per_tick = 0;
};

class ReplicationClientClock {
public:
    struct FrameRange {
        SyncFrame first = 0;
        SyncFrame last = 0;

        bool empty() const noexcept {
            return first == 0 && last == 0;
        }
    };

    struct AdvanceResult {
        FrameRange receive;
        FrameRange playback;
        FrameRange input;
    };

    explicit ReplicationClientClock(ReplicationClientClockConfig config = {});

    const ReplicationClientClockConfig& config() const noexcept {
        return config_;
    }
    const ReplicationClientTimingStats& stats() const noexcept {
        return stats_;
    }
    ReplicationClientTimingStats& mutable_stats() noexcept {
        return stats_;
    }

    bool bootstrapped() const noexcept {
        return bootstrapped_;
    }
    SyncFrame receive_frame() const noexcept {
        return receive_frame_;
    }
    SyncFrame playback_frame() const noexcept {
        return playback_frame_;
    }
    SyncFrame input_frame() const noexcept {
        return input_frame_;
    }
    SyncFrame interpolation_buffer_frames() const noexcept {
        return interpolation_buffer_frames_;
    }
    float time_dilation() const noexcept {
        return stats_.time_dilation;
    }
    float prediction_time_dilation() const noexcept {
        return stats_.prediction_time_dilation;
    }
    double display_accumulator_seconds() const noexcept {
        return display_accumulator_seconds_;
    }
    double consume_display_accumulator_seconds() noexcept;
    double input_accumulator_seconds() const noexcept {
        return input_accumulator_seconds_;
    }
    double receive_subframe() const noexcept {
        return receive_accumulator_seconds_ / config_.fixed_dt_seconds;
    }
    double playback_subframe() const noexcept {
        return playback_accumulator_seconds_ / config_.fixed_dt_seconds;
    }
    double input_subframe() const noexcept {
        return input_accumulator_seconds_ / config_.fixed_dt_seconds;
    }
    double continuous_receive_frame() const noexcept {
        return static_cast<double>(receive_frame_) + receive_subframe();
    }
    double continuous_playback_frame() const noexcept {
        return static_cast<double>(playback_frame_) + playback_subframe();
    }
    double continuous_input_frame() const noexcept {
        return static_cast<double>(input_frame_) + input_subframe();
    }
    double display_target_frame() const noexcept {
        return display_target_frame_;
    }

    AdvanceResult advance(double dt_seconds) noexcept;
    FrameRange advance_receive(double dt_seconds) noexcept;
    AdvanceResult advance_playback_input(double dt_seconds) noexcept;
    void advance_receive_frame_to(SyncFrame receive_frame) noexcept;
    void advance_input_frame_to(SyncFrame input_frame) noexcept;
    bool bootstrap_from_server_update(
        SyncFrame server_frame,
        bool seed_prediction_lead = true,
        bool complete_warmup = false) noexcept;
    void record_pong(SyncFrame send_frame, SyncFrame receive_frame) noexcept;
    void record_continuous_pong(double send_frame, double receive_frame) noexcept;
    SyncFrame record_server_update(
        SyncFrame server_frame,
        bool has_buffered_entities,
        SyncFrame last_recorded_input_frame) noexcept;
    SyncFrame record_server_update(
        SyncFrame server_frame,
        SyncFrame observed_receive_frame,
        SyncFrame observed_playback_frame,
        bool has_buffered_entities,
        SyncFrame last_recorded_input_frame) noexcept;
    SyncFrame record_server_update(
        SyncFrame server_frame,
        double observed_receive_frame,
        double observed_playback_frame,
        double observed_input_frame,
        bool has_buffered_entities) noexcept;
    SyncFrame record_server_update(
        SyncFrame server_frame,
        double observed_receive_frame,
        double observed_playback_frame,
        double observed_input_frame,
        bool has_buffered_entities,
        SyncFrame last_recorded_input_frame) noexcept;
    void record_prediction_lead(SyncFrame server_frame, SyncFrame input_frame) noexcept;
    bool set_interpolation_buffer_frames(SyncFrame frames) noexcept;
    void update_display_target(double dt_seconds) noexcept;

private:
    void record_ping_sample(float sample) noexcept;
    void compute_auto_targets() noexcept;
    SyncFrame observed_downstream_frames(SyncFrame server_frame, SyncFrame observed_receive_frame) const noexcept;
    SyncFrame fast_recovery_step(SyncFrame current, SyncFrame target) const noexcept;
    SyncFrame prediction_target_from_observed(SyncFrame observed_downstream) const noexcept;
    void record_interpolation_timing(
        SyncFrame server_frame,
        double observed_receive_frame,
        double observed_playback_frame,
        bool has_buffered_entities) noexcept;
    void record_prediction_timing(
        SyncFrame server_frame,
        SyncFrame observed_downstream,
        double observed_input_frame,
        SyncFrame last_recorded_input_frame) noexcept;
    SyncFrame clamp_interpolation_target(SyncFrame frames) const noexcept;
    SyncFrame clamp_prediction_target(SyncFrame frames) const noexcept;

    ReplicationClientClockConfig config_;
    ReplicationClientTimingStats stats_;
    double receive_accumulator_seconds_ = 0.0;
    double playback_accumulator_seconds_ = 0.0;
    double input_accumulator_seconds_ = 0.0;
    double display_accumulator_seconds_ = 0.0;
    double display_target_frame_ = 0.0;
    SyncFrame receive_frame_ = 0;
    SyncFrame playback_frame_ = 0;
    SyncFrame input_frame_ = 0;
    SyncFrame interpolation_buffer_frames_ = 0;
    std::uint32_t warmup_samples_ = 0;
    bool bootstrapped_ = false;
    bool has_display_target_frame_ = false;
};

}  // namespace kage::sync
