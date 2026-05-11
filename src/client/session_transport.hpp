#pragma once

#include "client/state.hpp"

#include "ashiato/bit_buffer.hpp"
#include "ashiato/sync/client.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ashiato::sync::client_detail {

class ClientSessionTransport {
public:
    std::unordered_map<std::uint32_t, PendingPing> pending_pings;
    std::vector<ashiato::BitBuffer> inbound_packets;
    std::function<void(const ashiato::BitBuffer&)> packet_sender;
    std::string connect_error;
    ReplicationClientConnectionState connection_state = ReplicationClientConnectionState::Connecting;
    double connect_resend_accumulator_seconds = 0.0;
    double ping_accumulator_seconds = 0.0;
    std::uint32_t next_ping_sequence = 1;
    std::uint32_t stable_ping_samples = 0;
    bool sent_initial_connect_request = false;
    bool sent_initial_ping = false;
    bool adaptive_ping_active = true;
};

}  // namespace ashiato::sync::client_detail
