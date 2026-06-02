#include "server/packet.hpp"

namespace ashiato::sync::server_detail {

std::size_t configured_packet_id_bits(const ReplicationServerOptions& options) noexcept {
    return protocol::packet_id_bits(options.protocol);
}

std::size_t server_update_header_bits(const ReplicationServerOptions& options) noexcept {
    return protocol::server_update_header_bits_for(options.protocol);
}

std::size_t destroy_record_bits(std::uint32_t network_id, std::size_t tier0_bits) noexcept {
    return 1U + protocol::network_entity_id_encoded_bits(network_id, tier0_bits);
}

ServerPacketIdAllocator::ServerPacketIdAllocator(std::size_t max_pending_packet_acks_per_client) noexcept
    : max_packet_id_(
          protocol::packet_id_mask(protocol::packet_id_bits_for_max_pending(max_pending_packet_acks_per_client))) {}

std::uint32_t ServerPacketIdAllocator::allocate(std::uint32_t& next_packet_id) const noexcept {
    if (next_packet_id == 0U || next_packet_id > max_packet_id_) {
        next_packet_id = 1U;
    }

    const std::uint32_t packet_id = next_packet_id;
    next_packet_id = packet_id == max_packet_id_ ? 1U : packet_id + 1U;
    return packet_id;
}

ashiato::BitBuffer make_server_packet(
    std::size_t mtu_bytes,
    std::size_t packet_id_bits,
    SyncFrame frame,
    std::uint32_t packet_id,
    SyncFrame input_ack_frame,
    std::uint16_t entity_count,
    const ashiato::BitBuffer& records) {
    ashiato::BitBuffer packet;
    packet.reserve_bytes(mtu_bytes);
    packet.write_bits(protocol::server_update_message, protocol::message_bits);
    packet.write_bits(frame, 32U);
    packet.write_bits(packet_id, packet_id_bits);
    packet.write_bits(input_ack_frame, 32U);
    packet.write_bits(entity_count, 16U);
    packet.write_buffer_bits(records);
    return packet;
}

}  // namespace ashiato::sync::server_detail
