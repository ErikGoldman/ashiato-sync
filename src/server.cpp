#include "kage/sync/server.hpp"

#include "kage/sync/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace kage::sync {
namespace {

constexpr std::size_t destroy_record_bits = 1U + protocol::network_entity_id_bits;
constexpr std::size_t max_pending_quantized_frames_per_entity = 64;

struct OutboundPacket {
    ClientId client = invalid_client_id;
    BitBuffer packet;
};

bool init_frame_data(const SyncArchetype& archetype, QuantizedFrameData& frame) {
    if (archetype.tags.size() > 64U ||
        archetype.components.size() > 63U ||
        archetype.component_offsets.size() != archetype.components.size() ||
        archetype.component_ops.size() != archetype.components.size()) {
        return false;
    }
    frame.tag_mask = 0;
    frame.present_mask = 0;
    frame.bytes.assign(archetype.total_quantized_bytes, 0U);
    return true;
}

std::size_t sync_slot_count(const SyncArchetype& archetype) noexcept {
    return archetype.components.size() + 1U;
}

bool has_tag_slot(const SyncArchetype& archetype) noexcept {
    return !archetype.tags.empty();
}

std::uint64_t sync_slot_bit(std::size_t slot) noexcept {
    return std::uint64_t{1} << slot;
}

bool frame_has_component(const QuantizedFrameData& frame, std::size_t component_index) {
    return component_index < 64U && (frame.present_mask & (std::uint64_t{1} << component_index)) != 0U;
}

const std::uint8_t* frame_component_data(
    const SyncArchetype& archetype,
    const QuantizedFrameData& frame,
    std::size_t component_index) {
    if (!frame_has_component(frame, component_index) ||
        component_index >= archetype.component_offsets.size() ||
        component_index >= archetype.component_ops.size()) {
        return nullptr;
    }
    const std::size_t offset = archetype.component_offsets[component_index];
    const std::size_t size = archetype.component_ops[component_index].quantized_size;
    if (offset + size > frame.bytes.size()) {
        return nullptr;
    }
    return frame.bytes.data() + offset;
}

std::uint8_t* mutable_frame_component_data(
    const SyncArchetype& archetype,
    QuantizedFrameData& frame,
    std::size_t component_index) {
    if (component_index >= 64U ||
        component_index >= archetype.component_offsets.size() ||
        component_index >= archetype.component_ops.size()) {
        return nullptr;
    }
    const std::size_t offset = archetype.component_offsets[component_index];
    const std::size_t size = archetype.component_ops[component_index].quantized_size;
    if (offset + size > frame.bytes.size()) {
        return nullptr;
    }
    frame.present_mask |= (std::uint64_t{1} << component_index);
    return frame.bytes.data() + offset;
}

bool set_frame_component_bytes(
    const SyncArchetype& archetype,
    QuantizedFrameData& frame,
    std::size_t component_index,
    const SyncComponentOps::QuantizedBytes& bytes) {
    if (component_index >= 64U ||
        component_index >= archetype.component_offsets.size() ||
        component_index >= archetype.component_ops.size() ||
        bytes.size() != archetype.component_ops[component_index].quantized_size) {
        return false;
    }
    const std::size_t offset = archetype.component_offsets[component_index];
    if (offset + bytes.size() > frame.bytes.size()) {
        return false;
    }
    std::memcpy(frame.bytes.data() + offset, bytes.data(), bytes.size());
    frame.present_mask |= (std::uint64_t{1} << component_index);
    return true;
}

std::uint64_t visible_tag_mask(
    const ecs::Registry& registry,
    const SyncArchetype& archetype,
    ecs::Entity entity,
    ClientId client) {
    std::uint64_t mask = 0;
    const NetworkOwner* owner = registry.try_get<NetworkOwner>(entity);
    for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
        const SyncTagReplication& replication = archetype.tags[tag_index];
        if (replication.audience == ReplicationAudience::Owner &&
            (owner == nullptr || owner->client != client)) {
            continue;
        }
        if (registry.has(entity, replication.tag)) {
            mask |= std::uint64_t{1} << tag_index;
        }
    }
    return mask;
}

BitBuffer make_server_packet(
    std::size_t mtu_bytes,
    SyncFrame frame,
    std::uint32_t packet_id,
    std::uint16_t entity_count,
    const BitBuffer& records) {
    BitBuffer packet;
    packet.reserve_bytes(mtu_bytes);
    packet.push_bits(protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(packet_id, protocol::server_packet_id_bits);
    packet.push_bits(entity_count, 16U);
    packet.push_buffer_bits(records);
    return packet;
}

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
        [frame](const ClientEntityState::PendingQuantizedFrame& pending) {
            return pending.frame == frame;
        });
    if (found_pending == entity_state.pending.end()) {
        return false;
    }

    const std::uint32_t acked_quantized_frame = found_pending->quantized_frame;
    if (acked_quantized_frame == invalid_quantized_frame_id || acked_quantized_frame >= quantized_frames_.size() || !quantized_frames_[acked_quantized_frame].active) {
        return false;
    }

    if (entity_state.baseline != acked_quantized_frame) {
        release_quantized_frame(entity_state.baseline);
        entity_state.baseline = acked_quantized_frame;
        retain_quantized_frame(entity_state.baseline);
    }

    for (auto pending = entity_state.pending.begin(); pending != entity_state.pending.end();) {
        if (pending->frame <= frame) {
            release_quantized_frame(pending->quantized_frame);
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
            const auto packet_id = static_cast<std::uint32_t>(packet.read_bits(protocol::server_packet_id_bits));
            all_valid = acknowledge_packet(state, packet_id) && all_valid;
        }
        cleanup_packet_acks(state);
        return all_valid;
    } catch (const std::exception&) {
        return false;
    }
}

std::size_t ReplicationServer::retained_quantized_frame_count() const noexcept {
    std::size_t count = 0;
    for (const QuantizedFrame& quantized_frame : quantized_frames_) {
        if (quantized_frame.active && quantized_frame.ref_count != 0) {
            ++count;
        }
    }
    return count;
}

