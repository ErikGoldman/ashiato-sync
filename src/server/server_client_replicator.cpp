#include "kage/sync/server.hpp"

#include "server/detail.hpp"
#include "server/state.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace kage::sync {

server_detail::ServerClientReplicator::ServerClientReplicator()
    : update_scheduler(std::make_unique<UpdateScheduler>()) {}

server_detail::ServerClientReplicator::~ServerClientReplicator() = default;

void server_detail::ServerClientReplicator::accumulate_frame_delta(const ServerFrameDelta& frame) {
    const SyncSettings& settings = frame.registry.get<SyncSettings>();
    for (const SyncArchetype& archetype : settings.archetypes) {
        for (const SyncTagReplication& tag_replication : archetype.tags) {
            frame.dirty.each_dirty(tag_replication.tag, [&](ecs::Entity entity, const void*) {
                const std::uint32_t slot = frame.server.replicated_slot_for_entity(entity);
                if (slot != server_detail::invalid_quantized_frame_id) {
                    mark_dirty(frame.server, slot, frame.frame);
                }
            });
            frame.dirty.each_removed(tag_replication.tag, [&](ecs::Registry::ComponentRemoval removal) {
                const std::uint32_t slot = frame.server.replicated_slot_for_entity_index(removal.entity_index);
                if (slot != server_detail::invalid_quantized_frame_id) {
                    mark_dirty(frame.server, slot, frame.frame);
                }
            });
        }
    }

    for (const auto& component_ops : settings.component_ops) {
        const ecs::Entity component{component_ops.first};
        frame.dirty.each_dirty(component, [&](ecs::Entity entity, const void*) {
            const std::uint32_t slot = frame.server.replicated_slot_for_entity(entity);
            if (slot != server_detail::invalid_quantized_frame_id) {
                mark_dirty(frame.server, slot, frame.frame);
            }
        });
        frame.dirty.each_removed(component, [&](ecs::Registry::ComponentRemoval removal) {
            const std::uint32_t slot = frame.server.replicated_slot_for_entity_index(removal.entity_index);
            if (slot != server_detail::invalid_quantized_frame_id) {
                mark_dirty(frame.server, slot, frame.frame);
            }
        });
    }

    frame.dirty.each_dirty<NetworkOwner>([&](ecs::Entity entity, const void*) {
        const std::uint32_t slot = frame.server.replicated_slot_for_entity(entity);
        if (slot != server_detail::invalid_quantized_frame_id) {
            mark_dirty(frame.server, slot, frame.frame);
        }
    });
    frame.dirty.each_removed<NetworkOwner>([&](ecs::Registry::ComponentRemoval removal) {
        const std::uint32_t slot = frame.server.replicated_slot_for_entity_index(removal.entity_index);
        if (slot != server_detail::invalid_quantized_frame_id) {
            mark_dirty(frame.server, slot, frame.frame);
        }
    });
}

void server_detail::ServerClientReplicator::flush(const ServerFrameFlushContext& context) {
    (void)context.completed_frames;
    if (!context.server.prepare_client_update_send(*this)) {
        return;
    }
    const SyncSettings& settings = context.registry.get<SyncSettings>();
    update_scheduler->send_client(context.server, context.registry, settings, *this, context.completed_frames);
}

void server_detail::ServerClientReplicator::EntityStates::ensure_capacity(std::size_t size) {
    if (states.size() < size) {
        states.resize(size);
    }
}

std::size_t server_detail::ServerClientReplicator::EntityStates::size() const noexcept {
    return states.size();
}

server_detail::ClientEntityState*
server_detail::ServerClientReplicator::EntityStates::try_get(std::uint32_t slot) noexcept {
    return slot < states.size() ? &states[slot] : nullptr;
}

const server_detail::ClientEntityState*
server_detail::ServerClientReplicator::EntityStates::try_get(std::uint32_t slot) const noexcept {
    return slot < states.size() ? &states[slot] : nullptr;
}

