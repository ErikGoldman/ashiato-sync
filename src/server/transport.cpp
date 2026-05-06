#include "kage/sync/server.hpp"

#include "server/packet.hpp"
#include "server/state.hpp"

#include <algorithm>
#include <cmath>

namespace kage::sync {

void ReplicationServer::send_packet(
    ClientState& client,
    SyncFrame frame,
    std::uint16_t entity_count,
    const ecs::BitBuffer& records,
    const std::vector<PacketAckRecord>& ack_records) {
    if (!options_.transport || entity_count == 0) {
        return;
    }

    const std::uint32_t packet_id = allocate_packet_id(client);
    ecs::BitBuffer packet =
        server_detail::make_server_packet(
            options_.mtu_bytes,
            server_detail::configured_packet_id_bits(options_),
            frame,
            packet_id,
            client.input.ack_frame(),
            entity_count,
            records);
    track_packet_ack(client, packet_id, frame, charged_packet_bytes(packet.byte_size()), ack_records);
    enforce_pending_packet_ack_limit(client);
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    trace_outgoing_update_packet(client, frame, packet_id, client.input.ack_frame(), ack_records);
#endif
    options_.transport(client.peer, packet);
}

void ReplicationServer::send_connect_response(ClientState& client) {
    if (!options_.transport) {
        return;
    }
    ecs::BitBuffer packet;
    packet.reserve_bytes(options_.mtu_bytes);
    packet.push_bits(protocol::server_connect_response_message, 8U);
    packet.push_bool(true);
    packet.push_unsigned_bits(client.id, 64U);
    options_.transport(client.peer, packet);
}

void ReplicationServer::send_pong(
    ClientId peer,
    std::uint32_t sequence,
    SyncFrame send_frame,
    std::uint16_t send_subframe) {
    if (!options_.transport) {
        return;
    }
    const double subframe = tick_accumulator_seconds_ / options_.fixed_dt_seconds;
    const auto server_subframe = static_cast<std::uint16_t>(std::clamp(
        static_cast<std::uint32_t>(std::floor(subframe * static_cast<double>(protocol::frame_subframe_scale))),
        std::uint32_t{0},
        protocol::frame_subframe_scale - 1U));
    ecs::BitBuffer packet;
    packet.reserve_bytes(options_.mtu_bytes);
    packet.push_bits(protocol::server_pong_message, 8U);
    packet.push_bits(sequence, 32U);
    packet.push_bits(send_frame, 32U);
    packet.push_bits(send_subframe, protocol::frame_subframe_bits);
    packet.push_bits(frame_, 32U);
    packet.push_bits(server_subframe, protocol::frame_subframe_bits);
    options_.transport(peer, packet);
}

}  // namespace kage::sync