std::size_t ReplicationServer::retained_quantized_frame_bytes() const noexcept {
    std::size_t bytes = 0;
    for (const QuantizedFrame& quantized_frame : quantized_frames_) {
        if (!quantized_frame.active || quantized_frame.ref_count == 0) {
            continue;
        }
        bytes += quantized_frame.data.bytes.size();
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
    if (options_.serialized_worker_threads > 1U && clients_.size() > 1U) {
        tick_serialized_parallel(registry);
        return;
    }

    refresh_replicated(registry);

    const SyncSettings& settings = registry.get<SyncSettings>();
    capture_dirty_components(registry, settings);
    std::vector<SerializedCandidate> candidates;
    std::vector<SerializedCandidate> update_candidates;
    std::vector<std::size_t> destroy_order;
    std::vector<std::uint32_t> sent;
    std::vector<std::uint32_t> unsent;
    BitBuffer records;
    std::vector<PacketAckRecord> packet_ack_records;
    SerializedEntity serialized;
    QuantizedFrameData quantized_frame_scratch;
    std::vector<std::uint64_t> quantized_frame_dirty_scratch;

    candidates.reserve(active_replicated_count_);
    update_candidates.reserve(active_replicated_count_);
    sent.reserve(active_replicated_count_);
    unsent.reserve(active_replicated_count_);
    ++frame_;

    for (ClientState& client : clients_) {
        ++client.epoch;

        std::size_t remaining = options_.bandwidth_limit_bytes_per_tick;
        std::uint16_t packet_entities = 0;
        candidates.clear();
        candidates.reserve(client.order.size() + client.pending_destroys.size());
        update_candidates.clear();
        update_candidates.reserve(client.order.size());
        destroy_order.clear();
        destroy_order.reserve(client.pending_destroys.size());
        sent.clear();
        unsent.clear();
        unsent.reserve(client.order.size());
        records.clear();
        records.reserve_bytes(options_.mtu_bytes);
        packet_ack_records.clear();
        packet_ack_records.reserve(options_.mtu_bytes / 8U);

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
            update_candidates.push_back(SerializedCandidate{SerializedCandidate::Kind::Update, slot, 0, priority});
        }

        if (client.pending_destroys.empty()) {
            candidates.insert(candidates.end(), update_candidates.begin(), update_candidates.end());
        } else {
            for (std::size_t index = 0; index < client.pending_destroys.size(); ++index) {
                destroy_order.push_back(index);
            }
            std::stable_sort(
                destroy_order.begin(),
                destroy_order.end(),
                [&](std::size_t lhs, std::size_t rhs) {
                    const ClientDestroyState& left = client.pending_destroys[lhs];
                    const ClientDestroyState& right = client.pending_destroys[rhs];
                    if (left.reset_epoch != right.reset_epoch) {
                        return left.reset_epoch < right.reset_epoch;
                    }
                    return lhs < rhs;
                });

            std::size_t destroy_cursor = 0;
            auto append_destroy = [&]() {
                const std::size_t destroy_index = destroy_order[destroy_cursor++];
                const ClientDestroyState& destroy = client.pending_destroys[destroy_index];
                candidates.push_back(SerializedCandidate{
                    SerializedCandidate::Kind::Destroy,
                    0,
                    destroy_index,
                    client.epoch - destroy.reset_epoch});
            };

            for (const SerializedCandidate& update : update_candidates) {
                const std::uint64_t update_priority = update.priority;
                while (destroy_cursor < destroy_order.size()) {
                    const ClientDestroyState& destroy = client.pending_destroys[destroy_order[destroy_cursor]];
                    const std::uint64_t destroy_priority = client.epoch - destroy.reset_epoch;
                    if (update_priority >= destroy_priority) {
                        break;
                    }
                    append_destroy();
                }
                candidates.push_back(update);
            }
            while (destroy_cursor < destroy_order.size()) {
                append_destroy();
            }
        }

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
                    send_packet(client, frame_, packet_entities, records, packet_ack_records);
                    remaining -= packet_bytes;
                    records.clear();
                    packet_ack_records.clear();
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
                records.push_bits(destroy.network_id, protocol::network_entity_id_bits);
                packet_ack_records.push_back(PacketAckRecord{destroy.entity, frame_, true});
                ++packet_entities;
                continue;
            }

            const std::uint32_t slot = candidate.slot;
            if (!slot_is_replicable(registry, slot)) {
                continue;
            }

            serialized.payload.clear();
            serialized.quantized_frame = invalid_quantized_frame_id;
            if (!serialize_entity(
                    registry,
                    settings,
                    client,
                    slot,
                    frame_,
                    quantized_frame_scratch,
                    quantized_frame_dirty_scratch,
                    serialized)) {
                unsent.push_back(slot);
                continue;
            }

            const std::size_t next_packet_bits =
                protocol::server_update_header_bits + records.bit_size() + 1U + serialized.payload.bit_size();
            if (!records.empty() && protocol::bytes_for_bits(next_packet_bits) > options_.mtu_bytes) {
                const std::size_t packet_bytes =
                    protocol::bytes_for_bits(protocol::server_update_header_bits + records.bit_size());
                if (packet_bytes > remaining) {
                    release_quantized_frame(serialized.quantized_frame);
                    unsent.push_back(slot);
                    continue;
                }
                send_packet(client, frame_, packet_entities, records, packet_ack_records);
                remaining -= packet_bytes;
                records.clear();
                packet_ack_records.clear();
                packet_entities = 0;
            }

            const std::size_t single_packet_bits =
                protocol::server_update_header_bits + records.bit_size() + 1U + serialized.payload.bit_size();
            const std::size_t packet_bytes = protocol::bytes_for_bits(single_packet_bits);
            if (packet_bytes > options_.mtu_bytes || packet_bytes > remaining) {
                release_quantized_frame(serialized.quantized_frame);
                unsent.push_back(slot);
                continue;
            }

            records.push_bool(false);
            records.push_buffer_bits(serialized.payload);
            packet_ack_records.push_back(PacketAckRecord{replicated_[slot].entity, frame_, false});
            ++packet_entities;
            client.reset_epochs[slot] = client.epoch;
            ClientEntityState& entity_state = client.entity_states[slot];
            entity_state.pending.push_back(ClientEntityState::PendingQuantizedFrame{serialized.quantized_frame, frame_});
            retain_quantized_frame(serialized.quantized_frame);
            while (entity_state.pending.size() > max_pending_quantized_frames_per_entity) {
                release_quantized_frame(entity_state.pending.front().quantized_frame);
                entity_state.pending.erase(entity_state.pending.begin());
            }
            sent.push_back(slot);
        }

        if (!records.empty()) {
            const std::size_t packet_bytes =
                protocol::bytes_for_bits(protocol::server_update_header_bits + records.bit_size());
            if (packet_bytes <= remaining) {
                send_packet(client, frame_, packet_entities, records, packet_ack_records);
                remaining -= packet_bytes;
            }
        }
        unsent.insert(unsent.end(), sent.begin(), sent.end());
        client.order = std::move(unsent);
    }
}

