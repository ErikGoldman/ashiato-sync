#pragma once

#include "ecs/bit_buffer.hpp"
#include "kage/sync/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kage::sync::client_detail {

struct ClientAckPacketTrace {
    std::vector<std::uint32_t> acks;
};

class ClientAckQueue {
public:
    void push(std::uint32_t packet_id);
    std::size_t size() const noexcept;
    std::vector<std::uint32_t>& pending() noexcept;

    void drain_ack_packets(
        std::size_t mtu_bytes,
        std::size_t packet_id_bits,
        std::vector<ecs::BitBuffer>& packets,
        std::vector<ClientAckPacketTrace>* traces);

private:
    std::vector<std::uint32_t> pending_;
};

}  // namespace kage::sync::client_detail
