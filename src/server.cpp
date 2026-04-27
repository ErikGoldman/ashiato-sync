#include "kage/sync/server.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace kage::sync {

ReplicationServer::ReplicationServer(ReplicationServerOptions options)
    : options_(options) {}

void ReplicationServer::set_transport(TransportFn transport) {
    options_.transport = std::move(transport);
}

bool ReplicationServer::add_client(ClientId client) {
    if (client == invalid_client_id || client_to_index_.find(client) != client_to_index_.end()) {
        return false;
    }

    ClientState state;
    state.id = client;
    state.reset_epochs.resize(replicated_.size(), state.epoch);
    state.baselines.resize(replicated_.size());
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
        for (ClientState& client : clients_) {
            if (found->second < client.baselines.size()) {
                client.baselines[found->second].clear();
            }
        }
        return true;
    }

    const std::uint32_t slot = allocate_slot(entity, archetype);
    entity_to_slot_[key] = slot;
    for (ClientState& client : clients_) {
        if (client.reset_epochs.size() < replicated_.size()) {
            client.reset_epochs.resize(replicated_.size(), client.epoch);
            client.baselines.resize(replicated_.size());
        }
        client.reset_epochs[slot] = client.epoch;
        client.baselines[slot].clear();
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
    if (options_.transport) {
        tick_serialized(registry);
        return;
    }

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

void ReplicationServer::tick_serialized(ecs::Registry& registry) {
    register_components(registry);

    const SyncSettings& settings = registry.get<SyncSettings>();
    std::vector<std::uint32_t> sent;
    std::vector<std::uint32_t> next_order;
    BitBuffer buffer;
    std::vector<QuantizedBaseline> current_baselines;

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

            buffer.clear();
            current_baselines.clear();
            if (!serialize_entity(registry, settings, client, slot, buffer, current_baselines)) {
                next_order.push_back(slot);
                continue;
            }

            if (buffer.byte_size() > remaining) {
                next_order.push_back(slot);
                continue;
            }

            options_.transport(client.id, buffer);
            remaining -= buffer.byte_size();
            client.reset_epochs[slot] = client.epoch;
            client.baselines[slot] = current_baselines;
            sent.push_back(slot);
        }

        next_order.insert(next_order.end(), sent.begin(), sent.end());
        client.order = std::move(next_order);
    }
}

bool ReplicationServer::serialize_entity(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    const ClientState& client,
    std::uint32_t slot,
    BitBuffer& out,
    std::vector<QuantizedBaseline>& current) const {
    if (slot >= replicated_.size() || slot >= client.baselines.size()) {
        return false;
    }

    const ReplicatedSlot& replicated = replicated_[slot];
    if (replicated.archetype.value >= settings.archetypes.size()) {
        return false;
    }

    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    const NetworkOwner* owner = registry.try_get<NetworkOwner>(replicated.entity);
    const std::vector<QuantizedBaseline>& previous_baselines = client.baselines[slot];
    bool serialized_component = false;

    current.reserve(archetype.components.size());
    for (const ComponentReplication& replication : archetype.components) {
        if (replication.audience == ReplicationAudience::Owner &&
            (owner == nullptr || owner->client != client.id)) {
            continue;
        }

        const void* component_value = registry.get(replicated.entity, replication.component);
        if (component_value == nullptr) {
            continue;
        }

        const auto found_ops = settings.component_ops.find(replication.component.value);
        if (found_ops == settings.component_ops.end()) {
            throw std::logic_error("sync component traits are not registered for replicated component");
        }

        QuantizedBaseline quantized;
        quantized.component = replication.component;
        found_ops->second.quantize(component_value, quantized.bytes);

        const auto previous_found = std::find_if(
            previous_baselines.begin(),
            previous_baselines.end(),
            [&replication](const QuantizedBaseline& baseline) {
                return baseline.component == replication.component;
            });
        const SyncComponentOps::QuantizedBytes* previous =
            previous_found != previous_baselines.end() ? &previous_found->bytes : nullptr;
        found_ops->second.serialize(previous, quantized.bytes, out);
        current.push_back(std::move(quantized));
        serialized_component = true;
    }

    return serialized_component;
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
    for (ClientState& client : clients_) {
        if (slot < client.baselines.size()) {
            client.baselines[slot].clear();
        }
    }
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

}  // namespace kage::sync