void ReplicationServer::tick_serialized_parallel(ecs::Registry& registry) {
    refresh_replicated(registry);

    const SyncSettings& settings = registry.get<SyncSettings>();
    capture_dirty_components(registry, settings);
    ++frame_;

    for (std::uint32_t slot = 0; slot < replicated_.size(); ++slot) {
        if (replicated_[slot].active && !slot_is_replicable(registry, slot)) {
            deactivate_slot(slot);
        }
    }

    const std::size_t worker_count =
        std::min<std::size_t>(options_.serialized_worker_threads, clients_.size());
    struct PreparedCandidate {
        SerializedCandidate candidate;
        std::uint32_t quantized_frame = invalid_quantized_frame_id;
    };
    struct PreparedClient {
        std::vector<PreparedCandidate> candidates;
    };

    std::vector<PreparedClient> prepared(clients_.size());
    std::vector<std::vector<OutboundPacket>> packets_by_client(clients_.size());
    std::vector<std::vector<std::uint32_t>> releases_by_client(clients_.size());
    std::vector<SerializedCandidate> update_candidates;
    std::vector<std::size_t> destroy_order;
    QuantizedFrameData quantized_frame_scratch;
    std::vector<std::uint64_t> quantized_frame_dirty_scratch;

    for (std::size_t client_index = 0; client_index < clients_.size(); ++client_index) {
        ClientState& client = clients_[client_index];
        ++client.epoch;

        PreparedClient& prepared_client = prepared[client_index];
        prepared_client.candidates.clear();
        prepared_client.candidates.reserve(client.order.size() + client.pending_destroys.size());
        update_candidates.clear();
        update_candidates.reserve(client.order.size());
        destroy_order.clear();
        destroy_order.reserve(client.pending_destroys.size());

        for (const std::uint32_t slot : client.order) {
            if (!slot_is_replicable(registry, slot)) {
                continue;
            }
            const std::uint64_t priority = slot < client.reset_epochs.size()
                ? client.epoch - client.reset_epochs[slot]
                : 0;
            update_candidates.push_back(SerializedCandidate{SerializedCandidate::Kind::Update, slot, 0, priority});
        }

        auto append_prepared_update = [&](const SerializedCandidate& candidate) {
            std::uint32_t quantized_frame = invalid_quantized_frame_id;
            if (candidate.slot < client.entity_states.size()) {
                quantized_frame = find_or_create_quantized_frame(
                    registry,
                    settings,
                    client,
                    candidate.slot,
                    frame_,
                    quantized_frame_scratch,
                    quantized_frame_dirty_scratch);
                retain_quantized_frame(quantized_frame);
            }
            prepared_client.candidates.push_back(PreparedCandidate{candidate, quantized_frame});
        };

        auto append_prepared_destroy = [&](std::size_t destroy_index) {
            const ClientDestroyState& destroy = client.pending_destroys[destroy_index];
            prepared_client.candidates.push_back(PreparedCandidate{
                SerializedCandidate{
                    SerializedCandidate::Kind::Destroy,
                    0,
                    destroy_index,
                    client.epoch - destroy.reset_epoch},
                invalid_quantized_frame_id});
        };

        if (client.pending_destroys.empty()) {
            for (const SerializedCandidate& candidate : update_candidates) {
                append_prepared_update(candidate);
            }
        } else {
            for (std::size_t index = 0; index < client.pending_destroys.size(); ++index) {
                destroy_order.push_back(index);
            }
            std::stable_sort(
                destroy_order.begin(),
                destroy_order.end(),
                [&](std::size_t lhs, std::size_t rhs) {
                    const ClientDestroyState& left = client.pending_destroys[lhs];
                    const ClientDestroyState& right = client.pending_destroys[rhs];
                    if (left.reset_epoch != right.reset_epoch) {
                        return left.reset_epoch < right.reset_epoch;
                    }
                    return lhs < rhs;
                });

            std::size_t destroy_cursor = 0;
            for (const SerializedCandidate& update : update_candidates) {
                const std::uint64_t update_priority = update.priority;
                while (destroy_cursor < destroy_order.size()) {
                    const ClientDestroyState& destroy = client.pending_destroys[destroy_order[destroy_cursor]];
                    const std::uint64_t destroy_priority = client.epoch - destroy.reset_epoch;
                    if (update_priority >= destroy_priority) {
                        break;
                    }
                    append_prepared_destroy(destroy_order[destroy_cursor++]);
                }
                append_prepared_update(update);
            }
            while (destroy_cursor < destroy_order.size()) {
                append_prepared_destroy(destroy_order[destroy_cursor++]);
            }
        }
    }

    std::atomic<std::size_t> next_client{0};
    std::mutex exception_mutex;
    std::exception_ptr worker_exception;

    auto pack_client = [&](std::size_t client_index) {
        ClientState& client = clients_[client_index];
        const PreparedClient& prepared_client = prepared[client_index];
        std::vector<std::uint32_t> sent;
        std::vector<std::uint32_t> unsent;
        BitBuffer records;
        std::vector<PacketAckRecord> packet_ack_records;
        SerializedEntity serialized;

        sent.reserve(client.order.size());
        unsent.reserve(client.order.size());
        records.reserve_bytes(options_.mtu_bytes);
        packet_ack_records.reserve(options_.mtu_bytes / 8U);

        std::size_t remaining = options_.bandwidth_limit_bytes_per_tick;
        std::uint16_t packet_entities = 0;

        auto emit_records = [&]() {
            if (packet_entities == 0U) {
                return;
            }
            const std::uint32_t packet_id = client.next_packet_id++;
            packets_by_client[client_index].push_back(OutboundPacket{
                client.id,
                make_server_packet(options_.mtu_bytes, frame_, packet_id, packet_entities, records)});
            track_packet_ack(client, packet_id, packet_ack_records);
            records.clear();
            packet_ack_records.clear();
            packet_entities = 0;
        };

        auto release_prepared_quantized_frame = [&](std::uint32_t quantized_frame) {
            releases_by_client[client_index].push_back(quantized_frame);
        };

        for (const PreparedCandidate& prepared_candidate : prepared_client.candidates) {
            const SerializedCandidate& candidate = prepared_candidate.candidate;
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
                    emit_records();
                    remaining -= packet_bytes;
                }

                const std::size_t packet_bytes = protocol::bytes_for_bits(
                    protocol::server_update_header_bits + records.bit_size() + destroy_record_bits);
                if (packet_bytes > options_.mtu_bytes || packet_bytes > remaining) {
                    continue;
                }

                destroy.frame = frame_;
                destroy.reset_epoch = client.epoch;
                records.push_bool(true);
                records.push_bits(destroy.network_id, protocol::network_entity_id_bits);
                packet_ack_records.push_back(PacketAckRecord{destroy.entity, frame_, true});
                ++packet_entities;
                continue;
            }

            const std::uint32_t slot = candidate.slot;
            if (prepared_candidate.quantized_frame == invalid_quantized_frame_id ||
                prepared_candidate.quantized_frame >= quantized_frames_.size() ||
                !quantized_frames_[prepared_candidate.quantized_frame].active) {
                unsent.push_back(slot);
                continue;
            }

            serialized.payload.clear();
            serialized.quantized_frame = prepared_candidate.quantized_frame;
            write_entity_record(registry, settings, client, slot, quantized_frames_[serialized.quantized_frame], serialized.payload);
            if (serialized.payload.empty()) {
                release_prepared_quantized_frame(serialized.quantized_frame);
                unsent.push_back(slot);
                continue;
            }

            const std::size_t next_packet_bits =
                protocol::server_update_header_bits + records.bit_size() + 1U + serialized.payload.bit_size();
            if (!records.empty() && protocol::bytes_for_bits(next_packet_bits) > options_.mtu_bytes) {
                const std::size_t packet_bytes =
                    protocol::bytes_for_bits(protocol::server_update_header_bits + records.bit_size());
                if (packet_bytes > remaining) {
                    release_prepared_quantized_frame(serialized.quantized_frame);
                    unsent.push_back(slot);
                    continue;
                }
                emit_records();
                remaining -= packet_bytes;
            }

            const std::size_t single_packet_bits =
                protocol::server_update_header_bits + records.bit_size() + 1U + serialized.payload.bit_size();
            const std::size_t packet_bytes = protocol::bytes_for_bits(single_packet_bits);
            if (packet_bytes > options_.mtu_bytes || packet_bytes > remaining) {
                release_prepared_quantized_frame(serialized.quantized_frame);
                unsent.push_back(slot);
                continue;
            }

            records.push_bool(false);
            records.push_buffer_bits(serialized.payload);
            packet_ack_records.push_back(PacketAckRecord{replicated_[slot].entity, frame_, false});
            ++packet_entities;
            client.reset_epochs[slot] = client.epoch;
            ClientEntityState& entity_state = client.entity_states[slot];
            entity_state.pending.push_back(ClientEntityState::PendingQuantizedFrame{serialized.quantized_frame, frame_});
            while (entity_state.pending.size() > max_pending_quantized_frames_per_entity) {
                const std::uint32_t quantized_frame = entity_state.pending.front().quantized_frame;
                release_prepared_quantized_frame(quantized_frame);
                entity_state.pending.erase(entity_state.pending.begin());
            }
            sent.push_back(slot);
        }

        if (!records.empty()) {
            const std::size_t packet_bytes =
                protocol::bytes_for_bits(protocol::server_update_header_bits + records.bit_size());
            if (packet_bytes <= remaining) {
                emit_records();
            }
        }
        unsent.insert(unsent.end(), sent.begin(), sent.end());
        client.order = std::move(unsent);
    };

    auto worker = [&]() {
        try {
            for (;;) {
                const std::size_t client_index = next_client.fetch_add(1U, std::memory_order_relaxed);
                if (client_index >= clients_.size()) {
                    break;
                }
                pack_client(client_index);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(exception_mutex);
            if (worker_exception == nullptr) {
                worker_exception = std::current_exception();
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers.emplace_back(worker);
    }
    for (std::thread& thread : workers) {
        thread.join();
    }
    if (worker_exception != nullptr) {
        std::rethrow_exception(worker_exception);
    }

    for (const std::vector<std::uint32_t>& releases : releases_by_client) {
        for (const std::uint32_t quantized_frame : releases) {
            release_quantized_frame(quantized_frame);
        }
    }

    if (options_.transport) {
        for (const std::vector<OutboundPacket>& client_packets : packets_by_client) {
            for (const OutboundPacket& outbound : client_packets) {
                options_.transport(outbound.client, outbound.packet);
            }
        }
    }
}

void ReplicationServer::capture_dirty_components(const ecs::Registry& registry, const SyncSettings& settings) {
    for (const SyncArchetype& archetype : settings.archetypes) {
        for (const SyncTagReplication& tag_replication : archetype.tags) {
            registry.each_dirty(tag_replication.tag, [&](ecs::Entity entity, const void*) {
                const auto found = entity_to_slot_.find(entity.value);
                if (found != entity_to_slot_.end()) {
                    mark_dirty_tag(settings, found->second, tag_replication.tag);
                }
            });
            registry.each_removed(tag_replication.tag, [&](ecs::Registry::ComponentRemoval removal) {
                const auto found = entity_index_to_slot_.find(removal.entity_index);
                if (found != entity_index_to_slot_.end()) {
                    mark_dirty_tag(settings, found->second, tag_replication.tag);
                }
            });
        }
    }

    for (const auto& component_ops : settings.component_ops) {
        const ecs::Entity component{component_ops.first};
        registry.each_dirty(component, [&](ecs::Entity entity, const void*) {
            const auto found = entity_to_slot_.find(entity.value);
            if (found != entity_to_slot_.end()) {
                mark_dirty_component(settings, found->second, component);
            }
        });
        registry.each_removed(component, [&](ecs::Registry::ComponentRemoval removal) {
            const auto found = entity_index_to_slot_.find(removal.entity_index);
            if (found != entity_index_to_slot_.end()) {
                mark_dirty_component(settings, found->second, component);
            }
        });
    }

    registry.each_dirty<NetworkOwner>([&](ecs::Entity entity, const void*) {
        const auto found = entity_to_slot_.find(entity.value);
        if (found != entity_to_slot_.end()) {
            mark_owner_visibility_dirty(settings, found->second);
        }
    });
    registry.each_removed<NetworkOwner>([&](ecs::Registry::ComponentRemoval removal) {
        const auto found = entity_index_to_slot_.find(removal.entity_index);
        if (found != entity_index_to_slot_.end()) {
            mark_owner_visibility_dirty(settings, found->second);
        }
    });
}

void ReplicationServer::mark_dirty_component(
    const SyncSettings& settings,
    std::uint32_t slot,
    ecs::Entity component) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }
    ReplicatedSlot& replicated = replicated_[slot];
    if (replicated.archetype.value >= settings.archetypes.size()) {
        return;
    }
    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    if (replicated.component_dirty_generations.size() < sync_slot_count(archetype)) {
        replicated.component_dirty_generations.resize(sync_slot_count(archetype), 1U);
    }
    for (std::size_t index = 0; index < archetype.components.size(); ++index) {
        if (archetype.components[index].component == component) {
            const std::size_t dirty_index = index + 1U;
            ++replicated.component_dirty_generations[dirty_index];
            if (replicated.component_dirty_generations[dirty_index] == 0) {
                replicated.component_dirty_generations[dirty_index] = 1;
            }
            return;
        }
    }
}

