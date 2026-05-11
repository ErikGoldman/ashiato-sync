#pragma once

#include "ashiato/sync/sync.hpp"

#include <stdexcept>

namespace ashiato_sync_tests {

inline bool configure_test_server_registry(ashiato::Registry& registry) {
    ashiato::sync::register_components(registry);
    ashiato::sync::SyncSettings& settings = registry.write<ashiato::sync::SyncSettings>();
    settings.role = ashiato::sync::SyncRole::Server;
    settings.local_client = ashiato::sync::invalid_client_id;
    registry.write<ashiato::sync::SyncAuthority>().authoritative = true;
    return true;
}

inline bool configure_test_client_registry(ashiato::Registry& registry, ashiato::sync::ClientId client) {
    if (client == ashiato::sync::invalid_client_id || client > ashiato::sync::max_client_entity_network_id_client) {
        throw std::invalid_argument("client id cannot fit in client entity network ids");
    }
    ashiato::sync::register_components(registry);
    ashiato::sync::SyncSettings& settings = registry.write<ashiato::sync::SyncSettings>();
    settings.role = ashiato::sync::SyncRole::Client;
    settings.local_client = client;
    registry.write<ashiato::sync::SyncAuthority>().authoritative = false;
    return true;
}

inline ashiato::sync::ReplicationClientOptions make_test_client_options(
    ashiato::Registry& registry,
    ashiato::sync::ReplicationClientOptions options = {}) {
    ashiato::sync::register_components(registry);
    if (options.session.local_client == ashiato::sync::invalid_client_id) {
        options.session.local_client = registry.get<ashiato::sync::SyncSettings>().local_client;
    }
    return options;
}

}  // namespace ashiato_sync_tests
