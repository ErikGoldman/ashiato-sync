#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <random>
#include <utility>

namespace ashiato::sync {

struct SimulatedLinkSettings {
    double latency_ms = 0.0;
    double jitter_ms = 0.0;
    double loss_percent = 0.0;
};

template <typename Payload, typename Endpoint>
class SimulatedLink {
public:
    struct QueuedPacket {
        Endpoint endpoint;
        Payload payload;
        double deliver_at = 0.0;
    };

    SimulatedLink() = default;

    explicit SimulatedLink(std::uint32_t seed)
        : rng_(seed) {}

    explicit SimulatedLink(SimulatedLinkSettings initial_settings, std::uint32_t seed = 0)
        : settings(initial_settings), rng_(seed) {}

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

    bool enqueue(const Endpoint& endpoint, const Payload& payload, double now_seconds) {
        if (drops_packet()) {
            return false;
        }
        enqueue_ready(endpoint, payload, now_seconds + delay_seconds());
        return true;
    }

    bool enqueue(const Endpoint& endpoint, Payload&& payload, double now_seconds) {
        if (drops_packet()) {
            return false;
        }
        enqueue_ready(endpoint, std::move(payload), now_seconds + delay_seconds());
        return true;
    }

    template <typename Fn>
    std::size_t deliver_ready(double now_seconds, Fn&& fn) {
        std::size_t delivered = 0;
        while (!queued_.empty() && queued_.front().deliver_at <= now_seconds) {
            fn(queued_.front().endpoint, queued_.front().payload);
            queued_.pop_front();
            ++delivered;
        }
        return delivered;
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

    std::mt19937& random_engine() noexcept {
        return rng_;
    }

    const std::mt19937& random_engine() const noexcept {
        return rng_;
    }

    SimulatedLinkSettings settings;

private:
    template <typename PacketPayload>
    void enqueue_ready(const Endpoint& endpoint, PacketPayload&& payload, double deliver_at) {
        QueuedPacket queued{
            endpoint,
            std::forward<PacketPayload>(payload),
            deliver_at};
        const auto insert_at = std::upper_bound(
            queued_.begin(),
            queued_.end(),
            queued.deliver_at,
            [](double deliver_at_value, const QueuedPacket& queued_packet) {
                return deliver_at_value < queued_packet.deliver_at;
            });
        queued_.insert(insert_at, std::move(queued));
    }

    std::deque<QueuedPacket> queued_;
    std::mt19937 rng_{0};
};

}  // namespace ashiato::sync