void ReplicationServer::mark_dirty_tag(const SyncSettings& settings, std::uint32_t slot, ecs::Entity tag) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }
    const ReplicatedSlot& replicated = replicated_[slot];
    if (replicated.archetype.value >= settings.archetypes.size()) {
        return;
    }
    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    for (const SyncTagReplication& replication : archetype.tags) {
        if (replication.tag == tag) {
            mark_dirty_tags(settings, slot);
            return;
        }
    }
}

void ReplicationServer::mark_dirty_tags(const SyncSettings& settings, std::uint32_t slot) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }
    ReplicatedSlot& replicated = replicated_[slot];
    if (replicated.archetype.value >= settings.archetypes.size()) {
        return;
    }
    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    if (archetype.tags.empty()) {
        return;
    }
    if (replicated.component_dirty_generations.size() < sync_slot_count(archetype)) {
        replicated.component_dirty_generations.resize(sync_slot_count(archetype), 1U);
    }
    ++replicated.component_dirty_generations[0];
    if (replicated.component_dirty_generations[0] == 0) {
        replicated.component_dirty_generations[0] = 1;
    }
}

void ReplicationServer::mark_owner_visibility_dirty(const SyncSettings& settings, std::uint32_t slot) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }
    ReplicatedSlot& replicated = replicated_[slot];
    if (replicated.archetype.value >= settings.archetypes.size()) {
        return;
    }
    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    if (replicated.component_dirty_generations.size() < sync_slot_count(archetype)) {
        replicated.component_dirty_generations.resize(sync_slot_count(archetype), 1U);
    }
    for (const SyncTagReplication& replication : archetype.tags) {
        if (replication.audience == ReplicationAudience::Owner) {
            ++replicated.component_dirty_generations[0];
            if (replicated.component_dirty_generations[0] == 0) {
                replicated.component_dirty_generations[0] = 1;
            }
            break;
        }
    }
    for (std::size_t index = 0; index < archetype.components.size(); ++index) {
        if (archetype.components[index].audience == ReplicationAudience::Owner) {
            const std::size_t dirty_index = index + 1U;
            ++replicated.component_dirty_generations[dirty_index];
            if (replicated.component_dirty_generations[dirty_index] == 0) {
                replicated.component_dirty_generations[dirty_index] = 1;
            }
        }
    }
}

