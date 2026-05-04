#pragma once

#include "kage/sync/client.hpp"
#include "kage/sync/client_clock.hpp"
#include "kage/sync/server.hpp"

namespace kage::sync::detail {

ReplicationClientOptions validate_client_options(ReplicationClientOptions options);
ReplicationClientClockConfig validate_client_clock_config(ReplicationClientClockConfig config);
ReplicationServerOptions validate_server_options(ReplicationServerOptions options);

}  // namespace kage::sync::detail
