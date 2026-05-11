#include "kage/sync/client.hpp"

#include "client/store/cue_store.hpp"
#include "client/store/entity_store.hpp"
#include "client/store/frame_ring_store.hpp"
#include "client/state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace kage::sync {
namespace {

constexpr std::size_t invalid_membership_index = std::numeric_limits<std::size_t>::max();
constexpr std::uint32_t invalid_entity_index = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t max_destroy_tombstones = 65536;

using DestroyTombstoneAgeEntry = client_detail::ClientEntityStore::DestroyTombstoneAgeEntry;
using client_detail::WireNetworkIdState;

template <typename IndexAccessor>
void set_indexed_membership(
    std::vector<client_detail::EntityState>& entities,
    std::uint32_t entity_index,
    bool active,
    std::vector<std::uint32_t>& members,
    IndexAccessor index_accessor) {
    if (entity_index >= entities.size()) {
        return;
    }

    client_detail::EntityState& state = entities[entity_index];
    std::size_t& index = index_accessor(state);
    if (active) {
        if (index == invalid_membership_index) {
            index = members.size();
            members.push_back(entity_index);
        }
        return;
    }

    if (index == invalid_membership_index || index >= members.size()) {
        index = invalid_membership_index;
        return;
    }

    const std::uint32_t moved = members.back();
    members[index] = moved;
    index_accessor(entities[moved]) = index;
    members.pop_back();
    index = invalid_membership_index;
}

}  // namespace