bool ReplicationServer::archetype_is_same_frame_cacheable(const SyncArchetype& archetype) {
    for (const SyncTagReplication& replication : archetype.tags) {
        if (replication.audience != ReplicationAudience::All) {
            return false;
        }
    }
    for (const ComponentReplication& replication : archetype.components) {
        if (replication.audience != ReplicationAudience::All) {
            return false;
        }
    }
    return true;
}

bool ReplicationServer::serialize_entity(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    ClientState& client,
    std::uint32_t slot,
    SyncFrame frame,
    QuantizedFrameData& scratch,
    std::vector<std::uint64_t>& scratch_dirty_generations,
    SerializedEntity& out) {
    if (slot >= replicated_.size() || slot >= client.entity_states.size()) {
        return false;
    }

    const ReplicatedSlot& replicated = replicated_[slot];
    if (replicated.archetype.value >= settings.archetypes.size()) {
        return false;
    }

    const std::uint32_t quantized_frame = find_or_create_quantized_frame(
        registry,
        settings,
        client,
        slot,
        frame,
        scratch,
        scratch_dirty_generations);
    if (quantized_frame == invalid_quantized_frame_id) {
        return false;
    }

    out.quantized_frame = quantized_frame;
    write_entity_record(registry, settings, client, slot, quantized_frames_[quantized_frame], out.payload);
    return !out.payload.empty();
}

