#pragma once

#include "kage/sync/client_clock.hpp"

#include <cstddef>
#include <cstdint>

namespace kage::sync::client_detail {

class ClientTimingStatsCalculator {
public:
    explicit ClientTimingStatsCalculator(std::size_t max_pending_packet_acks_per_client) noexcept;

    void record_entity_update_packet(std::uint32_t packet_id, ReplicationClientTimingStats& stats) noexcept;

private:
    std::uint32_t max_packet_id_ = 0;
    std::uint32_t highest_entity_update_packet_id_ = 0;
    std::uint64_t entity_update_packet_window_mask_ = 0;
    std::uint32_t entity_update_packet_window_span_ = 0;
    bool has_entity_update_packet_window_ = false;
};

}  // namespace kage::sync::client_detail