void server_detail::ServerClientReplicator::EntityStates::clear(ReplicationServer& server, std::uint32_t slot) {
    ClientEntityState* state = try_get(slot);
    if (state == nullptr) {
        return;
    }
    server.clear_server_client_entity_state(*state);
}

void server_detail::ServerClientReplicator::EntityStates::clear_all(ReplicationServer& server) {
    for (ClientEntityState& state : states) {
        server.clear_server_client_entity_state(state);
    }
}

void server_detail::ServerClientReplicator::EntityStates::clear_preserving_network_identity(
    ReplicationServer& server,
    std::uint32_t slot) {
    ClientEntityState* state = try_get(slot);
    if (state == nullptr) {
        return;
    }
    const std::uint32_t network_id = state->network_id;
    const std::uint32_t network_version = state->network_version;
    const bool has_network_id = state->has_network_id;
    server.clear_server_client_entity_state(*state);
    state->network_id = network_id;
    state->network_version = network_version;
    state->has_network_id = has_network_id;
}

void server_detail::ServerClientReplicator::EntityStates::expire_pending_cues(SyncFrame frame) {
    for (ClientEntityState& state : states) {
        state.pending_cues.erase(
            std::remove_if(
                state.pending_cues.begin(),
                state.pending_cues.end(),
                [frame](const ClientEntityState::PendingCue& cue) {
                    return frame > cue.expire_frame;
                }),
            state.pending_cues.end());
    }
}

void server_detail::ServerClientReplicator::DestroyQueue::enqueue(
    ecs::Entity entity,
    SyncFrame frame,
    std::uint32_t network_id,
    std::uint32_t network_version) {
    const auto found = std::find_if(
        pending.begin(),
        pending.end(),
        [entity, network_id](const ClientDestroyState& destroy) {
            return destroy.entity == entity && destroy.network_id == network_id;
        });
    if (found == pending.end()) {
        pending.push_back(ClientDestroyState{entity, frame, 0, network_id, network_version});
    }
}

bool server_detail::ServerClientReplicator::DestroyQueue::empty() const noexcept {
    return pending.empty();
}

std::size_t server_detail::ServerClientReplicator::DestroyQueue::size() const noexcept {
    return pending.size();
}

server_detail::ClientDestroyState&
server_detail::ServerClientReplicator::DestroyQueue::at(std::size_t index) noexcept {
    return pending[index];
}

const server_detail::ClientDestroyState&
server_detail::ServerClientReplicator::DestroyQueue::at(std::size_t index) const noexcept {
    return pending[index];
}

bool server_detail::ServerClientReplicator::DestroyQueue::acknowledge(
    ServerClientReplicator& client,
    ecs::Entity entity,
    SyncFrame frame) {
    const auto found = std::find_if(
        pending.begin(),
        pending.end(),
        [entity, frame](const ClientDestroyState& destroy) {
            return destroy.entity == entity && frame <= destroy.frame;
        });
    if (found == pending.end()) {
        return false;
    }

    const std::uint32_t network_id = found->network_id;
    pending.erase(found);
    client.free_network_id(network_id);
    return true;
}

bool server_detail::ServerClientReplicator::DestroyQueue::contains_ack_record(
    ecs::Entity entity,
    SyncFrame frame) const {
    return std::any_of(
        pending.begin(),
        pending.end(),
        [entity, frame](const ClientDestroyState& destroy) {
            return destroy.entity == entity && frame <= destroy.frame;
        });
}

std::uint32_t server_detail::ServerClientReplicator::NetworkIds::network_id_for(
    ReplicationServer& server,
    ServerClientReplicator& client,
    std::uint32_t slot) {
    ClientEntityState* state = client.entities.try_get(slot);
    if (state == nullptr) {
        return 0U;
    }
    if (state->has_network_id) {
        return state->network_id;
    }
    return allocate_for(server, client, slot);
}

