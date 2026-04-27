#include "kage/sync/sync.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace kage::sync {
namespace {

bool valid_archetype_id(const SyncSettings& settings, SyncArchetypeId id) {
    return id.value < settings.archetypes.size();
}

void validate_component_replication(const ecs::Registry& registry, const ComponentReplication& replication) {
    if (!replication.component || registry.component_info(replication.component) == nullptr) {
        throw std::invalid_argument("sync archetype references an unregistered component");
    }
}

}  // namespace

ReplicationServer::ReplicationServer(ReplicationServerOptions options)
    : options_(options) {}

bool ReplicationServer::add_client(ClientId client) {
    if (client == invalid_client_id || client_to_index_.find(client) != client_to_index_.end()) {
        return false;
    }

    ClientState state;
    state.id = client;
    state.reset_epochs.resize(replicated_.size(), state.epoch);
    state.order.reserve(active_replicated_count_);
    for (std::uint32_t slot = 0; slot < replicated_.size(); ++slot) {
        if (replicated_[slot].active) {
            state.order.push_back(slot);
        }
    }

    const std::size_t index = clients_.size();
    clients_.push_back(std::move(state));
    client_to_index_[client] = index;
    return true;
}

bool ReplicationServer::remove_client(ClientId client) {
    const auto found = client_to_index_.find(client);
    if (found == client_to_index_.end()) {
        return false;
    }

    const std::size_t index = found->second;
    const std::size_t last = clients_.size() - 1;
    if (index != last) {
        clients_[index] = std::move(clients_[last]);
        client_to_index_[clients_[index].id] = index;
    }

    clients_.pop_back();
    client_to_index_.erase(found);
    return true;
}

bool ReplicationServer::has_client(ClientId client) const {
    return client_to_index_.find(client) != client_to_index_.end();
}

std::size_t ReplicationServer::client_count() const noexcept {
    return clients_.size();
}

bool ReplicationServer::add_replicated(ecs::Registry& registry, ecs::Entity entity, SyncArchetypeId archetype) {
    if (!mark_replicated(registry, entity, archetype)) {
        return false;
    }

    const EntityKey key = entity.value;
    const auto found = entity_to_slot_.find(key);
    if (found != entity_to_slot_.end()) {
        replicated_[found->second].archetype = archetype;
        return true;
    }

    const std::uint32_t slot = allocate_slot(entity, archetype);
    entity_to_slot_[key] = slot;
    for (ClientState& client : clients_) {
        if (client.reset_epochs.size() < replicated_.size()) {
            client.reset_epochs.resize(replicated_.size(), client.epoch);
        }
        client.reset_epochs[slot] = client.epoch;
        client.order.push_back(slot);
    }

    ++active_replicated_count_;
    return true;
}

bool ReplicationServer::remove_replicated(ecs::Registry& registry, ecs::Entity entity) {
    register_components(registry);

    const auto found = entity_to_slot_.find(entity.value);
    if (found == entity_to_slot_.end()) {
        return false;
    }

    const bool removed = unmark_replicated(registry, entity);
    deactivate_slot(found->second);
    return removed;
}

bool ReplicationServer::is_replicated(ecs::Entity entity) const {
    return entity_to_slot_.find(entity.value) != entity_to_slot_.end();
}

std::size_t ReplicationServer::replicated_count() const noexcept {
    return active_replicated_count_;
}

std::uint64_t ReplicationServer::priority(ClientId client, ecs::Entity entity) const {
    const auto client_found = client_to_index_.find(client);
    const auto slot_found = entity_to_slot_.find(entity.value);
    if (client_found == client_to_index_.end() || slot_found == entity_to_slot_.end()) {
        return 0;
    }

    const ClientState& state = clients_[client_found->second];
    const std::uint32_t slot = slot_found->second;
    if (slot >= state.reset_epochs.size() || !replicated_[slot].active) {
        return 0;
    }

    return state.epoch - state.reset_epochs[slot];
}

void ReplicationServer::tick(ecs::Registry& registry) {
    tick(registry, ReplicateFn{});
}

