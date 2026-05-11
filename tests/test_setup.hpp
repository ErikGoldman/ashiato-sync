#pragma once

#include "kage/sync/sync.hpp"

#include <stdexcept>

namespace kage_sync_tests {

inline bool configure_test_server_registry(ecs::Registry& registry) {
    kage::sync::register_components(registry);
    kage::sync::SyncSettings& settings = registry.write<kage::sync::SyncSettings>();
    settings.role = kage::sync::SyncRole::Server;
    settings.local_client = kage::sync::invalid_client_id;
    registry.write<kage::sync::SyncAuthority>().authoritative = true;
    return true;
}

inline bool configure_test_client_registry(ecs::Registry& registry, kage::sync::ClientId client) {
    if (client == kage::sync::invalid_client_id || client > kage::sync::max_client_entity_network_id_client) {
        throw std::invalid_argument("client id cannot fit in client entity network ids");
    }
    kage::sync::register_components(registry);
    kage::sync::SyncSettings& settings = registry.write<kage::sync::SyncSettings>();
    settings.role = kage::sync::SyncRole::Client;
    settings.local_client = client;
    registry.write<kage::sync::SyncAuthority>().authoritative = false;
    return true;
}

inline kage::sync::ReplicationClientOptions make_test_client_options(
    ecs::Registry& registry,
    kage::sync::ReplicationClientOptions options = {}) {
    kage::sync::register_components(registry);
    if (options.session.local_client == kage::sync::invalid_client_id) {
        options.session.local_client = registry.get<kage::sync::SyncSettings>().local_client;
    }
    return options;
}

}  // namespace kage_sync_tests
