#include "client/packet_window.hpp"

#include "kage/sync/protocol.hpp"

#include <algorithm>
#include <limits>

namespace kage::sync::client_detail {
namespace {

constexpr std::uint32_t server_update_packet_loss_window = 64;

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
    return std::min(server_update_packet_loss_window, max_packet_id / 2U);
}

std::uint64_t low_bit_mask(std::uint32_t bit_count) noexcept {
    return bit_count >= 64U ? std::numeric_limits<std::uint64_t>::max() : ((std::uint64_t{1} << bit_count) - 1U);
}

void record_packet_window_with_max(
    std::uint32_t packet_id,
    std::uint32_t max_packet_id,
    ReplicationClientTimingStats& stats,
    std::uint32_t& highest_packet_id,
    std::uint64_t& window_mask,
    std::uint32_t& window_span,
    bool& has_window) noexcept {
    if (packet_id == 0U || packet_id > max_packet_id) {
        return;
    }

    ++stats.server_update_packets_received;

    const std::uint32_t window_size = packet_id_window_size(max_packet_id);
    if (!has_window) {
        highest_packet_id = packet_id;
        window_mask = 1U;
        window_span = 1U;
        has_window = true;
    } else {
        const std::uint32_t forward_distance =
            packet_id_forward_distance(highest_packet_id, packet_id, max_packet_id);
        const std::uint32_t backward_distance =
            packet_id_forward_distance(packet_id, highest_packet_id, max_packet_id);

        if (forward_distance != 0U && forward_distance <= backward_distance) {
            const std::uint32_t new_span = std::min(window_size, window_span + forward_distance);
            const std::uint32_t finalized_count = window_span + forward_distance - new_span;

            if (finalized_count != 0U) {
                std::uint32_t finalized_received = 0;
                if (forward_distance < 64U) {
                    const std::uint32_t retained_old_count = new_span > forward_distance ? new_span - forward_distance : 0U;
                    const std::uint64_t retained_old_mask = low_bit_mask(retained_old_count);
                    finalized_received = count_bits(window_mask & ~retained_old_mask);
                } else {
                    finalized_received = count_bits(window_mask);
                }
                stats.server_update_packets_missing += finalized_count - finalized_received;
            }

            window_mask = forward_distance >= 64U ? 0U : (window_mask << forward_distance);
            window_mask |= 1U;
            window_mask &= low_bit_mask(window_size);
            window_span = new_span;
            highest_packet_id = packet_id;
        } else {
            ++stats.server_update_packets_reordered_or_duplicate;
            if (backward_distance < window_span && backward_distance < 64U) {
                window_mask |= std::uint64_t{1} << backward_distance;
            }
        }
    }

    const std::uint64_t total = stats.server_update_packets_received + stats.server_update_packets_missing;
    stats.server_update_packet_loss = total == 0U
        ? 0.0f
        : static_cast<float>(stats.server_update_packets_missing) / static_cast<float>(total);
}

}  // namespace

ClientPacketWindow::ClientPacketWindow(std::size_t max_pending_packet_acks_per_client) noexcept
    : max_packet_id_(protocol::packet_id_mask(protocol::packet_id_bits_for_max_pending(max_pending_packet_acks_per_client))) {}

void ClientPacketWindow::record(std::uint32_t packet_id, ReplicationClientTimingStats& stats) noexcept {
    record_packet_window_with_max(
        packet_id,
        max_packet_id_,
        stats,
        highest_packet_id_,
        window_mask_,
        window_span_,
        has_window_);
}

void record_packet_window(
    std::uint32_t packet_id,
    std::size_t max_pending_packet_acks_per_client,
    ReplicationClientTimingStats& stats,
    std::uint32_t& highest_packet_id,
    std::uint64_t& window_mask,
    std::uint32_t& window_span,
    bool& has_window) noexcept {
    record_packet_window_with_max(
        packet_id,
        protocol::packet_id_mask(protocol::packet_id_bits_for_max_pending(max_pending_packet_acks_per_client)),
        stats,
        highest_packet_id,
        window_mask,
        window_span,
        has_window);
}

}  // namespace kage::sync::client_detail