void ReplicationServer::tick(ecs::Registry& registry, const ReplicateFn& replicate) {
    register_components(registry);

    std::vector<std::uint32_t> sent;
    std::vector<std::uint32_t> next_order;
    sent.reserve(active_replicated_count_);
    next_order.reserve(active_replicated_count_);

    for (ClientState& client : clients_) {
        ++client.epoch;

        std::size_t remaining = options_.bandwidth_limit_bytes_per_tick;
        std::vector<std::uint32_t> order = std::move(client.order);
        sent.clear();
        next_order.clear();
        next_order.reserve(order.size());

        for (const std::uint32_t slot : order) {
            if (!slot_is_replicable(registry, slot)) {
                if (slot < replicated_.size() && replicated_[slot].active) {
                    deactivate_slot(slot);
                }
                continue;
            }

            if (options_.fixed_entity_replication_cost_bytes > remaining) {
                next_order.push_back(slot);
                continue;
            }

            if (replicate) {
                replicate(client.id, replicated_[slot].entity);
            }
            remaining -= options_.fixed_entity_replication_cost_bytes;
            client.reset_epochs[slot] = client.epoch;
            sent.push_back(slot);
        }

        next_order.insert(next_order.end(), sent.begin(), sent.end());
        client.order = std::move(next_order);
    }
}

std::uint32_t ReplicationServer::allocate_slot(ecs::Entity entity, SyncArchetypeId archetype) {
    if (!free_replicated_slots_.empty()) {
        const std::uint32_t slot = free_replicated_slots_.back();
        free_replicated_slots_.pop_back();
        replicated_[slot] = ReplicatedSlot{entity, archetype, true};
        return slot;
    }

    if (replicated_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::length_error("kage sync replicated slot space exhausted");
    }

    const std::uint32_t slot = static_cast<std::uint32_t>(replicated_.size());
    replicated_.push_back(ReplicatedSlot{entity, archetype, true});
    return slot;
}

void ReplicationServer::deactivate_slot(std::uint32_t slot) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }

    entity_to_slot_.erase(replicated_[slot].entity.value);
    replicated_[slot].active = false;
    free_replicated_slots_.push_back(slot);
    --active_replicated_count_;
    remove_slot_from_client_orders(slot);
}

void ReplicationServer::remove_slot_from_client_orders(std::uint32_t slot) {
    for (ClientState& client : clients_) {
        client.order.erase(std::remove(client.order.begin(), client.order.end(), slot), client.order.end());
    }
}

bool ReplicationServer::slot_is_replicable(const ecs::Registry& registry, std::uint32_t slot) const {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return false;
    }

    const ecs::Entity entity = replicated_[slot].entity;
    return registry.alive(entity) && registry.contains<Replicated>(entity);
}

void register_components(ecs::Registry& registry) {
    registry.register_component<SyncSettings>("kage.sync.SyncSettings");
    registry.register_component<Replicated>("kage.sync.Replicated");
    registry.register_component<NetworkOwner>("kage.sync.NetworkOwner");
}

void configure_server(ecs::Registry& registry) {
    register_components(registry);

    SyncSettings& settings = registry.write<SyncSettings>();
    settings.role = SyncRole::Server;
    settings.local_client = invalid_client_id;
}

void configure_client(ecs::Registry& registry, ClientId local_client) {
    register_components(registry);

    SyncSettings& settings = registry.write<SyncSettings>();
    settings.role = SyncRole::Client;
    settings.local_client = local_client;
}

SyncArchetypeId define_archetype(
    ecs::Registry& registry,
    std::string name,
    std::vector<ComponentReplication> components) {
    register_components(registry);

    for (const ComponentReplication& replication : components) {
        validate_component_replication(registry, replication);
    }

    SyncSettings& settings = registry.write<SyncSettings>();
    const SyncArchetypeId id{static_cast<std::uint32_t>(settings.archetypes.size())};
    settings.archetypes.push_back(SyncArchetype{std::move(name), std::move(components)});
    return id;
}

const SyncArchetype* find_archetype(const ecs::Registry& registry, SyncArchetypeId id) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!valid_archetype_id(settings, id)) {
        return nullptr;
    }

    return &settings.archetypes[id.value];
}

bool mark_replicated(ecs::Registry& registry, ecs::Entity entity, SyncArchetypeId archetype) {
    register_components(registry);

    if (!registry.alive(entity)) {
        return false;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!valid_archetype_id(settings, archetype)) {
        return false;
    }

    return registry.add<Replicated>(entity, Replicated{archetype}) != nullptr;
}

bool unmark_replicated(ecs::Registry& registry, ecs::Entity entity) {
    register_components(registry);
    return registry.remove<Replicated>(entity);
}

bool set_owner(ecs::Registry& registry, ecs::Entity entity, ClientId client) {
    register_components(registry);

    if (!registry.alive(entity)) {
        return false;
    }

    return registry.add<NetworkOwner>(entity, NetworkOwner{client}) != nullptr;
}

}  // namespace kage::sync
