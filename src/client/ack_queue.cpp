#include "client/ack_queue.hpp"

#include <algorithm>
#include <limits>

namespace kage::sync::client_detail {

void ClientAckQueue::push(std::uint32_t packet_id) {
    pending_.push_back(packet_id);
}

std::size_t ClientAckQueue::size() const noexcept {
    return pending_.size();
}

std::vector<std::uint32_t>& ClientAckQueue::pending() noexcept {
    return pending_;
}

void ClientAckQueue::drain_ack_packets(
    std::size_t mtu_bytes,
    std::size_t packet_id_bits,
    std::vector<BitBuffer>& packets,
    std::vector<ClientAckPacketTrace>* traces) {
    if (pending_.empty()) {
        pending_.clear();
        return;
    }

    const std::size_t one_ack_bytes = protocol::bytes_for_bits(protocol::client_ack_header_bits + packet_id_bits);
    if (one_ack_bytes > mtu_bytes) {
        return;
    }

    const std::size_t mtu_bits = mtu_bytes * 8U;
    const std::size_t max_acks_per_packet =
        std::min<std::size_t>(
            std::numeric_limits<std::uint16_t>::max(),
            (mtu_bits - protocol::client_ack_header_bits) / packet_id_bits);
    if (max_acks_per_packet == 0U) {
        return;
    }

    packets.reserve(packets.size() + (pending_.size() + max_acks_per_packet - 1U) / max_acks_per_packet);
    BitBuffer packet;
    std::uint16_t packet_ack_count = 0;
    std::size_t packet_count_offset = 0;
    ClientAckPacketTrace trace;
    auto begin_packet = [&]() {
        packet.clear();
        packet.reserve_bytes(mtu_bytes);
        packet.push_bits(protocol::client_ack_message, 8U);
        packet_count_offset = packet.bit_size();
        packet.push_bits(0, 16U);
        packet_ack_count = 0;
        trace.acks.clear();
    };
    auto finish_packet = [&]() {
        if (packet_ack_count == 0U) {
            return;
        }
        packet.overwrite_unsigned_bits(packet_count_offset, packet_ack_count, 16U);
        if (traces != nullptr) {
            traces->push_back(trace);
        }
        packets.push_back(std::move(packet));
        packet = BitBuffer{};
    };

    begin_packet();
    for (const std::uint32_t packet_id : pending_) {
        if (packet_ack_count == max_acks_per_packet) {
            finish_packet();
            begin_packet();
        }
        packet.push_bits(packet_id, packet_id_bits);
        trace.acks.push_back(packet_id);
        ++packet_ack_count;
    }
    finish_packet();

    pending_.clear();
}

}  // namespace kage::sync::client_detail
