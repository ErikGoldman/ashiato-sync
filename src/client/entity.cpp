#include "ashiato/sync/client.hpp"

#include "client/runtime/buffered_runtime.hpp"
#include "client/runtime/cue_runtime.hpp"
#include "client/runtime/prediction_runtime.hpp"
#include "client/store/entity_store.hpp"
#include "client/state.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ashiato::sync {
namespace {

constexpr std::uint32_t invalid_entity_index = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t max_baseline_history_per_entity = 64;

}  // namespace

ReplicationClient::EntityState* ReplicationClient::find_entity_state(
    ClientEntityNetworkId client_entity_network_id) noexcept {
    return entity_store_->find_by_client_entity_network_id(client_entity_network_id);
}

const ReplicationClient::EntityState* ReplicationClient::find_entity_state(
    ClientEntityNetworkId client_entity_network_id) const noexcept {
    return entity_store_->find_by_client_entity_network_id(client_entity_network_id);
}

ReplicationClient::EntityState* ReplicationClient::find_entity_state_by_wire_id(
    std::uint32_t wire_network_id) noexcept {
    return entity_store_->find_by_wire_id(wire_network_id);
}

const ReplicationClient::EntityState* ReplicationClient::find_entity_state_by_wire_id(
    std::uint32_t wire_network_id) const noexcept {
    return entity_store_->find_by_wire_id(wire_network_id);
}

ReplicationClient::EntityState* ReplicationClient::find_entity_state_for_local(ashiato::Entity local) noexcept {
    if (!local) {
        return nullptr;
    }
    return entity_store_->find_by_local_entity(local);
}

#ifdef ASHIATO_SYNC_ENABLE_TRACING
void ReplicationClient::register_local_entity_index(const EntityState& state) {
    entity_store_->register_local_entity_index(state);
}

void ReplicationClient::unregister_local_entity_index(const EntityState& state) {
    entity_store_->unregister_local_entity_index(state);
}
#endif

ReplicationClient::EntityState* ReplicationClient::ensure_entity_state(
    ashiato::Registry& registry,
    ClientEntityNetworkId client_entity_network_id,
    std::uint32_t wire_network_id) {
    (void)registry;
    if (client_entity_network_id == invalid_client_entity_network_id || wire_network_id == 0U) {
        return nullptr;
    }
    return entity_store_->ensure_entity_state(client_entity_network_id, wire_network_id);
}

void ReplicationClient::erase_entity_state(
    ashiato::Registry& registry,
    std::uint32_t entity_index,
    bool destroy_local) {
    if (!entity_store_->contains_entity_index(entity_index)) {
        return;
    }

    EntityState& state = entity_store_->state_unchecked(entity_index);
    if (state.identity.client_entity_network_id == invalid_client_entity_network_id) {
        return;
    }
    entity_store_->unmap_entity_state(entity_index);

    if (state.identity.local) {
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        unregister_local_entity_index(state);
#endif
        if (destroy_local && registry.alive(state.identity.local)) {
            registry.destroy(state.identity.local);
        }
    }

    buffered_runtime_->reset_entity(entity_index);
    prediction_->reset_entity(entity_index);
    cue_runtime_->erase_for_entity(entity_index);
    entity_store_->release_entity_state(entity_index);
}

void ReplicationClient::sync_entity_memberships(EntityState& state) {
    if (state.identity.client_entity_network_id == invalid_client_entity_network_id) {
        return;
    }
    const std::uint32_t entity_index =
        entity_store_->entity_index_for_client_entity_network_id(state.identity.client_entity_network_id);
    if (entity_index == invalid_entity_index) {
        return;
    }
    entity_store_->sync_mode_memberships(entity_index);
}

bool ReplicationClient::destroy_tombstone_blocks(std::uint32_t wire_network_id, SyncFrame frame) const {
    return entity_store_->destroy_tombstone_blocks(wire_network_id, frame);
}

