#include "kage/sync/server.hpp"

#include "kage/sync/protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace kage::sync {
namespace {

constexpr std::size_t destroy_record_bits = 1U + 64U;
constexpr std::size_t max_pending_snapshots_per_entity = 64;

}  // namespace

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
    state.entity_states.resize(replicated_.size());
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
    for (ClientEntityState& entity_state : clients_[index].entity_states) {
        clear_client_entity_state(entity_state);
    }

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

void ReplicationServer::refresh_replicated(ecs::Registry& registry) {
    register_components(registry);

    if (!replicated_initialized_) {
        registry.view<const Replicated>().each([&](ecs::Entity entity, const Replicated& replicated) {
            upsert_replicated(registry, entity, replicated.archetype);
        });
        replicated_initialized_ = true;
        registry.clear_all_dirty<Replicated>();
        return;
    }

    registry.each_removed<Replicated>([&](ecs::Registry::ComponentRemoval removal) {
        deactivate_entity_index(removal.entity_index);
    });

    registry.each_dirty<Replicated>([&](ecs::Entity entity, const void* value) {
        upsert_replicated(registry, entity, static_cast<const Replicated*>(value)->archetype);
    });
    registry.clear_all_dirty<Replicated>();
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

bool ReplicationServer::acknowledge_entity(ClientId client, ecs::Entity entity, SyncFrame frame) {
    const auto client_found = client_to_index_.find(client);
    const auto slot_found = entity_to_slot_.find(entity.value);
    if (client_found == client_to_index_.end() || slot_found == entity_to_slot_.end()) {
        return false;
    }

    ClientState& state = clients_[client_found->second];
    const std::uint32_t slot = slot_found->second;
    if (slot >= state.entity_states.size()) {
        return false;
    }

    ClientEntityState& entity_state = state.entity_states[slot];
    const auto found_pending = std::find_if(
        entity_state.pending.begin(),
        entity_state.pending.end(),
        [frame](const ClientEntityState::PendingSnapshot& pending) {
            return pending.frame == frame;
        });
    if (found_pending == entity_state.pending.end()) {
        return false;
    }

    const std::uint32_t acked_snapshot = found_pending->snapshot;
    if (acked_snapshot == invalid_snapshot_id || acked_snapshot >= snapshots_.size() || !snapshots_[acked_snapshot].active) {
        return false;
    }

    if (entity_state.baseline != acked_snapshot) {
        release_snapshot(entity_state.baseline);
        entity_state.baseline = acked_snapshot;
        retain_snapshot(entity_state.baseline);
    }

    for (auto pending = entity_state.pending.begin(); pending != entity_state.pending.end();) {
        if (pending->frame <= frame) {
            release_snapshot(pending->snapshot);
            pending = entity_state.pending.erase(pending);
        } else {
            ++pending;
        }
    }
    return true;
}

bool ReplicationServer::process_packet(ClientId client, BitBuffer packet) {
    const auto client_found = client_to_index_.find(client);
    if (client_found == client_to_index_.end()) {
        return false;
    }

    try {
        if (packet.remaining_bits() < 24U) {
            return false;
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message != protocol::client_ack_message) {
            return false;
        }

        ClientState& state = clients_[client_found->second];
        const auto ack_count = static_cast<std::uint16_t>(packet.read_bits(16U));
        bool all_valid = true;
        for (std::uint16_t ack = 0; ack < ack_count; ++ack) {
            const bool destroy = packet.read_bool();
            const auto frame = static_cast<SyncFrame>(packet.read_bits(32U));
            const ecs::Entity entity{packet.read_unsigned_bits(64U)};
            const bool accepted = destroy
                ? acknowledge_destroy(state, entity, frame)
                : acknowledge_entity(client, entity, frame);
            all_valid = all_valid && accepted;
        }
        return all_valid;
    } catch (const std::exception&) {
        return false;
    }
}

std::size_t ReplicationServer::retained_snapshot_count() const noexcept {
    std::size_t count = 0;
    for (const QuantizedSnapshot& snapshot : snapshots_) {
        if (snapshot.active && snapshot.ref_count != 0) {
            ++count;
        }
    }
    return count;
}

std::size_t ReplicationServer::retained_snapshot_bytes() const noexcept {
    std::size_t bytes = 0;
    for (const QuantizedSnapshot& snapshot : snapshots_) {
        if (!snapshot.active || snapshot.ref_count == 0) {
            continue;
        }
        for (const QuantizedBaseline& baseline : snapshot.baselines) {
            bytes += baseline.bytes.size();
        }
    }
    return bytes;
}

void ReplicationServer::tick(ecs::Registry& registry) {
    if (options_.transport) {
        tick_serialized(registry);
        return;
    }

    tick(registry, ReplicateFn{});
}

void ReplicationServer::tick(ecs::Registry& registry, const ReplicateFn& replicate) {
    refresh_replicated(registry);

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
    refresh_replicated(registry);

    const SyncSettings& settings = registry.get<SyncSettings>();
    std::vector<SerializedCandidate> candidates;
    std::vector<std::uint32_t> sent;
    std::vector<std::uint32_t> unsent;
    BitBuffer records;
    SerializedEntity serialized;
    std::vector<QuantizedBaseline> snapshot_scratch;

    candidates.reserve(active_replicated_count_);
    sent.reserve(active_replicated_count_);
    unsent.reserve(active_replicated_count_);
    ++frame_;

    for (ClientState& client : clients_) {
        ++client.epoch;

        std::size_t remaining = options_.bandwidth_limit_bytes_per_tick;
        std::uint16_t packet_entities = 0;
        candidates.clear();
        candidates.reserve(client.order.size() + client.pending_destroys.size());
        sent.clear();
        unsent.clear();
        unsent.reserve(client.order.size());
        records.clear();
        records.reserve_bytes(options_.mtu_bytes);

        for (const std::uint32_t slot : client.order) {
            if (!slot_is_replicable(registry, slot)) {
                if (slot < replicated_.size() && replicated_[slot].active) {
                    deactivate_slot(slot);
                }
                continue;
            }
            const std::uint64_t priority = slot < client.reset_epochs.size()
                ? client.epoch - client.reset_epochs[slot]
                : 0;
            candidates.push_back(SerializedCandidate{SerializedCandidate::Kind::Update, slot, 0, priority});
        }
        for (std::size_t index = 0; index < client.pending_destroys.size(); ++index) {
            const ClientDestroyState& destroy = client.pending_destroys[index];
            candidates.push_back(SerializedCandidate{
                SerializedCandidate::Kind::Destroy,
                0,
                index,
                client.epoch - destroy.reset_epoch});
        }
        std::stable_sort(candidates.begin(), candidates.end(), candidate_before);

        for (const SerializedCandidate& candidate : candidates) {
            if (candidate.kind == SerializedCandidate::Kind::Destroy) {
                if (candidate.destroy_index >= client.pending_destroys.size()) {
                    continue;
                }
                ClientDestroyState& destroy = client.pending_destroys[candidate.destroy_index];
                const std::size_t next_packet_bits =
                    protocol::server_update_header_bits + records.bit_size() + destroy_record_bits;
                if (!records.empty() && protocol::bytes_for_bits(next_packet_bits) > options_.mtu_bytes) {
                    const std::size_t packet_bytes =
                        protocol::bytes_for_bits(protocol::server_update_header_bits + records.bit_size());
                    if (packet_bytes > remaining) {
                        break;
                    }
                    send_packet(client.id, frame_, packet_entities, records);
                    remaining -= packet_bytes;
                    records.clear();
                    packet_entities = 0;
                }

                const std::size_t packet_bytes = protocol::bytes_for_bits(
                    protocol::server_update_header_bits + records.bit_size() + destroy_record_bits);
                if (packet_bytes > options_.mtu_bytes || packet_bytes > remaining) {
                    continue;
                }

                destroy.frame = frame_;
                destroy.reset_epoch = client.epoch;
                records.push_bool(true);
                records.push_unsigned_bits(destroy.entity.value, 64U);
                ++packet_entities;
                continue;
            }

            const std::uint32_t slot = candidate.slot;
            if (!slot_is_replicable(registry, slot)) {
                continue;
            }

            serialized.payload.clear();
            serialized.snapshot = invalid_snapshot_id;
            if (!serialize_entity(registry, settings, client, slot, frame_, snapshot_scratch, serialized)) {
                unsent.push_back(slot);
                continue;
            }

            const std::size_t next_packet_bits =
                protocol::server_update_header_bits + records.bit_size() + 1U + serialized.payload.bit_size();
            if (!records.empty() && protocol::bytes_for_bits(next_packet_bits) > options_.mtu_bytes) {
                const std::size_t packet_bytes =
                    protocol::bytes_for_bits(protocol::server_update_header_bits + records.bit_size());
                if (packet_bytes > remaining) {
                    release_snapshot(serialized.snapshot);
                    unsent.push_back(slot);
                    continue;
                }
                send_packet(client.id, frame_, packet_entities, records);
                remaining -= packet_bytes;
                records.clear();
                packet_entities = 0;
            }

            const std::size_t single_packet_bits =
                protocol::server_update_header_bits + records.bit_size() + 1U + serialized.payload.bit_size();
            const std::size_t packet_bytes = protocol::bytes_for_bits(single_packet_bits);
            if (packet_bytes > options_.mtu_bytes || packet_bytes > remaining) {
                release_snapshot(serialized.snapshot);
                unsent.push_back(slot);
                continue;
            }

            records.push_bool(false);
            records.push_buffer_bits(serialized.payload);
            ++packet_entities;
            client.reset_epochs[slot] = client.epoch;
            ClientEntityState& entity_state = client.entity_states[slot];
            entity_state.pending.push_back(ClientEntityState::PendingSnapshot{serialized.snapshot, frame_});
            retain_snapshot(serialized.snapshot);
            while (entity_state.pending.size() > max_pending_snapshots_per_entity) {
                release_snapshot(entity_state.pending.front().snapshot);
                entity_state.pending.erase(entity_state.pending.begin());
            }
            sent.push_back(slot);
        }

        if (!records.empty()) {
            const std::size_t packet_bytes =
                protocol::bytes_for_bits(protocol::server_update_header_bits + records.bit_size());
            if (packet_bytes <= remaining) {
                send_packet(client.id, frame_, packet_entities, records);
                remaining -= packet_bytes;
            }
        }
        unsent.insert(unsent.end(), sent.begin(), sent.end());
        client.order = std::move(unsent);
    }
}

bool ReplicationServer::candidate_before(
    const SerializedCandidate& lhs,
    const SerializedCandidate& rhs) noexcept {
    if (lhs.priority != rhs.priority) {
        return lhs.priority > rhs.priority;
    }
    if (lhs.kind != rhs.kind) {
        return lhs.kind == SerializedCandidate::Kind::Update;
    }
    if (lhs.kind == SerializedCandidate::Kind::Update) {
        return lhs.slot < rhs.slot;
    }
    return lhs.destroy_index < rhs.destroy_index;
}

bool ReplicationServer::serialize_entity(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    ClientState& client,
    std::uint32_t slot,
    SyncFrame frame,
    std::vector<QuantizedBaseline>& scratch,
    SerializedEntity& out) {
    if (slot >= replicated_.size() || slot >= client.entity_states.size()) {
        return false;
    }

    const ReplicatedSlot& replicated = replicated_[slot];
    if (replicated.archetype.value >= settings.archetypes.size()) {
        return false;
    }

    const std::uint32_t snapshot = find_or_create_snapshot(registry, settings, client.id, slot, frame, scratch);
    if (snapshot == invalid_snapshot_id) {
        return false;
    }

    out.snapshot = snapshot;
    write_entity_record(registry, settings, client, slot, snapshots_[snapshot], out.payload);
    return !out.payload.empty();
}

std::uint32_t ReplicationServer::find_or_create_snapshot(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    ClientId client,
    std::uint32_t slot,
    SyncFrame frame,
    std::vector<QuantizedBaseline>& scratch) {
    if (slot >= replicated_.size()) {
        return invalid_snapshot_id;
    }

    const ReplicatedSlot& replicated = replicated_[slot];
    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    const NetworkOwner* owner = registry.try_get<NetworkOwner>(replicated.entity);

    scratch.clear();
    scratch.reserve(archetype.components.size());
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        const ComponentReplication& replication = archetype.components[component_index];
        if (replication.audience == ReplicationAudience::Owner &&
            (owner == nullptr || owner->client != client)) {
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
        quantized.component_index = static_cast<std::uint16_t>(component_index);
        found_ops->second.quantize(component_value, quantized.bytes);
        scratch.push_back(std::move(quantized));
    }

    if (scratch.empty()) {
        return invalid_snapshot_id;
    }

    for (const std::uint32_t index : replicated_[slot].snapshots) {
        if (index >= snapshots_.size()) {
            continue;
        }
        const QuantizedSnapshot& snapshot = snapshots_[index];
        if (snapshot.active && snapshot.slot == slot && snapshot.frame == frame &&
            snapshot.archetype == replicated.archetype && same_snapshot_components(snapshot, scratch)) {
            return index;
        }
    }

    std::uint32_t index = invalid_snapshot_id;
    if (!free_snapshots_.empty()) {
        index = free_snapshots_.back();
        free_snapshots_.pop_back();
    } else {
        if (snapshots_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::length_error("kage sync quantized snapshot space exhausted");
        }
        index = static_cast<std::uint32_t>(snapshots_.size());
        snapshots_.push_back(QuantizedSnapshot{});
    }

    QuantizedSnapshot& snapshot = snapshots_[index];
    snapshot.slot = slot;
    snapshot.frame = frame;
    snapshot.archetype = replicated.archetype;
    snapshot.ref_count = 0;
    snapshot.active = true;
    snapshot.baselines = std::move(scratch);
    replicated_[slot].snapshots.push_back(index);
    return index;
}

void ReplicationServer::retain_snapshot(std::uint32_t snapshot) {
    if (snapshot != invalid_snapshot_id && snapshot < snapshots_.size() && snapshots_[snapshot].active) {
        ++snapshots_[snapshot].ref_count;
    }
}

void ReplicationServer::release_snapshot(std::uint32_t snapshot) {
    if (snapshot == invalid_snapshot_id || snapshot >= snapshots_.size() || !snapshots_[snapshot].active) {
        return;
    }

    QuantizedSnapshot& current = snapshots_[snapshot];
    if (current.ref_count == 0) {
        if (current.slot < replicated_.size()) {
            std::vector<std::uint32_t>& slot_snapshots = replicated_[current.slot].snapshots;
            slot_snapshots.erase(
                std::remove(slot_snapshots.begin(), slot_snapshots.end(), snapshot),
                slot_snapshots.end());
        }
        current.active = false;
        current.baselines.clear();
        free_snapshots_.push_back(snapshot);
        return;
    }

    --current.ref_count;
    if (current.ref_count == 0) {
        if (current.slot < replicated_.size()) {
            std::vector<std::uint32_t>& slot_snapshots = replicated_[current.slot].snapshots;
            slot_snapshots.erase(
                std::remove(slot_snapshots.begin(), slot_snapshots.end(), snapshot),
                slot_snapshots.end());
        }
        current.active = false;
        current.baselines.clear();
        free_snapshots_.push_back(snapshot);
    }
}

void ReplicationServer::clear_client_entity_state(ClientEntityState& state) {
    release_snapshot(state.baseline);
    for (const ClientEntityState::PendingSnapshot& pending : state.pending) {
        release_snapshot(pending.snapshot);
    }
    state.baseline = invalid_snapshot_id;
    state.pending.clear();
}

bool ReplicationServer::acknowledge_destroy(ClientState& client, ecs::Entity entity, SyncFrame frame) {
    const auto found = std::find_if(
        client.pending_destroys.begin(),
        client.pending_destroys.end(),
        [entity, frame](const ClientDestroyState& pending) {
            return pending.entity == entity && frame <= pending.frame;
        });
    if (found == client.pending_destroys.end()) {
        return false;
    }

    client.pending_destroys.erase(found);
    return true;
}

const SyncComponentOps::QuantizedBytes* ReplicationServer::find_baseline_component(
    std::uint32_t snapshot,
    ecs::Entity component) const {
    if (snapshot == invalid_snapshot_id || snapshot >= snapshots_.size() || !snapshots_[snapshot].active) {
        return nullptr;
    }

    const auto found = std::find_if(
        snapshots_[snapshot].baselines.begin(),
        snapshots_[snapshot].baselines.end(),
        [component](const QuantizedBaseline& baseline) {
            return baseline.component == component;
        });
    return found != snapshots_[snapshot].baselines.end() ? &found->bytes : nullptr;
}

bool ReplicationServer::same_snapshot_components(
    const QuantizedSnapshot& snapshot,
    const std::vector<QuantizedBaseline>& baselines) const {
    if (snapshot.baselines.size() != baselines.size()) {
        return false;
    }

    for (std::size_t index = 0; index < baselines.size(); ++index) {
        if (snapshot.baselines[index].component != baselines[index].component ||
            snapshot.baselines[index].component_index != baselines[index].component_index ||
            snapshot.baselines[index].bytes != baselines[index].bytes) {
            return false;
        }
    }
    return true;
}

void ReplicationServer::write_entity_record(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    const ClientState& client,
    std::uint32_t slot,
    const QuantizedSnapshot& snapshot,
    BitBuffer& out) const {
    const ReplicatedSlot& replicated = replicated_[slot];
    const ClientEntityState& entity_state = client.entity_states[slot];
    bool delta = entity_state.baseline != invalid_snapshot_id &&
        entity_state.baseline < snapshots_.size() &&
        snapshots_[entity_state.baseline].active &&
        snapshots_[entity_state.baseline].archetype == snapshot.archetype;
    if (delta) {
        const QuantizedSnapshot& baseline = snapshots_[entity_state.baseline];
        delta = baseline.baselines.size() == snapshot.baselines.size();
        for (std::size_t index = 0; delta && index < snapshot.baselines.size(); ++index) {
            delta = baseline.baselines[index].component == snapshot.baselines[index].component;
        }
    }

    out.push_unsigned_bits(replicated.entity.value, 64U);
    out.push_bool(!delta);
    if (!delta) {
        out.push_bits(snapshot.archetype.value, 32U);
    } else {
        protocol::write_baseline_frame(out, snapshot.frame, snapshots_[entity_state.baseline].frame);
    }
    out.push_bits(static_cast<std::int64_t>(snapshot.baselines.size()), 16U);

    const SyncArchetype& archetype = settings.archetypes[snapshot.archetype.value];
    for (const QuantizedBaseline& baseline : snapshot.baselines) {
        if (baseline.component_index >= archetype.components.size() ||
            archetype.components[baseline.component_index].component != baseline.component) {
            throw std::logic_error("replicated snapshot component is not in its archetype");
        }

        const auto found_ops = settings.component_ops.find(baseline.component.value);
        if (found_ops == settings.component_ops.end()) {
            throw std::logic_error("sync component traits are not registered for replicated component");
        }

        const SyncComponentOps::QuantizedBytes* previous =
            delta ? find_baseline_component(entity_state.baseline, baseline.component) : nullptr;

        out.push_bits(static_cast<std::int64_t>(baseline.component_index), 16U);
        if (found_ops->second.serialized_size_bits != SyncComponentOps::variable_serialized_bits) {
            if (found_ops->second.serialized_size_bits >
                static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
                throw std::length_error("fixed-size replicated component payload bit size exceeds protocol limit");
            }
            out.push_bits(static_cast<std::int64_t>(found_ops->second.serialized_size_bits), 32U);
            const std::size_t payload_begin = out.bit_size();
            found_ops->second.serialize(previous, baseline.bytes, out);
            if (out.bit_size() - payload_begin != found_ops->second.serialized_size_bits) {
                throw std::logic_error("fixed-size replicated component serializer wrote an unexpected bit count");
            }
            continue;
        }

        const std::size_t payload_size_offset = out.bit_size();
        out.push_bits(0, 32U);
        const std::size_t payload_begin = out.bit_size();
        found_ops->second.serialize(previous, baseline.bytes, out);
        const std::size_t payload_bits = out.bit_size() - payload_begin;
        if (payload_bits > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::length_error("replicated component payload bit size exceeds protocol limit");
        }
        out.overwrite_unsigned_bits(payload_size_offset, payload_bits, 32U);
    }

    (void)registry;
}

void ReplicationServer::send_packet(
    ClientId client,
    SyncFrame frame,
    std::uint16_t entity_count,
    const BitBuffer& records) const {
    if (!options_.transport || entity_count == 0) {
        return;
    }

    BitBuffer packet;
    packet.reserve_bytes(options_.mtu_bytes);
    packet.push_bits(protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(entity_count, 16U);
    packet.push_buffer_bits(records);
    options_.transport(client, packet);
}

bool ReplicationServer::valid_archetype(const ecs::Registry& registry, SyncArchetypeId archetype) const {
    const SyncSettings& settings = registry.get<SyncSettings>();
    return archetype.value < settings.archetypes.size();
}

bool ReplicationServer::upsert_replicated(ecs::Registry& registry, ecs::Entity entity, SyncArchetypeId archetype) {
    if (!registry.alive(entity) || !valid_archetype(registry, archetype)) {
        deactivate_entity_index(ecs::Registry::entity_index(entity));
        return false;
    }

    const EntityKey key = entity.value;
    const auto found = entity_to_slot_.find(key);
    if (found != entity_to_slot_.end()) {
        replicated_[found->second].archetype = archetype;
        for (ClientState& client : clients_) {
            if (found->second < client.entity_states.size()) {
                clear_client_entity_state(client.entity_states[found->second]);
            }
        }
        return true;
    }

    deactivate_entity_index(ecs::Registry::entity_index(entity));

    const std::uint32_t slot = allocate_slot(entity, archetype);
    entity_to_slot_[key] = slot;
    entity_index_to_slot_[ecs::Registry::entity_index(entity)] = slot;
    for (ClientState& client : clients_) {
        if (client.reset_epochs.size() < replicated_.size()) {
            client.reset_epochs.resize(replicated_.size(), client.epoch);
            client.entity_states.resize(replicated_.size());
        }
        client.reset_epochs[slot] = client.epoch;
        clear_client_entity_state(client.entity_states[slot]);
        client.order.push_back(slot);
    }

    ++active_replicated_count_;
    return true;
}

std::uint32_t ReplicationServer::allocate_slot(ecs::Entity entity, SyncArchetypeId archetype) {
    if (!free_replicated_slots_.empty()) {
        const std::uint32_t slot = free_replicated_slots_.back();
        free_replicated_slots_.pop_back();
        replicated_[slot] = ReplicatedSlot{entity, archetype, {}, true};
        return slot;
    }

    if (replicated_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::length_error("kage sync replicated slot space exhausted");
    }

    const std::uint32_t slot = static_cast<std::uint32_t>(replicated_.size());
    replicated_.push_back(ReplicatedSlot{entity, archetype, {}, true});
    return slot;
}

void ReplicationServer::deactivate_slot(std::uint32_t slot) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }

    const ecs::Entity entity = replicated_[slot].entity;
    entity_to_slot_.erase(entity.value);
    entity_index_to_slot_.erase(ecs::Registry::entity_index(entity));
    replicated_[slot].active = false;
    replicated_[slot].snapshots.clear();
    free_replicated_slots_.push_back(slot);
    --active_replicated_count_;
    for (ClientState& client : clients_) {
        if (slot < client.entity_states.size()) {
            clear_client_entity_state(client.entity_states[slot]);
        }
        const auto found_destroy = std::find_if(
            client.pending_destroys.begin(),
            client.pending_destroys.end(),
            [entity](const ClientDestroyState& pending) {
                return pending.entity == entity;
            });
        if (found_destroy == client.pending_destroys.end()) {
            client.pending_destroys.push_back(ClientDestroyState{entity, frame_});
        }
    }
    remove_slot_from_client_orders(slot);
}

void ReplicationServer::deactivate_entity_index(std::uint32_t entity_index) {
    const auto found = entity_index_to_slot_.find(entity_index);
    if (found == entity_index_to_slot_.end()) {
        return;
    }

    deactivate_slot(found->second);
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
