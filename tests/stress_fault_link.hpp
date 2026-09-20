#pragma once

#include "ashiato/bit_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <utility>
#include <vector>

namespace ashiato_sync_tests {

struct PacketFault {
    std::uint32_t delay_ticks = 0;
    std::uint32_t duplicate_count = 0;
    std::size_t flipped_bit = std::numeric_limits<std::size_t>::max();
    bool drop = false;
};

template <typename Endpoint>
class PacketFaultLink {
public:
    struct QueuedPacket {
        Endpoint endpoint;
        ashiato::BitBuffer payload;
        std::uint64_t delivery_tick = 0;
        std::uint64_t sequence = 0;
    };

    std::uint64_t enqueue(
        Endpoint endpoint,
        const ashiato::BitBuffer& payload,
        std::uint64_t current_tick,
        PacketFault fault = {}) {
        const std::uint64_t sequence = next_sequence_++;
        saved_.push_back(QueuedPacket{endpoint, payload, current_tick, sequence});
        if (fault.drop) {
            ++dropped_;
            return sequence;
        }

        for (std::uint32_t copy = 0; copy <= fault.duplicate_count; ++copy) {
            ashiato::BitBuffer queued_payload = payload;
            if (fault.flipped_bit < queued_payload.bit_size()) {
                const std::size_t byte_index = fault.flipped_bit / 8U;
                const std::size_t bit_index = fault.flipped_bit % 8U;
                std::vector<std::uint8_t> bytes = queued_payload.bytes();
                bytes[byte_index] ^= static_cast<std::uint8_t>(std::uint8_t{1U} << bit_index);
                queued_payload.assign_bytes(std::move(bytes), queued_payload.bit_size());
            }
            queue_.push_back(QueuedPacket{
                endpoint,
                std::move(queued_payload),
                current_tick + fault.delay_ticks,
                sequence});
        }
        sort_queue();
        return sequence;
    }

    bool replay(std::uint64_t sequence, std::uint64_t delivery_tick) {
        const auto found = std::find_if(saved_.begin(), saved_.end(), [sequence](const QueuedPacket& packet) {
            return packet.sequence == sequence;
        });
        if (found == saved_.end()) {
            return false;
        }
        queue_.push_back(QueuedPacket{found->endpoint, found->payload, delivery_tick, found->sequence});
        sort_queue();
        return true;
    }

    template <typename Fn>
    std::size_t deliver_ready(std::uint64_t current_tick, Fn&& receive) {
        std::size_t delivered = 0;
        while (!queue_.empty() && queue_.front().delivery_tick <= current_tick) {
            QueuedPacket packet = std::move(queue_.front());
            queue_.erase(queue_.begin());
            receive(packet.endpoint, packet.payload, packet.sequence);
            ++delivered;
        }
        delivered_ += delivered;
        return delivered;
    }

    std::size_t queued_count() const noexcept {
        return queue_.size();
    }

    std::size_t saved_count() const noexcept {
        return saved_.size();
    }

    std::uint64_t dropped_count() const noexcept {
        return dropped_;
    }

    std::uint64_t delivered_count() const noexcept {
        return delivered_;
    }

private:
    void sort_queue() {
        std::stable_sort(queue_.begin(), queue_.end(), [](const QueuedPacket& lhs, const QueuedPacket& rhs) {
            return lhs.delivery_tick < rhs.delivery_tick;
        });
    }

    std::deque<QueuedPacket> queue_;
    std::vector<QueuedPacket> saved_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t dropped_ = 0;
    std::uint64_t delivered_ = 0;
};

}  // namespace ashiato_sync_tests