std::uint32_t ReplicationServer::find_or_create_quantized_frame(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    const ClientState& client,
    std::uint32_t slot,
    SyncFrame frame,
    QuantizedFrameData& scratch,
    std::vector<std::uint64_t>& scratch_dirty_generations) {
    if (slot >= replicated_.size()) {
        return invalid_quantized_frame_id;
    }

    const ReplicatedSlot& replicated = replicated_[slot];
    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    if (replicated.same_frame_cacheable &&
        replicated.same_frame_quantized_frame_frame == frame &&
        replicated.same_frame_quantized_frame != invalid_quantized_frame_id &&
        replicated.same_frame_quantized_frame < quantized_frames_.size()) {
        const QuantizedFrame& cached = quantized_frames_[replicated.same_frame_quantized_frame];
        if (cached.active && cached.slot == slot && cached.frame == frame &&
            cached.archetype == replicated.archetype) {
            return replicated.same_frame_quantized_frame;
        }
    }

    const NetworkOwner* owner = registry.try_get<NetworkOwner>(replicated.entity);
    const ClientEntityState* entity_state =
        slot < client.entity_states.size() ? &client.entity_states[slot] : nullptr;
    const QuantizedFrame* baseline_quantized_frame = nullptr;
    if (entity_state != nullptr &&
        entity_state->baseline != invalid_quantized_frame_id &&
        entity_state->baseline < quantized_frames_.size() &&
        quantized_frames_[entity_state->baseline].active &&
        quantized_frames_[entity_state->baseline].archetype == replicated.archetype) {
        baseline_quantized_frame = &quantized_frames_[entity_state->baseline];
    }

    if (!init_frame_data(archetype, scratch)) {
        return invalid_quantized_frame_id;
    }
    scratch_dirty_generations.assign(sync_slot_count(archetype), 0U);
    if (has_tag_slot(archetype)) {
        scratch.tag_mask = visible_tag_mask(registry, archetype, replicated.entity, client.id);
        scratch_dirty_generations[0] = !replicated.component_dirty_generations.empty()
            ? replicated.component_dirty_generations[0]
            : 0;
    }
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        const ComponentReplication& replication = archetype.components[component_index];
        if (replication.audience == ReplicationAudience::Owner &&
            (owner == nullptr || owner->client != client.id)) {
            continue;
        }

        const void* component_value = registry.get(replicated.entity, replication.component);
        if (component_value == nullptr) {
            continue;
        }

        if (component_index >= archetype.component_ops.size()) {
            throw std::logic_error("sync component traits are not registered for replicated component");
        }
        const SyncComponentOps& ops = archetype.component_ops[component_index];

        const std::size_t dirty_index = component_index + 1U;
        const std::uint64_t dirty_generation = dirty_index < replicated.component_dirty_generations.size()
            ? replicated.component_dirty_generations[dirty_index]
            : 0;
        scratch_dirty_generations[dirty_index] = dirty_generation;

        std::uint8_t* destination = mutable_frame_component_data(archetype, scratch, component_index);
        if (destination == nullptr) {
            return invalid_quantized_frame_id;
        }
        if (baseline_quantized_frame != nullptr &&
            dirty_index < baseline_quantized_frame->dirty_generations.size() &&
            baseline_quantized_frame->dirty_generations[dirty_index] == dirty_generation &&
            frame_has_component(baseline_quantized_frame->data, component_index)) {
            const std::size_t offset = archetype.component_offsets[component_index];
            const std::size_t size = archetype.component_ops[component_index].quantized_size;
            if (offset + size > baseline_quantized_frame->data.bytes.size()) {
                return invalid_quantized_frame_id;
            }
            std::memcpy(destination, baseline_quantized_frame->data.bytes.data() + offset, size);
        } else {
            if (ops.quantize_bytes == nullptr) {
                SyncComponentOps::QuantizedBytes quantized;
                ops.quantize(component_value, quantized);
                if (!set_frame_component_bytes(archetype, scratch, component_index, quantized)) {
                    return invalid_quantized_frame_id;
                }
            } else {
                ops.quantize_bytes(component_value, destination);
            }
        }
    }

    if (scratch.present_mask == 0U && !has_tag_slot(archetype)) {
        return invalid_quantized_frame_id;
    }

    for (const std::uint32_t index : replicated_[slot].quantized_frames) {
        if (index >= quantized_frames_.size()) {
            continue;
        }
        const QuantizedFrame& quantized_frame = quantized_frames_[index];
        if (quantized_frame.active && quantized_frame.slot == slot && quantized_frame.frame == frame &&
            quantized_frame.archetype == replicated.archetype &&
            same_quantized_frame_components(quantized_frame, scratch, scratch_dirty_generations)) {
            return index;
        }
    }

    std::uint32_t index = invalid_quantized_frame_id;
    if (!free_quantized_frames_.empty()) {
        index = free_quantized_frames_.back();
        free_quantized_frames_.pop_back();
    } else {
        if (quantized_frames_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::length_error("kage sync quantized frame space exhausted");
        }
        index = static_cast<std::uint32_t>(quantized_frames_.size());
        quantized_frames_.push_back(QuantizedFrame{});
    }

    QuantizedFrame& quantized_frame = quantized_frames_[index];
    quantized_frame.slot = slot;
    quantized_frame.frame = frame;
    quantized_frame.archetype = replicated.archetype;
    quantized_frame.ref_count = 0;
    quantized_frame.active = true;
    quantized_frame.data = std::move(scratch);
    quantized_frame.dirty_generations = std::move(scratch_dirty_generations);
    replicated_[slot].quantized_frames.push_back(index);
    if (replicated_[slot].same_frame_cacheable) {
        replicated_[slot].same_frame_quantized_frame = index;
        replicated_[slot].same_frame_quantized_frame_frame = frame;
    }
    return index;
}

void ReplicationServer::retain_quantized_frame(std::uint32_t quantized_frame) {
    if (quantized_frame != invalid_quantized_frame_id &&
        quantized_frame < quantized_frames_.size() &&
        quantized_frames_[quantized_frame].active) {
        ++quantized_frames_[quantized_frame].ref_count;
    }
}

void ReplicationServer::release_quantized_frame(std::uint32_t quantized_frame) {
    if (quantized_frame == invalid_quantized_frame_id ||
        quantized_frame >= quantized_frames_.size() ||
        !quantized_frames_[quantized_frame].active) {
        return;
    }

    QuantizedFrame& current = quantized_frames_[quantized_frame];
    if (current.ref_count == 0) {
        if (current.slot < replicated_.size()) {
            std::vector<std::uint32_t>& slot_quantized_frames = replicated_[current.slot].quantized_frames;
            slot_quantized_frames.erase(
                std::remove(slot_quantized_frames.begin(), slot_quantized_frames.end(), quantized_frame),
                slot_quantized_frames.end());
        }
        current.active = false;
        current.data.clear();
        current.dirty_generations.clear();
        free_quantized_frames_.push_back(quantized_frame);
        return;
    }

    --current.ref_count;
    if (current.ref_count == 0) {
        if (current.slot < replicated_.size()) {
            std::vector<std::uint32_t>& slot_quantized_frames = replicated_[current.slot].quantized_frames;
            slot_quantized_frames.erase(
                std::remove(slot_quantized_frames.begin(), slot_quantized_frames.end(), quantized_frame),
                slot_quantized_frames.end());
        }
        current.active = false;
        current.data.clear();
        current.dirty_generations.clear();
        free_quantized_frames_.push_back(quantized_frame);
    }
}