void ReplicationClient::record_destroy_tombstone(std::uint32_t wire_network_id, SyncFrame frame) {
    (void)entity_store_->record_destroy_tombstone(wire_network_id, frame);
}

const QuantizedFrameData* ReplicationClient::find_baseline(
    const EntityState& state,
    SyncFrame frame) const noexcept {
    if (state.replication.history.empty()) {
        return nullptr;
    }

    const std::size_t count = state.replication.history.size();
    if (count == max_baseline_history_per_entity) {
        const client_detail::EntityFrameBaseline& baseline =
            state.replication.history[frame & (max_baseline_history_per_entity - 1U)];
        return baseline.valid && baseline.frame == frame ? &baseline.baseline : nullptr;
    }

    for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t index = (state.replication.history_next + count - 1U - offset) % count;
        const client_detail::EntityFrameBaseline& baseline = state.replication.history[index];
        if (baseline.valid && baseline.frame == frame) {
            return &baseline.baseline;
        }
    }
    return nullptr;
}

ashiato::Entity ReplicationClient::local_entity(ClientEntityNetworkId client_entity_network_id) const {
    const EntityState* state = find_entity_state(client_entity_network_id);
    return state != nullptr ? state->identity.local : ashiato::Entity{};
}

bool ReplicationClient::is_alive_client_entity_network_id(ClientEntityNetworkId client_entity_network_id) const noexcept {
    const EntityState* state = find_entity_state(client_entity_network_id);
    return state != nullptr && state->identity.local;
}

EntityReferenceStatus ReplicationClient::resolve_entity_reference(EntityReference& reference) const noexcept {
    const ClientEntityNetworkId client_entity_network_id = reference.client_entity_network_id;
    if (client_entity_network_id == invalid_client_entity_network_id) {
        reference = EntityReference{};
        return EntityReferenceStatus::Invalid;
    }

    const std::uint32_t wire_network_id = client_entity_network_id_wire_id(client_entity_network_id);
    if (wire_network_id == 0U || wire_network_id > max_client_local_wire_network_id) {
        reference = EntityReference{};
        return EntityReferenceStatus::Invalid;
    }

    const EntityState* state = find_entity_state(client_entity_network_id);
    if (state != nullptr) {
        if (state->identity.local) {
            reference.entity = state->identity.local;
            return EntityReferenceStatus::Alive;
        }
        reference.entity = ashiato::Entity{};
        return EntityReferenceStatus::Pending;
    }

    const client_detail::WireNetworkIdState* wire_state = entity_store_->wire_state(wire_network_id);
    if (wire_state == nullptr) {
        reference.entity = ashiato::Entity{};
        return EntityReferenceStatus::Pending;
    }

    if (wire_state->version == 0U) {
        reference.entity = ashiato::Entity{};
        return EntityReferenceStatus::Pending;
    }

    if (wire_state->version != client_entity_network_id_version(client_entity_network_id)) {
        reference = EntityReference{};
        return EntityReferenceStatus::Destroyed;
    }

    reference.entity = ashiato::Entity{};
    return EntityReferenceStatus::Pending;
}

ClientEntityNetworkId ReplicationClient::client_entity_network_id_for_wire(std::uint32_t wire_network_id) {
    if (wire_network_id == 0U || wire_network_id > max_client_local_wire_network_id) {
        return invalid_client_entity_network_id;
    }
    const std::uint32_t version = entity_store_->ensure_wire_version(wire_network_id);
    const ClientId id = client_id_ == invalid_client_id ? 0U : client_id_;
    return make_client_entity_network_id(id, wire_network_id, version);
}

void ReplicationClient::advance_wire_network_id_version(std::uint32_t wire_network_id) {
    if (wire_network_id == 0U || wire_network_id > max_client_local_wire_network_id) {
        return;
    }
    entity_store_->advance_wire_version(wire_network_id);
}

}  // namespace ashiato::sync
