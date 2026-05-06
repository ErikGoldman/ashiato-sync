#pragma once

#include "kage/sync/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace kage::sync::server_detail {

class BandwidthController {
public:
    explicit BandwidthController(const ReplicationBandwidthOptions& options)
        : target_bytes_per_second_(static_cast<double>(options.initial_bytes_per_second)),
          available_bytes_(static_cast<double>(burst_limit(options))) {}

    std::size_t begin_tick(const ReplicationBandwidthOptions& options, double fixed_dt_seconds) {
        const double burst = static_cast<double>(burst_limit(options));
        target_bytes_per_second_ = std::clamp(
            target_bytes_per_second_,
            static_cast<double>(options.min_bytes_per_second),
            static_cast<double>(options.max_bytes_per_second));
        available_bytes_ = std::min(burst, available_bytes_ + target_bytes_per_second_ * fixed_dt_seconds);
        adjust_target(options);
        return static_cast<std::size_t>(available_bytes_);
    }

    void spend(std::size_t bytes) noexcept {
        const double value = static_cast<double>(bytes);
        available_bytes_ = value >= available_bytes_ ? 0.0 : available_bytes_ - value;
    }

    void packet_sent(std::size_t charged_bytes) noexcept {
        in_flight_bytes_ += charged_bytes;
    }

    void packet_acked(const ReplicationBandwidthOptions& options, SyncFrame sent_frame, SyncFrame ack_frame, std::size_t charged_bytes) {
        if (charged_bytes <= in_flight_bytes_) {
            in_flight_bytes_ -= charged_bytes;
        } else {
            in_flight_bytes_ = 0;
        }
        push_sample(options, ack_frame, charged_bytes, false);
        const SyncFrame rtt_frames = ack_frame >= sent_frame ? ack_frame - sent_frame : 0;
        const double rtt = static_cast<double>(rtt_frames);
        if (!has_baseline_rtt_ || rtt < baseline_rtt_frames_) {
            baseline_rtt_frames_ = rtt;
            has_baseline_rtt_ = true;
        }
        rtt_ewma_frames_ = has_rtt_ ? rtt_ewma_frames_ * 0.875 + rtt * 0.125 : rtt;
        has_rtt_ = true;
    }

    void packet_lost(const ReplicationBandwidthOptions& options, SyncFrame frame, std::size_t charged_bytes) {
        if (charged_bytes <= in_flight_bytes_) {
            in_flight_bytes_ -= charged_bytes;
        } else {
            in_flight_bytes_ = 0;
        }
        push_sample(options, frame, 0, true);
    }

    double target_bytes_per_second() const noexcept {
        return target_bytes_per_second_;
    }

    std::size_t in_flight_bytes() const noexcept {
        return in_flight_bytes_;
    }

    double available_bytes() const noexcept {
        return available_bytes_;
    }

    float loss_rate() const noexcept {
        const std::uint32_t total = delivered_packets_ + lost_packets_;
        return total == 0U ? 0.0f : static_cast<float>(lost_packets_) / static_cast<float>(total);
    }

    std::uint64_t delivered_bytes() const noexcept {
        return delivered_bytes_;
    }

private:
    struct Sample {
        SyncFrame frame = 0;
        std::size_t bytes = 0;
        bool lost = false;
    };

    static std::size_t burst_limit(const ReplicationBandwidthOptions& options) noexcept {
        return options.max_burst_bytes == 0U
            ? std::max(options.initial_bytes_per_second / 10U, std::size_t{1})
            : options.max_burst_bytes;
    }

    void push_sample(const ReplicationBandwidthOptions& options, SyncFrame frame, std::size_t bytes, bool lost) {
        samples_.push_back(Sample{frame, bytes, lost});
        delivered_bytes_ += bytes;
        if (lost) {
            ++lost_packets_;
        } else {
            ++delivered_packets_;
        }
        const SyncFrame first_retained = frame > options.sample_window_frames
            ? frame - options.sample_window_frames
            : 0;
        while (!samples_.empty() && samples_.front().frame < first_retained) {
            const Sample& old = samples_.front();
            delivered_bytes_ -= old.bytes;
            if (old.lost) {
                --lost_packets_;
            } else {
                --delivered_packets_;
            }
            samples_.pop_front();
        }
    }

    void adjust_target(const ReplicationBandwidthOptions& options) {
        const bool rtt_inflated = has_rtt_ && has_baseline_rtt_ && baseline_rtt_frames_ > 0.0 &&
            rtt_ewma_frames_ > baseline_rtt_frames_ * static_cast<double>(options.rtt_inflation_decrease_threshold);
        if (loss_rate() > options.loss_decrease_threshold || rtt_inflated) {
            target_bytes_per_second_ = std::max(
                static_cast<double>(options.min_bytes_per_second),
                target_bytes_per_second_ * static_cast<double>(options.multiplicative_decrease));
            return;
        }
        if (delivered_packets_ > 0U && in_flight_bytes_ == 0U) {
            target_bytes_per_second_ = std::min(
                static_cast<double>(options.max_bytes_per_second),
                target_bytes_per_second_ + static_cast<double>(options.additive_increase_bytes_per_second) / 60.0);
        }
    }

    std::deque<Sample> samples_;
    double target_bytes_per_second_ = 0.0;
    double available_bytes_ = 0.0;
    double baseline_rtt_frames_ = 0.0;
    double rtt_ewma_frames_ = 0.0;
    std::size_t in_flight_bytes_ = 0;
    std::uint64_t delivered_bytes_ = 0;
    std::uint32_t delivered_packets_ = 0;
    std::uint32_t lost_packets_ = 0;
    bool has_baseline_rtt_ = false;
    bool has_rtt_ = false;
};

}  // namespace kage::sync::server_detail