void ReplicationServer::clear_client_entity_state(ClientEntityState& state) {
    release_quantized_frame(state.baseline);
    for (const ClientEntityState::PendingQuantizedFrame& pending : state.pending) {
        release_quantized_frame(pending.quantized_frame);
    }
    state.baseline = invalid_quantized_frame_id;
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

bool ReplicationServer::acknowledge_packet(ClientState& client, std::uint32_t packet_id) {
    const auto found = std::find_if(
        client.pending_packet_acks.begin(),
        client.pending_packet_acks.end(),
        [packet_id](const PendingPacketAck& pending) {
            return pending.packet_id == packet_id;
        });
    if (found == client.pending_packet_acks.end()) {
        return false;
    }

    bool all_valid = true;
    for (const PacketAckRecord& record : found->records) {
        const bool accepted = record.destroy
            ? acknowledge_destroy(client, record.entity, record.frame)
            : acknowledge_entity(client.id, record.entity, record.frame);
        all_valid = accepted && all_valid;
    }
    client.pending_packet_acks.erase(found);
    return all_valid;
}

bool ReplicationServer::packet_ack_record_pending(const ClientState& client, const PacketAckRecord& record) const {
    if (record.destroy) {
        return std::any_of(
            client.pending_destroys.begin(),
            client.pending_destroys.end(),
            [&](const ClientDestroyState& pending) {
                return pending.entity == record.entity && record.frame <= pending.frame;
            });
    }

    const auto found_slot = entity_to_slot_.find(record.entity.value);
    if (found_slot == entity_to_slot_.end()) {
        return false;
    }
    const std::uint32_t slot = found_slot->second;
    if (slot >= client.entity_states.size()) {
        return false;
    }
    const ClientEntityState& state = client.entity_states[slot];
    return std::any_of(
        state.pending.begin(),
        state.pending.end(),
        [&](const ClientEntityState::PendingQuantizedFrame& pending) {
            return pending.frame == record.frame;
        });
}

void ReplicationServer::cleanup_packet_acks(ClientState& client) {
    client.pending_packet_acks.erase(
        std::remove_if(
            client.pending_packet_acks.begin(),
            client.pending_packet_acks.end(),
            [&](const PendingPacketAck& pending_packet) {
                return std::none_of(
                    pending_packet.records.begin(),
                    pending_packet.records.end(),
                    [&](const PacketAckRecord& record) {
                        return packet_ack_record_pending(client, record);
                    });
            }),
        client.pending_packet_acks.end());
}

bool ReplicationServer::same_quantized_frame_components(
    const QuantizedFrame& quantized_frame,
    const QuantizedFrameData& data,
    const std::vector<std::uint64_t>& dirty_generations) const {
    if (quantized_frame.data.tag_mask != data.tag_mask ||
        quantized_frame.data.present_mask != data.present_mask ||
        quantized_frame.data.bytes != data.bytes ||
        quantized_frame.dirty_generations.size() != dirty_generations.size()) {
        return false;
    }
    for (std::size_t index = 0; index < dirty_generations.size(); ++index) {
        if (quantized_frame.dirty_generations[index] != dirty_generations[index]) {
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
    const QuantizedFrame& quantized_frame,
    BitBuffer& out) const {
    const ReplicatedSlot& replicated = replicated_[slot];
    const ClientEntityState& entity_state = client.entity_states[slot];
    bool delta = entity_state.baseline != invalid_quantized_frame_id &&
        entity_state.baseline < quantized_frames_.size() &&
        quantized_frames_[entity_state.baseline].active &&
        quantized_frames_[entity_state.baseline].archetype == quantized_frame.archetype;
    if (delta) {
        const QuantizedFrame& baseline = quantized_frames_[entity_state.baseline];
        delta = baseline.data.present_mask == quantized_frame.data.present_mask;
    }

    out.push_bits(replicated.network_id, protocol::network_entity_id_bits);
    out.push_bool(!delta);
    if (!delta) {
        out.push_unsigned_bits(replicated.entity.value, 64U);
        out.push_bits(quantized_frame.archetype.value, 32U);
    } else {
        protocol::write_baseline_frame(out, quantized_frame.frame, quantized_frames_[entity_state.baseline].frame);
    }

    const SyncArchetype& archetype = settings.archetypes[quantized_frame.archetype.value];
    if (delta) {
        const QuantizedFrame& baseline_quantized_frame = quantized_frames_[entity_state.baseline];
        std::uint64_t changed_mask = 0;
        if (has_tag_slot(archetype)) {
            const std::uint64_t tag_bit_mask = archetype.tags.size() == 64U
                ? std::numeric_limits<std::uint64_t>::max()
                : ((std::uint64_t{1} << archetype.tags.size()) - 1U);
            const bool tags_changed =
                (quantized_frame.data.tag_mask & tag_bit_mask) !=
                (baseline_quantized_frame.data.tag_mask & tag_bit_mask);
            out.push_bool(tags_changed);
            if (tags_changed) {
                changed_mask |= sync_slot_bit(0);
            }
        } else {
            out.push_bool(false);
        }
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            if (!frame_has_component(quantized_frame.data, component_index)) {
                out.push_bool(false);
                continue;
            }
            const std::size_t offset = archetype.component_offsets[component_index];
            const std::size_t size = archetype.component_ops[component_index].quantized_size;
            if (offset + size > quantized_frame.data.bytes.size() ||
                offset + size > baseline_quantized_frame.data.bytes.size()) {
                throw std::logic_error("replicated quantized frame component bytes are out of range");
            }
            const bool component_changed =
                std::memcmp(
                    quantized_frame.data.bytes.data() + offset,
                    baseline_quantized_frame.data.bytes.data() + offset,
                    size) != 0;
            out.push_bool(component_changed);
            if (component_changed) {
                changed_mask |= sync_slot_bit(component_index + 1U);
            }
        }
        if ((changed_mask & sync_slot_bit(0)) != 0U) {
            out.push_unsigned_bits(quantized_frame.data.tag_mask, archetype.tags.size());
        }
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            if ((changed_mask & sync_slot_bit(component_index + 1U)) == 0U) {
                continue;
            }
            if (component_index >= archetype.component_ops.size()) {
                throw std::logic_error("sync component traits are not registered for replicated component");
            }
            const SyncComponentOps& ops = archetype.component_ops[component_index];
            const std::uint8_t* previous = frame_component_data(archetype, baseline_quantized_frame.data, component_index);
            const std::uint8_t* current = frame_component_data(archetype, quantized_frame.data, component_index);
            if (previous == nullptr || current == nullptr) {
                throw std::logic_error("replicated quantized frame component bytes are missing");
            }
            if (ops.serialize_bytes != nullptr) {
                ops.serialize_bytes(previous, current, out);
            } else {
                SyncComponentOps::QuantizedBytes previous_bytes;
                SyncComponentOps::QuantizedBytes current_bytes;
                previous_bytes.assign(previous, ops.quantized_size);
                current_bytes.assign(current, ops.quantized_size);
                ops.serialize(&previous_bytes, current_bytes, out);
            }
        }
        (void)registry;
        return;
    }

    std::uint16_t component_count = 0;
    if (has_tag_slot(archetype)) {
        ++component_count;
    }
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (frame_has_component(quantized_frame.data, component_index)) {
            ++component_count;
        }
    }
    out.push_bits(static_cast<std::int64_t>(component_count), 16U);
    const std::size_t sync_slot_bits = protocol::bits_for_range(sync_slot_count(archetype));
    if (has_tag_slot(archetype)) {
        out.push_bits(0, sync_slot_bits);
        out.push_unsigned_bits(quantized_frame.data.tag_mask, archetype.tags.size());
    }
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (!frame_has_component(quantized_frame.data, component_index)) {
            continue;
        }
        if (component_index >= archetype.component_ops.size()) {
            throw std::logic_error("sync component traits are not registered for replicated component");
        }
        const SyncComponentOps& ops = archetype.component_ops[component_index];
        const std::uint8_t* current = frame_component_data(archetype, quantized_frame.data, component_index);
        if (current == nullptr) {
            throw std::logic_error("replicated quantized frame component bytes are missing");
        }

        out.push_bits(static_cast<std::int64_t>(component_index + 1U), sync_slot_bits);
        if (ops.serialize_bytes != nullptr) {
            ops.serialize_bytes(nullptr, current, out);
        } else {
            SyncComponentOps::QuantizedBytes current_bytes;
            current_bytes.assign(current, ops.quantized_size);
            ops.serialize(nullptr, current_bytes, out);
        }
    }

    (void)registry;
}

