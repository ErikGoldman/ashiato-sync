#include "client/timing_stats.hpp"

#include "kage/sync/protocol.hpp"

#include <algorithm>
#include <limits>

namespace kage::sync::client_detail {
namespace {

constexpr std::uint32_t entity_update_packet_loss_window = 64;

std::uint32_t count_bits(std::uint64_t value) noexcept {
    std::uint32_t count = 0;
    while (value != 0U) {
        value &= value - 1U;
        ++count;
    }
    return count;
}

std::uint32_t packet_id_forward_distance(
    std::uint32_t from,
    std::uint32_t to,
    std::uint32_t max_packet_id) noexcept {
    return to >= from ? to - from : max_packet_id - from + to;
}

std::uint32_t packet_id_window_size(std::uint32_t max_packet_id) noexcept {
    if (max_packet_id <= 1U) {
        return 1U;
    }
    return std::min(entity_update_packet_loss_window, max_packet_id / 2U);
}

std::uint64_t low_bit_mask(std::uint32_t bit_count) noexcept {
    return bit_count >= 64U ? std::numeric_limits<std::uint64_t>::max() : ((std::uint64_t{1} << bit_count) - 1U);
}

}  // namespace

ClientTimingStatsCalculator::ClientTimingStatsCalculator(std::size_t max_pending_packet_acks_per_client) noexcept
    : max_packet_id_(protocol::packet_id_mask(protocol::packet_id_bits_for_max_pending(max_pending_packet_acks_per_client))) {}

void ClientTimingStatsCalculator::record_entity_update_packet(
    std::uint32_t packet_id,
    ReplicationClientTimingStats& stats) noexcept {
    if (packet_id == 0U || packet_id > max_packet_id_) {
        return;
    }

    ++stats.server_update_packets_received;

    const std::uint32_t window_size = packet_id_window_size(max_packet_id_);
    if (!has_entity_update_packet_window_) {
        highest_entity_update_packet_id_ = packet_id;
        entity_update_packet_window_mask_ = 1U;
        entity_update_packet_window_span_ = 1U;
        has_entity_update_packet_window_ = true;
    } else {
        const std::uint32_t forward_distance =
            packet_id_forward_distance(highest_entity_update_packet_id_, packet_id, max_packet_id_);
        const std::uint32_t backward_distance =
            packet_id_forward_distance(packet_id, highest_entity_update_packet_id_, max_packet_id_);

        if (forward_distance != 0U && forward_distance <= backward_distance) {
            const std::uint32_t new_span = std::min(window_size, entity_update_packet_window_span_ + forward_distance);
            const std::uint32_t finalized_count =
                entity_update_packet_window_span_ + forward_distance - new_span;

            if (finalized_count != 0U) {
                std::uint32_t finalized_received = 0;
                if (forward_distance < 64U) {
                    const std::uint32_t retained_old_count =
                        new_span > forward_distance ? new_span - forward_distance : 0U;
                    const std::uint64_t retained_old_mask = low_bit_mask(retained_old_count);
                    finalized_received = count_bits(entity_update_packet_window_mask_ & ~retained_old_mask);
                } else {
                    finalized_received = count_bits(entity_update_packet_window_mask_);
                }
                stats.server_update_packets_missing += finalized_count - finalized_received;
            }

            entity_update_packet_window_mask_ =
                forward_distance >= 64U ? 0U : (entity_update_packet_window_mask_ << forward_distance);
            entity_update_packet_window_mask_ |= 1U;
            entity_update_packet_window_mask_ &= low_bit_mask(window_size);
            entity_update_packet_window_span_ = new_span;
            highest_entity_update_packet_id_ = packet_id;
        } else {
            ++stats.server_update_packets_reordered_or_duplicate;
            if (backward_distance < entity_update_packet_window_span_ && backward_distance < 64U) {
                entity_update_packet_window_mask_ |= std::uint64_t{1} << backward_distance;
            }
        }
    }

    const std::uint64_t total = stats.server_update_packets_received + stats.server_update_packets_missing;
    stats.server_update_packet_loss = total == 0U
        ? 0.0f
        : static_cast<float>(stats.server_update_packets_missing) / static_cast<float>(total);
}

}  // namespace kage::sync::client_detail
