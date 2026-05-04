#pragma once

#include "kage/sync/types.hpp"
#include "kage/sync/protocol.hpp"

#include <cstddef>
#include <cstdint>

namespace kage::sync::server_detail {

std::size_t configured_packet_id_bits(const ReplicationServerOptions& options) noexcept;
std::size_t server_update_header_bits(const ReplicationServerOptions& options) noexcept;
std::size_t destroy_record_bits(std::uint32_t network_id, std::size_t tier0_bits) noexcept;

class ServerPacketIdAllocator {
public:
    explicit ServerPacketIdAllocator(std::size_t max_pending_packet_acks_per_client) noexcept;

    std::uint32_t allocate(std::uint32_t& next_packet_id) const noexcept;
    std::uint32_t max_packet_id() const noexcept {
        return max_packet_id_;
    }

private:
    std::uint32_t max_packet_id_ = 1;
};

BitBuffer make_server_packet(
    std::size_t mtu_bytes,
    std::size_t packet_id_bits,
    SyncFrame frame,
    std::uint32_t packet_id,
    SyncFrame input_ack_frame,
    std::uint16_t entity_count,
    const BitBuffer& records);

}  // namespace kage::sync::server_detail
