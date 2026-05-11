#pragma once

#include "ashiato/sync/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace ashiato::sync {

class ReplicationBandwidthBudget;

class ReplicationBandwidthShare {
public:
    struct Result {
        std::size_t charged_bytes = 0;
        bool had_pending_data = false;
        bool stopped_for_budget = false;
    };

    struct Participant {
        std::size_t weight = 1;
        int priority = 0;
        std::function<Result()> flush;
    };

    static Result run(
        ReplicationBandwidthBudget& budget,
        std::size_t total_bytes,
        const Participant* participants,
        std::size_t participant_count);

private:
    static void combine(Result& combined, const Result& result) noexcept;
};

class ReplicationBandwidthBudget {
public:
    struct FlushResult {
        std::size_t charged_bytes = 0;
        bool had_pending_data = false;
        bool stopped_for_budget = false;
    };

    struct FlushRequest {
        std::function<FlushResult()> flush;
    };

    explicit ReplicationBandwidthBudget(const ReplicationBandwidthOptions& options)
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

    std::size_t begin_fixed_tick(std::size_t bytes) noexcept {
        available_bytes_ = static_cast<double>(bytes);
        return bytes;
    }

    std::size_t begin_tick_once(const ReplicationBandwidthOptions& options, double fixed_dt_seconds) {
        if (!pending_requests_.empty()) {
            return static_cast<std::size_t>(available_bytes_);
        }
        return begin_tick(options, fixed_dt_seconds);
    }

    std::size_t begin_fixed_tick_once(std::size_t bytes) {
        if (!pending_requests_.empty()) {
            return static_cast<std::size_t>(available_bytes_);
        }
        return begin_fixed_tick(bytes);
    }

    ReplicationBandwidthParticipantId attach_participant(
        ReplicationBandwidthParticipantOptions options = {}) {
        if (options.weight == 0U) {
            options.weight = 1U;
        }
        const ReplicationBandwidthParticipantId id = next_participant_id_++;
        participants_.push_back(ParticipantState{id, options});
        return id;
    }

    void detach_participant(ReplicationBandwidthParticipantId participant) {
        if (participant == invalid_bandwidth_participant_id) {
            return;
        }
        participants_.erase(
            std::remove_if(
                participants_.begin(),
                participants_.end(),
                [participant](const ParticipantState& state) {
                    return state.id == participant;
                }),
            participants_.end());
        pending_requests_.erase(
            std::remove_if(
                pending_requests_.begin(),
                pending_requests_.end(),
                [participant](const PendingRequest& request) {
                    return request.participant == participant;
                }),
            pending_requests_.end());
        (void)try_run_pending();
    }

    bool set_participant_options(
        ReplicationBandwidthParticipantId participant,
        ReplicationBandwidthParticipantOptions options) {
        if (options.weight == 0U) {
            options.weight = 1U;
        }
        ParticipantState* state = participant_state(participant);
        if (state == nullptr) {
            return false;
        }
        state->options = options;
        return true;
    }

    ReplicationBandwidthParticipantOptions participant_options(
        ReplicationBandwidthParticipantId participant) const noexcept {
        const ParticipantState* state = participant_state(participant);
        return state == nullptr ? ReplicationBandwidthParticipantOptions{} : state->options;
    }

    FlushResult submit_flush(ReplicationBandwidthParticipantId participant, FlushRequest request) {
        if (participant_state(participant) == nullptr || !request.flush) {
            return {};
        }
        PendingRequest* pending = pending_request(participant);
        if (pending != nullptr) {
            pending->request = std::move(request);
        } else {
            pending_requests_.push_back(PendingRequest{participant, std::move(request)});
        }
        return try_run_pending();
    }

    void begin_send_limit(std::size_t bytes) noexcept {
        send_limit_bytes_ = bytes;
        send_limit_active_ = true;
    }

    void clear_send_limit() noexcept {
        send_limit_bytes_ = 0;
        send_limit_active_ = false;
    }

    std::size_t send_available_bytes() const noexcept {
        const std::size_t available = static_cast<std::size_t>(available_bytes_);
        return send_limit_active_ ? std::min(available, send_limit_bytes_) : available;
    }

    void spend(std::size_t bytes) noexcept {
        const double value = static_cast<double>(bytes);
        available_bytes_ = value >= available_bytes_ ? 0.0 : available_bytes_ - value;
        if (send_limit_active_) {
            send_limit_bytes_ = bytes >= send_limit_bytes_ ? 0U : send_limit_bytes_ - bytes;
        }
    }

    void packet_sent(std::size_t charged_bytes) noexcept {
        in_flight_bytes_ += charged_bytes;
    }