namespace client_detail {

EntityState* ClientEntityStore::find_by_client_entity_network_id(
    ClientEntityNetworkId client_entity_network_id) noexcept {
    const auto found = client_entity_indices_.find(client_entity_network_id);
    if (found == client_entity_indices_.end() || found->second >= entities_.size()) {
        return nullptr;
    }
    EntityState& state = entities_[found->second];
    return state.identity.client_entity_network_id == client_entity_network_id ? &state : nullptr;
}

const EntityState* ClientEntityStore::find_by_client_entity_network_id(
    ClientEntityNetworkId client_entity_network_id) const noexcept {
    const auto found = client_entity_indices_.find(client_entity_network_id);
    if (found == client_entity_indices_.end() || found->second >= entities_.size()) {
        return nullptr;
    }
    const EntityState& state = entities_[found->second];
    return state.identity.client_entity_network_id == client_entity_network_id ? &state : nullptr;
}

std::uint32_t ClientEntityStore::entity_index_for_client_entity_network_id(
    ClientEntityNetworkId client_entity_network_id) const noexcept {
    const auto found = client_entity_indices_.find(client_entity_network_id);
    return found == client_entity_indices_.end() ? invalid_entity_index : found->second;
}

EntityState* ClientEntityStore::find_by_wire_id(std::uint32_t wire_network_id) noexcept {
    if (wire_network_id == 0U || wire_network_id >= wire_network_ids_.size()) {
        return nullptr;
    }
    const std::uint32_t entity_index = wire_network_ids_[wire_network_id].entity_index;
    if (entity_index >= entities_.size()) {
        return nullptr;
    }
    EntityState& state = entities_[entity_index];
    return state.identity.wire_network_id == wire_network_id ? &state : nullptr;
}

const EntityState* ClientEntityStore::find_by_wire_id(std::uint32_t wire_network_id) const noexcept {
    if (wire_network_id == 0U || wire_network_id >= wire_network_ids_.size()) {
        return nullptr;
    }
    const std::uint32_t entity_index = wire_network_ids_[wire_network_id].entity_index;
    if (entity_index >= entities_.size()) {
        return nullptr;
    }
    const EntityState& state = entities_[entity_index];
    return state.identity.wire_network_id == wire_network_id ? &state : nullptr;
}

EntityState* ClientEntityStore::ensure_entity_state(
    ClientEntityNetworkId client_entity_network_id,
    std::uint32_t wire_network_id) {
    if (client_entity_network_id == invalid_client_entity_network_id || wire_network_id == 0U) {
        return nullptr;
    }

    if (EntityState* existing = find_by_client_entity_network_id(client_entity_network_id)) {
        return existing;
    }

    std::uint32_t entity_index = invalid_entity_index;
    if (!free_entity_indices_.empty()) {
        entity_index = free_entity_indices_.back();
        free_entity_indices_.pop_back();
    } else {
        if (entities_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::length_error("kage sync client entity state space exhausted");
        }
        entity_index = static_cast<std::uint32_t>(entities_.size());
        entities_.emplace_back();
    }

    EntityState& fresh = entities_[entity_index];
    fresh = EntityState{};
    fresh.identity.client_entity_network_id = client_entity_network_id;
    fresh.identity.wire_network_id = wire_network_id;
    set_active_membership(entity_index, true);
    client_entity_indices_[client_entity_network_id] = entity_index;
    if (wire_network_id >= wire_network_ids_.size()) {
        wire_network_ids_.resize(static_cast<std::size_t>(wire_network_id) + 1U);
    }
    wire_network_ids_[wire_network_id].entity_index = entity_index;
    wire_network_ids_[wire_network_id].alive = true;
    return &fresh;
}

void ClientEntityStore::unmap_entity_state(std::uint32_t entity_index) {
    if (entity_index >= entities_.size()) {
        return;
    }

    EntityState& state = entities_[entity_index];
    const auto found = client_entity_indices_.find(state.identity.client_entity_network_id);
    if (found != client_entity_indices_.end() && found->second == entity_index) {
        client_entity_indices_.erase(found);
    }
    if (state.identity.wire_network_id < wire_network_ids_.size() &&
        wire_network_ids_[state.identity.wire_network_id].entity_index == entity_index) {
        wire_network_ids_[state.identity.wire_network_id].entity_index = invalid_entity_index;
        wire_network_ids_[state.identity.wire_network_id].alive = false;
    }
}

void ClientEntityStore::release_entity_state(std::uint32_t entity_index) {
    if (entity_index >= entities_.size()) {
        return;
    }

    set_buffered_membership(entity_index, false);
    set_snap_error_membership(entity_index, false);
    set_predicted_membership(entity_index, false);
    set_prediction_rollback_membership(entity_index, false);
    set_active_membership(entity_index, false);
    entities_[entity_index] = EntityState{};
    free_entity_indices_.push_back(entity_index);
}

void ClientEntityStore::sync_mode_memberships(std::uint32_t entity_index) {
    if (entity_index >= entities_.size()) {
        return;
    }
    const EntityState& state = entities_[entity_index];
    if (state.identity.client_entity_network_id == invalid_client_entity_network_id) {
        return;
    }
    const auto found = client_entity_indices_.find(state.identity.client_entity_network_id);
    if (found == client_entity_indices_.end() || found->second != entity_index) {
        return;
    }
    set_buffered_membership(entity_index, state.mode.current == ReplicationClientMode::BufferedInterpolation);
    set_predicted_membership(entity_index, state.mode.current == ReplicationClientMode::Predict);
    set_snap_error_membership(
        entity_index,
        (state.mode.current == ReplicationClientMode::Snap || state.mode.current == ReplicationClientMode::Predict) &&
            !state.visual.snap_errors.empty());
}

#ifdef KAGE_SYNC_ENABLE_TRACING
EntityState* ClientEntityStore::find_by_local_entity(ecs::Entity local) noexcept {
    const auto found = local_entity_indices_.find(local.value);
    if (found == local_entity_indices_.end() || found->second >= entities_.size()) {
        return nullptr;
    }
    EntityState& state = entities_[found->second];
    if (state.identity.local == local) {
        return &state;
    }
    local_entity_indices_.erase(found);
    return nullptr;
}

void ClientEntityStore::register_local_entity_index(const EntityState& state) {
    if (state.identity.local) {
        local_entity_indices_[state.identity.local.value] = index_of(state);
    }
}

void ClientEntityStore::unregister_local_entity_index(const EntityState& state) {
    if (!state.identity.local) {
        return;
    }
    const auto found = local_entity_indices_.find(state.identity.local.value);
    if (found != local_entity_indices_.end() && found->second == index_of(state)) {
        local_entity_indices_.erase(found);
    }
}
#else
EntityState* ClientEntityStore::find_by_local_entity(ecs::Entity local) noexcept {
    for (EntityState& state : entities_) {
        if (state.identity.local == local) {
            return &state;
        }
    }
    return nullptr;
}
#endif

void ClientEntityStore::set_active_membership(std::uint32_t entity_index, bool active) {
    set_indexed_membership(
        entities_,
        entity_index,
        active,
        active_entities_,
        [](EntityState& state) -> std::size_t& { return state.memberships.active_index; });
}

void ClientEntityStore::set_buffered_membership(std::uint32_t entity_index, bool active) {
    set_indexed_membership(
        entities_,
        entity_index,
        active,
        buffered_entities_,
        [](EntityState& state) -> std::size_t& { return state.memberships.buffered_index; });
}

void ClientEntityStore::set_snap_error_membership(std::uint32_t entity_index, bool active) {
    set_indexed_membership(
        entities_,
        entity_index,
        active,
        snap_error_entities_,
        [](EntityState& state) -> std::size_t& { return state.memberships.snap_error_index; });
}

void ClientEntityStore::set_predicted_membership(std::uint32_t entity_index, bool active) {
    set_indexed_membership(
        entities_,
        entity_index,
        active,
        predicted_entities_,
        [](EntityState& state) -> std::size_t& { return state.memberships.predicted_index; });
}

void ClientEntityStore::set_prediction_rollback_membership(std::uint32_t entity_index, bool active) {
    set_indexed_membership(
        entities_,
        entity_index,
        active,
        prediction_rollback_entities_,
        [](EntityState& state) -> std::size_t& { return state.memberships.prediction_rollback_index; });
}

void ClientEntityStore::queue_prediction_rollback(std::uint32_t entity_index, SyncFrame frame) {
    if (entity_index >= entities_.size()) {
        return;
    }
    EntityState& state = entities_[entity_index];
    if (!state.prediction.rollback_pending) {
        set_prediction_rollback_membership(entity_index, true);
    }
    state.prediction.rollback_pending = true;
    state.prediction.rollback_frame = state.prediction.rollback_frame == 0
        ? frame
        : std::max(state.prediction.rollback_frame, frame);
}

void ClientEntityStore::clear_prediction_rollback_memberships() {
    for (const std::uint32_t entity_index : prediction_rollback_entities_) {
        if (entity_index >= entities_.size()) {
            continue;
        }
        EntityState& state = entities_[entity_index];
        state.prediction.rollback_pending = false;
        state.prediction.rollback_frame = 0;
        state.memberships.prediction_rollback_index = invalid_membership_index;
    }
    prediction_rollback_entities_.clear();
}

const WireNetworkIdState* ClientEntityStore::wire_state(std::uint32_t wire_network_id) const noexcept {
    return wire_network_id < wire_network_ids_.size() ? &wire_network_ids_[wire_network_id] : nullptr;
}

std::uint32_t ClientEntityStore::ensure_wire_version(std::uint32_t wire_network_id) {
    if (wire_network_id >= wire_network_ids_.size()) {
        wire_network_ids_.resize(static_cast<std::size_t>(wire_network_id) + 1U);
    }
    WireNetworkIdState& state = wire_network_ids_[wire_network_id];
    if (state.version == 0U) {
        state.version = 1U;
    }
    return state.version;
}

void ClientEntityStore::advance_wire_version(std::uint32_t wire_network_id) {
    if (wire_network_id >= wire_network_ids_.size()) {
        wire_network_ids_.resize(static_cast<std::size_t>(wire_network_id) + 1U);
    }
    WireNetworkIdState& state = wire_network_ids_[wire_network_id];
    if (state.version == 0U) {
        state.version = 1U;
    } else {
        ++state.version;
        if (state.version == 0U) {
            state.version = 1U;
        }
    }
    state.entity_index = invalid_entity_index;
    state.alive = false;
}

bool ClientEntityStore::destroy_tombstone_blocks(std::uint32_t wire_network_id, SyncFrame frame) const {
    const auto found = destroy_tombstones_.find(wire_network_id);
    return found != destroy_tombstones_.end() && frame <= found->second.frame;
}

ClientEntityStore::DestroyTombstoneRecordResult ClientEntityStore::record_destroy_tombstone(
    std::uint32_t wire_network_id,
    SyncFrame frame) {
    if (wire_network_id == 0U) {
        return DestroyTombstoneRecordResult::Ignored;
    }

    auto [found, inserted] = destroy_tombstones_.try_emplace(wire_network_id);
    if (!inserted && frame <= found->second.frame) {
        return DestroyTombstoneRecordResult::Ignored;
    }

    if (inserted) {
        found->second.frame = frame;
        found->second.age_order = destroy_tombstone_next_age_order_++;
    } else {
        found->second.frame = frame;
        ++found->second.generation;
        found->second.age_order = destroy_tombstone_next_age_order_++;
    }
    destroy_tombstone_ages_.push_back(
        DestroyTombstoneAgeEntry{wire_network_id, found->second.generation, found->second.age_order});
    compact_destroy_tombstone_ages();
    if (destroy_tombstones_.size() > max_destroy_tombstones) {
        while (destroy_tombstones_.size() > max_destroy_tombstones &&
               destroy_tombstone_age_begin_ < destroy_tombstone_ages_.size()) {
            const DestroyTombstoneAgeEntry age = destroy_tombstone_ages_[destroy_tombstone_age_begin_++];
            const auto erase = destroy_tombstones_.find(age.wire_network_id);
            if (erase == destroy_tombstones_.end() ||
                erase->second.generation != age.generation ||
                erase->second.age_order != age.age_order) {
                continue;
            }
            destroy_tombstones_.erase(erase);
        }
        compact_destroy_tombstone_ages();
    }
    return inserted ? DestroyTombstoneRecordResult::Inserted : DestroyTombstoneRecordResult::Updated;
}

void ClientEntityStore::erase_destroy_tombstone(std::uint32_t wire_network_id) {
    destroy_tombstones_.erase(wire_network_id);
}

void ClientEntityStore::compact_destroy_tombstone_ages() {
    if (destroy_tombstone_age_begin_ == destroy_tombstone_ages_.size()) {
        destroy_tombstone_ages_.clear();
        destroy_tombstone_age_begin_ = 0;
        return;
    }
    if (destroy_tombstone_age_begin_ != 0U &&
        destroy_tombstone_age_begin_ >= 4096U &&
        destroy_tombstone_age_begin_ * 2U >= destroy_tombstone_ages_.size()) {
        destroy_tombstone_ages_.erase(
            destroy_tombstone_ages_.begin(),
            destroy_tombstone_ages_.begin() + static_cast<std::ptrdiff_t>(destroy_tombstone_age_begin_));
        destroy_tombstone_age_begin_ = 0;
    }
    if (destroy_tombstone_ages_.size() <= max_destroy_tombstones * 2U) {
        return;
    }
    destroy_tombstone_ages_.clear();
    destroy_tombstone_ages_.reserve(destroy_tombstones_.size());
    for (const auto& tombstone : destroy_tombstones_) {
        destroy_tombstone_ages_.push_back(DestroyTombstoneAgeEntry{
            tombstone.first,
            tombstone.second.generation,
            tombstone.second.age_order});
    }
    std::sort(
        destroy_tombstone_ages_.begin(),
        destroy_tombstone_ages_.end(),
        [](const DestroyTombstoneAgeEntry& lhs, const DestroyTombstoneAgeEntry& rhs) {
            return lhs.age_order < rhs.age_order;
        });
    destroy_tombstone_age_begin_ = 0;
}

}  // namespace client_detail

}  // namespace kage::sync