void ReplicationServer::track_packet_ack(
    ClientState& client,
    std::uint32_t packet_id,
    const std::vector<PacketAckRecord>& records) {
    if (records.empty()) {
        return;
    }
    client.pending_packet_acks.push_back(PendingPacketAck{packet_id, records});
}

void ReplicationServer::send_packet(
    ClientState& client,
    SyncFrame frame,
    std::uint16_t entity_count,
    const BitBuffer& records,
    const std::vector<PacketAckRecord>& ack_records) {
    if (!options_.transport || entity_count == 0) {
        return;
    }

    const std::uint32_t packet_id = client.next_packet_id++;
    BitBuffer packet = make_server_packet(options_.mtu_bytes, frame, packet_id, entity_count, records);
    track_packet_ack(client, packet_id, ack_records);
    options_.transport(client.id, packet);
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
        replicated_[found->second].same_frame_quantized_frame = invalid_quantized_frame_id;
        replicated_[found->second].same_frame_quantized_frame_frame = 0;
        const SyncSettings& settings = registry.get<SyncSettings>();
        if (archetype.value < settings.archetypes.size()) {
            replicated_[found->second].same_frame_cacheable =
                archetype_is_same_frame_cacheable(settings.archetypes[archetype.value]);
            replicated_[found->second].component_dirty_generations.assign(
                sync_slot_count(settings.archetypes[archetype.value]),
                1U);
        }
        for (ClientState& client : clients_) {
            if (found->second < client.entity_states.size()) {
                clear_client_entity_state(client.entity_states[found->second]);
            }
        }
        return true;
    }

    deactivate_entity_index(ecs::Registry::entity_index(entity));

    const std::uint32_t slot = allocate_slot(entity, archetype);
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (archetype.value < settings.archetypes.size()) {
        replicated_[slot].same_frame_cacheable =
            archetype_is_same_frame_cacheable(settings.archetypes[archetype.value]);
        replicated_[slot].component_dirty_generations.assign(
            sync_slot_count(settings.archetypes[archetype.value]),
            1U);
    }
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
    if (next_network_id_ == 0U) {
        throw std::length_error("kage sync network entity id space exhausted");
    }
    const std::uint32_t network_id = next_network_id_++;
    if (!free_replicated_slots_.empty()) {
        const std::uint32_t slot = free_replicated_slots_.back();
        free_replicated_slots_.pop_back();
        replicated_[slot] =
            ReplicatedSlot{entity, archetype, network_id, {}, {}, invalid_quantized_frame_id, 0, false, true};
        return slot;
    }

    if (replicated_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::length_error("kage sync replicated slot space exhausted");
    }

    const std::uint32_t slot = static_cast<std::uint32_t>(replicated_.size());
    replicated_.push_back(
        ReplicatedSlot{entity, archetype, network_id, {}, {}, invalid_quantized_frame_id, 0, false, true});
    return slot;
}

void ReplicationServer::deactivate_slot(std::uint32_t slot) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }

    const ecs::Entity entity = replicated_[slot].entity;
    const std::uint32_t network_id = replicated_[slot].network_id;
    entity_to_slot_.erase(entity.value);
    entity_index_to_slot_.erase(ecs::Registry::entity_index(entity));
    replicated_[slot].active = false;
    replicated_[slot].quantized_frames.clear();
    replicated_[slot].same_frame_quantized_frame = invalid_quantized_frame_id;
    replicated_[slot].same_frame_quantized_frame_frame = 0;
    replicated_[slot].same_frame_cacheable = false;
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
            client.pending_destroys.push_back(ClientDestroyState{entity, frame_, 0, network_id});
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
