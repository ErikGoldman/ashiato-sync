#pragma once

#include "ashiato/bit_buffer.hpp"
#include "ashiato/sync/protocol.hpp"
#ifdef ASHIATO_SYNC_ENABLE_TRACING
#include "ashiato/sync/tracing.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ashiato::sync::client_detail {

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
        std::vector<ashiato::BitBuffer>& packets,
        std::vector<ClientAckPacketTrace>* traces
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        ,
        const SyncTracer* serialization_tracer = nullptr,
        ClientId client = invalid_client_id,
        SyncFrame frame = 0
#endif
    );

private:
    std::vector<std::uint32_t> pending_;
};

}  // namespace ashiato::sync::client_detail
