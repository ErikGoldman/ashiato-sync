#pragma once

#include "ashiato/sync/client.hpp"
#include "ashiato/sync/client_clock.hpp"
#include "ashiato/sync/server.hpp"

namespace ashiato::sync::detail {

ReplicationClientOptions validate_client_options(
    ReplicationClientOptions options,
    std::size_t buffered_frame_capacity = ReplicationClient::buffered_frame_capacity,
    std::size_t prediction_frame_capacity = ReplicationClient::prediction_frame_capacity);
ReplicationClientClockConfig validate_client_clock_config(ReplicationClientClockConfig config);
ReplicationServerOptions validate_server_options(ReplicationServerOptions options);

}  // namespace ashiato::sync::detail
