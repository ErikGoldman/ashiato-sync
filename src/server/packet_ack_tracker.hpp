#pragma once

#include "server/packet.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kage::sync::server_detail {

template <typename PendingPacketAck>
std::uint32_t allocate_tracked_packet_id(
    std::uint32_t& next_packet_id,
    std::vector<PendingPacketAck>& pending_packet_acks,
    std::size_t max_pending_packet_acks_per_client) {
    const ServerPacketIdAllocator allocator(max_pending_packet_acks_per_client);
    const std::uint32_t packet_id = allocator.allocate(next_packet_id);
    pending_packet_acks.erase(
        std::remove_if(
            pending_packet_acks.begin(),
            pending_packet_acks.end(),
            [packet_id](const PendingPacketAck& pending) {
                return pending.packet_id == packet_id;
            }),
        pending_packet_acks.end());
    return packet_id;
}

template <typename PendingPacketAck>
void enforce_pending_packet_ack_limit(
    std::vector<PendingPacketAck>& pending_packet_acks,
    std::size_t max_pending_packet_acks_per_client) {
    if (pending_packet_acks.size() > max_pending_packet_acks_per_client) {
        pending_packet_acks.erase(
            pending_packet_acks.begin(),
            pending_packet_acks.begin() +
                static_cast<typename std::vector<PendingPacketAck>::difference_type>(
                    pending_packet_acks.size() - max_pending_packet_acks_per_client));
    }
}

template <typename PendingPacketAck, typename PacketAckRecord>
void track_packet_ack(
    std::vector<PendingPacketAck>& pending_packet_acks,
    std::uint32_t packet_id,
    const std::vector<PacketAckRecord>& records) {
    if (records.empty()) {
        return;
    }
    pending_packet_acks.push_back(PendingPacketAck{packet_id, records});
}

}  // namespace kage::sync::server_detail