std::uint32_t server_detail::ServerClientReplicator::NetworkIds::allocate_for(
    ReplicationServer& server,
    ServerClientReplicator& client,
    std::uint32_t slot) {
    (void)server;
    ClientEntityState* state = client.entities.try_get(slot);
    if (state == nullptr) {
        return 0U;
    }
    if (entries.empty()) {
        entries.push_back(Entry{});
    }
    std::uint32_t network_id = 0;
    if (free_network_id != 0U) {
        network_id = free_network_id;
        Entry& entry = entries[network_id];
        free_network_id = entry.replicated_index_or_next_free_network_id;
        entry.replicated_index_or_next_free_network_id = slot;
        entry.active = true;
        entry.pending_destroy = false;
    } else {
        if (entries.size() > max_client_local_wire_network_id) {
            throw std::length_error("kage sync client-local network id space exhausted");
        }
        network_id = static_cast<std::uint32_t>(entries.size());
        entries.push_back(Entry{slot, 1U, true, false});
    }

    state->network_id = network_id;
    state->network_version = entries[network_id].version;
    state->has_network_id = true;
#ifdef KAGE_SYNC_ENABLE_TRACING
    server.trace_entity_started_syncing(client.id, slot, network_id, state->network_version);
#endif
    return network_id;
}

void server_detail::ServerClientReplicator::NetworkIds::free(std::uint32_t network_id) {
    Entry* entry = try_get(network_id);
    if (entry == nullptr || !entry->pending_destroy) {
        return;
    }
    ++entry->version;
    if (entry->version == 0U) {
        entry->version = 1U;
    }
    entry->replicated_index_or_next_free_network_id = free_network_id;
    entry->active = false;
    entry->pending_destroy = false;
    free_network_id = network_id;
}

bool server_detail::ServerClientReplicator::NetworkIds::mark_pending_destroy(std::uint32_t network_id) {
    Entry* entry = try_get(network_id);
    if (entry == nullptr) {
        return false;
    }
    entry->active = false;
    entry->pending_destroy = true;
    entry->replicated_index_or_next_free_network_id =
        server_detail::invalid_replicated_index_or_free_network_id;
    return true;
}

server_detail::ServerClientReplicator::NetworkIds::Entry*
server_detail::ServerClientReplicator::NetworkIds::try_get(std::uint32_t network_id) noexcept {
    return network_id != 0U && network_id < entries.size() ? &entries[network_id] : nullptr;
}

const server_detail::ServerClientReplicator::NetworkIds::Entry*
server_detail::ServerClientReplicator::NetworkIds::try_get(std::uint32_t network_id) const noexcept {
    return network_id != 0U && network_id < entries.size() ? &entries[network_id] : nullptr;
}

void server_detail::ServerClientReplicator::ensure_capacity(std::size_t replicated_count) {
    entities.ensure_capacity(replicated_count);
    if (dirty_queue.entries.size() < replicated_count) {
        dirty_queue.entries.resize(replicated_count);
    }
}

void server_detail::ServerClientReplicator::initialize_marking_all_dirty(
    ReplicationServer& server,
    SyncFrame frame) {
    ensure_capacity(server.replicated_slot_count());
    dirty_queue.dirty_replicated_indices.reserve(server.active_replicated_slot_count());
    for (std::uint32_t replicated_index = 0; replicated_index < server.replicated_slot_count(); ++replicated_index) {
        if (server.replicated_slot_active(replicated_index)) {
            mark_dirty(server, replicated_index, frame);
        }
    }
}

void server_detail::ServerClientReplicator::mark_dirty(
    const ReplicationServer& server,
    std::uint32_t replicated_index,
    SyncFrame frame) {
    if (replicated_index >= server.replicated_slot_count()) {
        return;
    }
    ensure_capacity(server.replicated_slot_count());
    ClientDirtyQueue::Entry& entry = dirty_queue.entries[replicated_index];
    if (!entry.queued) {
        entry.queued = true;
        entry.priority_accumulator = 0.0f;
        entry.component_mask = std::numeric_limits<std::uint64_t>::max();
        entry.last_priority = std::numeric_limits<float>::quiet_NaN();
        if (const ClientEntityState* state = entities.try_get(replicated_index)) {
            entry.last_priority = state->last_priority;
            if (state->baseline != server_detail::invalid_quantized_frame_id &&
                server.quantized_frame_active(state->baseline)) {
                entry.baseline_frame = server.quantized_frame_frame(state->baseline);
            }
        }
    }
    if (!entry.listed) {
        dirty_queue.dirty_replicated_indices.push_back(replicated_index);
        entry.listed = true;
    }
    entry.dirty_frame = std::max(entry.dirty_frame, frame);
    if (entry.dirty_frame <= entry.baseline_frame) {
        entry.dirty_frame = entry.baseline_frame + 1U;
    }
}

