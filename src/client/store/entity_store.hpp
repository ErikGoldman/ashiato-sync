#pragma once

#include "client/state.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ashiato::sync::client_detail {

class ClientEntityStore {
public:
    struct DestroyTombstone {
        SyncFrame frame = 0;
        std::uint32_t generation = 0;
        std::uint64_t age_order = 0;
    };

    struct DestroyTombstoneAgeEntry {
        std::uint32_t wire_network_id = 0;
        std::uint32_t generation = 0;
        std::uint64_t age_order = 0;
    };

    enum class DestroyTombstoneRecordResult {
        Ignored,
        Inserted,
        Updated
    };

    std::size_t entity_count() const noexcept { return entities_.size(); }
    bool contains_entity_index(std::uint32_t entity_index) const noexcept { return entity_index < entities_.size(); }
    EntityState* state(std::uint32_t entity_index) noexcept {
        return contains_entity_index(entity_index) ? &entities_[entity_index] : nullptr;
    }
    const EntityState* state(std::uint32_t entity_index) const noexcept {
        return contains_entity_index(entity_index) ? &entities_[entity_index] : nullptr;
    }
    EntityState& state_unchecked(std::uint32_t entity_index) noexcept { return entities_[entity_index]; }
    const EntityState& state_unchecked(std::uint32_t entity_index) const noexcept { return entities_[entity_index]; }
    std::uint32_t index_of(const EntityState& state) const noexcept {
        return static_cast<std::uint32_t>(&state - entities_.data());
    }
    const std::vector<EntityState>& all_entity_states() const noexcept { return entities_; }

    EntityState* find_by_client_entity_network_id(ClientEntityNetworkId client_entity_network_id) noexcept;
    const EntityState* find_by_client_entity_network_id(
        ClientEntityNetworkId client_entity_network_id) const noexcept;
    std::uint32_t entity_index_for_client_entity_network_id(
        ClientEntityNetworkId client_entity_network_id) const noexcept;
    EntityState* find_by_wire_id(std::uint32_t wire_network_id) noexcept;
    const EntityState* find_by_wire_id(std::uint32_t wire_network_id) const noexcept;
    EntityState* ensure_entity_state(
        ClientEntityNetworkId client_entity_network_id,
        std::uint32_t wire_network_id);
    void release_entity_state(std::uint32_t entity_index);
    void unmap_entity_state(std::uint32_t entity_index);
    void sync_mode_memberships(std::uint32_t entity_index);

    EntityState* find_by_local_entity(ashiato::Entity local) noexcept;
    void register_local_entity_index(const EntityState& state);
    void unregister_local_entity_index(const EntityState& state);

    void set_active_membership(std::uint32_t entity_index, bool active);
    void set_buffered_membership(std::uint32_t entity_index, bool active);
    void set_snap_error_membership(std::uint32_t entity_index, bool active);
    void set_predicted_membership(std::uint32_t entity_index, bool active);
    void set_prediction_rollback_membership(std::uint32_t entity_index, bool active);
    void queue_prediction_rollback(std::uint32_t entity_index, SyncFrame frame);
    void clear_prediction_rollback_memberships();

    const std::vector<std::uint32_t>& active_entity_indices() const noexcept { return active_entities_; }
    const std::vector<std::uint32_t>& buffered_entity_indices() const noexcept { return buffered_entities_; }
    const std::vector<std::uint32_t>& snap_error_entity_indices() const noexcept { return snap_error_entities_; }
    const std::vector<std::uint32_t>& predicted_entity_indices() const noexcept { return predicted_entities_; }
    const std::vector<std::uint32_t>& prediction_rollback_entity_indices() const noexcept {
        return prediction_rollback_entities_;
    }

    const WireNetworkIdState* wire_state(std::uint32_t wire_network_id) const noexcept;
    std::uint32_t ensure_wire_version(std::uint32_t wire_network_id);
    void advance_wire_version(std::uint32_t wire_network_id);

    bool destroy_tombstone_blocks(std::uint32_t wire_network_id, SyncFrame frame) const;
    DestroyTombstoneRecordResult record_destroy_tombstone(std::uint32_t wire_network_id, SyncFrame frame);
    void erase_destroy_tombstone(std::uint32_t wire_network_id);

private:
    void compact_destroy_tombstone_ages();

    std::vector<EntityState> entities_;
    std::vector<std::uint32_t> free_entity_indices_;
    std::vector<WireNetworkIdState> wire_network_ids_;
    std::unordered_map<ClientEntityNetworkId, std::uint32_t> client_entity_indices_;
    std::unordered_map<std::uint64_t, std::uint32_t> local_entity_indices_;
    std::unordered_map<std::uint32_t, DestroyTombstone> destroy_tombstones_;
    std::vector<DestroyTombstoneAgeEntry> destroy_tombstone_ages_;
    std::size_t destroy_tombstone_age_begin_ = 0;
    std::uint64_t destroy_tombstone_next_age_order_ = 0;
    std::vector<std::uint32_t> active_entities_;
    std::vector<std::uint32_t> buffered_entities_;
    std::vector<std::uint32_t> snap_error_entities_;
    std::vector<std::uint32_t> predicted_entities_;
    std::vector<std::uint32_t> prediction_rollback_entities_;
};

}  // namespace ashiato::sync::client_detail