    void packet_acked(
        const ReplicationBandwidthOptions& options,
        SyncFrame sent_frame,
        SyncFrame ack_frame,
        std::size_t charged_bytes) {
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

    struct ParticipantState {
        ReplicationBandwidthParticipantId id = invalid_bandwidth_participant_id;
        ReplicationBandwidthParticipantOptions options;
    };

    struct PendingRequest {
        ReplicationBandwidthParticipantId participant = invalid_bandwidth_participant_id;
        FlushRequest request;
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

    ParticipantState* participant_state(ReplicationBandwidthParticipantId participant) noexcept {
        const auto found = std::find_if(
            participants_.begin(),
            participants_.end(),
            [participant](const ParticipantState& state) {
                return state.id == participant;
            });
        return found == participants_.end() ? nullptr : &*found;
    }

    const ParticipantState* participant_state(ReplicationBandwidthParticipantId participant) const noexcept {
        const auto found = std::find_if(
            participants_.begin(),
            participants_.end(),
            [participant](const ParticipantState& state) {
                return state.id == participant;
            });
        return found == participants_.end() ? nullptr : &*found;
    }

    PendingRequest* pending_request(ReplicationBandwidthParticipantId participant) noexcept {
        const auto found = std::find_if(
            pending_requests_.begin(),
            pending_requests_.end(),
            [participant](const PendingRequest& request) {
                return request.participant == participant;
            });
        return found == pending_requests_.end() ? nullptr : &*found;
    }

    FlushResult try_run_pending() {
        if (participants_.empty() || pending_requests_.size() < participants_.size()) {
            return {};
        }
        if (participants_.size() == 1U) {
            PendingRequest request = std::move(pending_requests_.front());
            pending_requests_.clear();
            if (!request.request.flush) {
                return {};
            }
            return request.request.flush();
        }

        std::vector<ReplicationBandwidthShare::Participant> share_participants;
        share_participants.reserve(participants_.size());
        for (const ParticipantState& participant : participants_) {
            PendingRequest* pending = pending_request(participant.id);
            if (pending == nullptr || !pending->request.flush) {
                return {};
            }
            auto flush = std::move(pending->request.flush);
            share_participants.push_back(ReplicationBandwidthShare::Participant{
                participant.options.weight,
                participant.options.priority,
                [flush = std::move(flush)]() mutable {
                    const FlushResult result = flush();
                    return ReplicationBandwidthShare::Result{
                        result.charged_bytes,
                        result.had_pending_data,
                        result.stopped_for_budget};
                }});
        }

        pending_requests_.clear();
        const ReplicationBandwidthShare::Result result =
            ReplicationBandwidthShare::run(
                *this,
                static_cast<std::size_t>(available_bytes_),
                share_participants.data(),
                share_participants.size());
        return FlushResult{result.charged_bytes, result.had_pending_data, result.stopped_for_budget};
    }

    std::deque<Sample> samples_;
    std::vector<ParticipantState> participants_;
    std::vector<PendingRequest> pending_requests_;
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
    std::size_t send_limit_bytes_ = 0;
    ReplicationBandwidthParticipantId next_participant_id_ = 1;
    bool send_limit_active_ = false;
};

inline ReplicationBandwidthShare::Result ReplicationBandwidthShare::run(
    ReplicationBandwidthBudget& budget,
    std::size_t total_bytes,
    const Participant* participants,
    std::size_t participant_count) {
    if (participants == nullptr || participant_count == 0U || total_bytes == 0U) {
        return {};
    }

    std::vector<std::size_t> order;
    order.reserve(participant_count);
    std::size_t total_weight = 0;
    for (std::size_t index = 0; index < participant_count; ++index) {
        if (participants[index].weight == 0U || !participants[index].flush) {
            continue;
        }
        order.push_back(index);
        total_weight += participants[index].weight;
    }
    if (total_weight == 0U) {
        return {};
    }

    std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        return participants[lhs].priority > participants[rhs].priority;
    });

    Result combined;
    std::vector<Result> results(participant_count);
    std::vector<std::size_t> allowances(participant_count, 0U);
    std::size_t allocated = 0;
    for (const std::size_t index : order) {
        allowances[index] = total_bytes * participants[index].weight / total_weight;
        allocated += allowances[index];
    }
    std::size_t remainder = total_bytes - allocated;
    for (const std::size_t index : order) {
        if (remainder == 0U) {
            break;
        }
        ++allowances[index];
        --remainder;
    }

    std::size_t leftover = 0;
    for (const std::size_t index : order) {
        const std::size_t allowance = allowances[index];
        budget.begin_send_limit(allowance);
        results[index] = participants[index].flush();
        budget.clear_send_limit();
        combine(combined, results[index]);
        leftover += allowance > results[index].charged_bytes ? allowance - results[index].charged_bytes : 0U;
    }

    for (const std::size_t index : order) {
        if (leftover == 0U) {
            break;
        }
        if (!results[index].stopped_for_budget) {
            continue;
        }
        budget.begin_send_limit(leftover);
        const Result extra = participants[index].flush();
        budget.clear_send_limit();
        combine(combined, extra);
        leftover = extra.charged_bytes >= leftover ? 0U : leftover - extra.charged_bytes;
    }

    return combined;
}

inline void ReplicationBandwidthShare::combine(Result& combined, const Result& result) noexcept {
    combined.charged_bytes += result.charged_bytes;
    combined.had_pending_data = combined.had_pending_data || result.had_pending_data;
    combined.stopped_for_budget = combined.stopped_for_budget || result.stopped_for_budget;
}

}  // namespace ashiato::sync