void server_detail::ServerClientReplicator::clear_dirty(std::uint32_t replicated_index) {
    if (replicated_index >= dirty_queue.entries.size()) {
        return;
    }
    ClientDirtyQueue::Entry& entry = dirty_queue.entries[replicated_index];
    const bool listed = entry.listed;
    entry = ClientDirtyQueue::Entry{};
    entry.listed = listed;
}

void server_detail::ServerClientReplicator::expire_pending_cues(SyncFrame frame) {
    entities.expire_pending_cues(frame);
}

std::uint32_t server_detail::ServerClientReplicator::network_id_for(
    ReplicationServer& server,
    std::uint32_t replicated_index) {
    return network_ids.network_id_for(server, *this, replicated_index);
}

void server_detail::ServerClientReplicator::free_network_id(std::uint32_t network_id) {
    network_ids.free(network_id);
}

bool server_detail::ServerClientReplicator::enqueue_destroy(
    ReplicationServer& server,
    std::uint32_t replicated_index,
    ecs::Entity entity,
    SyncFrame frame) {
    std::uint32_t network_id = 0;
    std::uint32_t network_version = 0;
    if (ClientEntityState* state = entities.try_get(replicated_index)) {
        network_id = state->network_id;
        network_version = state->network_version;
        entities.clear(server, replicated_index);
    }
    clear_dirty(replicated_index);
    if (!network_ids.mark_pending_destroy(network_id)) {
        return false;
    }
    destroys.enqueue(entity, frame, network_id, network_version);
    return true;
}

bool server_detail::ServerClientReplicator::acknowledge_entity(
    ReplicationServer& server,
    std::uint32_t replicated_index,
    SyncFrame frame) {
    ClientEntityState* entity_state = entities.try_get(replicated_index);
    if (entity_state == nullptr) {
        return false;
    }

    const auto found_pending = std::find_if(
        entity_state->pending.begin(),
        entity_state->pending.end(),
        [frame](const ClientEntityState::PendingQuantizedFrame& pending) {
            return pending.frame == frame;
        });
    if (found_pending == entity_state->pending.end()) {
        return false;
    }

    const std::uint32_t acked_quantized_frame = found_pending->quantized_frame;
    if (acked_quantized_frame == server_detail::invalid_quantized_frame_id ||
        !server.quantized_frame_active(acked_quantized_frame)) {
        return false;
    }

    if (entity_state->baseline != acked_quantized_frame) {
        server.release_server_quantized_frame(entity_state->baseline);
        entity_state->baseline = acked_quantized_frame;
        server.retain_server_quantized_frame(entity_state->baseline);
    }
    if (replicated_index < dirty_queue.entries.size()) {
        ClientDirtyQueue::Entry& entry = dirty_queue.entries[replicated_index];
        entry.baseline_frame = frame;
        if (entry.baseline_frame >= entry.dirty_frame) {
            entry.queued = false;
            entry.priority_accumulator = 0.0f;
        }
    }

    for (auto pending = entity_state->pending.begin(); pending != entity_state->pending.end();) {
        if (pending->frame <= frame) {
            server.release_server_quantized_frame(pending->quantized_frame);
            pending = entity_state->pending.erase(pending);
        } else {
            ++pending;
        }
    }
    server.acknowledge_server_cues(*entity_state, frame);
    return true;
}

}  // namespace kage::sync
