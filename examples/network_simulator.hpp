#pragma once

#include "kage/sync/sync.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <random>
#include <utility>

namespace kage::sync::examples {

struct NetworkSimulatorSettings {
    double latency_ms = 0.0;
    double jitter_ms = 0.0;
    double loss_percent = 0.0;
    double bandwidth_kbps = 0.0;
    std::size_t max_queue_bytes = 64U * 1024U;
    std::size_t transport_overhead_bytes_per_packet = 28U;
};

template <typename Endpoint>
class NetworkSimulator {
public:
    struct QueuedPacket {
        Endpoint endpoint;
        ecs::BitBuffer payload;
        double deliver_at = 0.0;
        std::size_t charged_bytes = 0;
    };

    NetworkSimulator() = default;

    explicit NetworkSimulator(std::uint32_t seed)
        : rng_(seed) {}

    explicit NetworkSimulator(NetworkSimulatorSettings initial_settings, std::uint32_t seed = 0)
        : settings(initial_settings), rng_(seed) {}

    bool enqueue(const Endpoint& endpoint, const ecs::BitBuffer& payload, double now_seconds) {
        if (drops_packet()) {
            return false;
        }
        const std::size_t charged_bytes = payload.byte_size() + settings.transport_overhead_bytes_per_packet;
        if (settings.max_queue_bytes != 0U && queued_bytes_ + charged_bytes > settings.max_queue_bytes) {
            return false;
        }
        const double ready_at = now_seconds + delay_seconds();
        const double deliver_at = ready_at + bandwidth_delay_seconds(ready_at, charged_bytes);
        enqueue_ready(endpoint, payload, deliver_at, charged_bytes);
        return true;
    }

    bool enqueue(const Endpoint& endpoint, ecs::BitBuffer&& payload, double now_seconds) {
        if (drops_packet()) {
            return false;
        }
        const std::size_t charged_bytes = payload.byte_size() + settings.transport_overhead_bytes_per_packet;
        if (settings.max_queue_bytes != 0U && queued_bytes_ + charged_bytes > settings.max_queue_bytes) {
            return false;
        }
        const double ready_at = now_seconds + delay_seconds();
        const double deliver_at = ready_at + bandwidth_delay_seconds(ready_at, charged_bytes);
        enqueue_ready(endpoint, std::move(payload), deliver_at, charged_bytes);
        return true;
    }

    template <typename Fn>
    std::size_t deliver_ready(double now_seconds, Fn&& fn) {
        std::size_t delivered = 0;
        while (!queued_.empty() && queued_.front().deliver_at <= now_seconds) {
            fn(queued_.front().endpoint, queued_.front().payload);
            queued_bytes_ -= queued_.front().charged_bytes;
            queued_.pop_front();
            ++delivered;
        }
        if (queued_.empty()) {
            bandwidth_available_at_seconds_ = std::min(bandwidth_available_at_seconds_, now_seconds);
        }
        return delivered;
    }

    std::size_t drop_queued(double now_seconds) noexcept {
        const std::size_t dropped = queued_.size();
        queued_.clear();
        queued_bytes_ = 0;
        bandwidth_available_at_seconds_ = now_seconds;
        return dropped;
    }

    const std::deque<QueuedPacket>& queued_packets() const noexcept {
        return queued_;
    }

    bool empty() const noexcept {
        return queued_.empty();
    }

    std::size_t size() const noexcept {
        return queued_.size();
    }

    std::size_t queued_bytes() const noexcept {
        return queued_bytes_;
    }

    std::mt19937& random_engine() noexcept {
        return rng_;
    }

    const std::mt19937& random_engine() const noexcept {
        return rng_;
    }

    NetworkSimulatorSettings settings;

private:
    bool drops_packet() {
        if (settings.loss_percent <= 0.0) {
            return false;
        }
        std::uniform_real_distribution<double> distribution(0.0, 100.0);
        return distribution(rng_) < settings.loss_percent;
    }

    double delay_seconds() {
        double latency_ms = std::max(0.0, settings.latency_ms);
        if (settings.jitter_ms > 0.0) {
            std::uniform_real_distribution<double> distribution(-settings.jitter_ms, settings.jitter_ms);
            latency_ms = std::max(0.0, latency_ms + distribution(rng_));
        }
        return latency_ms / 1000.0;
    }

    double bandwidth_delay_seconds(double ready_at, std::size_t charged_bytes) {
        if (settings.bandwidth_kbps <= 0.0) {
            return 0.0;
        }
        const double bytes_per_second = settings.bandwidth_kbps * 1000.0 / 8.0;
        const double begin_at = std::max(ready_at, bandwidth_available_at_seconds_);
        const double duration = static_cast<double>(charged_bytes) / bytes_per_second;
        bandwidth_available_at_seconds_ = begin_at + duration;
        return bandwidth_available_at_seconds_ - ready_at;
    }

    template <typename PacketPayload>
    void enqueue_ready(const Endpoint& endpoint, PacketPayload&& payload, double deliver_at, std::size_t charged_bytes) {
        QueuedPacket queued{
            endpoint,
            std::forward<PacketPayload>(payload),
            deliver_at,
            charged_bytes};
        const auto insert_at = std::upper_bound(
            queued_.begin(),
            queued_.end(),
            queued.deliver_at,
            [](double deliver_at_value, const QueuedPacket& queued_packet) {
                return deliver_at_value < queued_packet.deliver_at;
            });
        queued_bytes_ += charged_bytes;
        queued_.insert(insert_at, std::move(queued));
    }

    std::deque<QueuedPacket> queued_;
    std::mt19937 rng_{0};
    double bandwidth_available_at_seconds_ = 0.0;
    std::size_t queued_bytes_ = 0;
};

}  // namespace kage::sync::examples
