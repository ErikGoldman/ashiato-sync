#pragma once

#include "kage/sync/client_clock.hpp"

#include <cstddef>
#include <cstdint>

namespace kage::sync::client_detail {

class ClientPacketWindow {
public:
    explicit ClientPacketWindow(std::size_t max_pending_packet_acks_per_client) noexcept;

    void record(std::uint32_t packet_id, ReplicationClientTimingStats& stats) noexcept;

private:
    std::uint32_t max_packet_id_ = 0;
    std::uint32_t highest_packet_id_ = 0;
    std::uint64_t window_mask_ = 0;
    std::uint32_t window_span_ = 0;
    bool has_window_ = false;
};

void record_packet_window(
    std::uint32_t packet_id,
    std::size_t max_pending_packet_acks_per_client,
    ReplicationClientTimingStats& stats,
    std::uint32_t& highest_packet_id,
    std::uint64_t& window_mask,
    std::uint32_t& window_span,
    bool& has_window) noexcept;

}  // namespace kage::sync::client_detail
