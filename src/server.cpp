#include "kage/sync/server.hpp"

#include "kage/sync/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
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

constexpr std::size_t max_pending_quantized_frames_per_entity = 64;
constexpr std::size_t max_cues_per_entity_record = 255;

std::size_t configured_packet_id_bits(const ReplicationServerOptions& options) noexcept {
    return protocol::packet_id_bits_for_max_pending(options.max_pending_packet_acks_per_client);
}

std::size_t server_update_header_bits(const ReplicationServerOptions& options) noexcept {
    return 8U + 32U + configured_packet_id_bits(options) + 16U;
}

std::size_t destroy_record_bits(std::uint32_t network_id, std::size_t tier0_bits) noexcept {
    return 1U + protocol::network_entity_id_encoded_bits(network_id, tier0_bits);
}

constexpr std::uint32_t invalid_slot_or_free = std::numeric_limits<std::uint32_t>::max();

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
    std::size_t packet_id_bits,
    SyncFrame frame,
    std::uint32_t packet_id,
    std::uint16_t entity_count,
    const BitBuffer& records) {
    BitBuffer packet;
    packet.reserve_bytes(mtu_bytes);
    packet.push_bits(protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(packet_id, packet_id_bits);
    packet.push_bits(entity_count, 16U);
    packet.push_buffer_bits(records);
    return packet;
}

}  // namespace

ReplicationServer::ReplicationServer(ReplicationServerOptions options)
    : options_(options) {
    if (!protocol::valid_network_entity_id_tier0_bits(options_.network_entity_id_tier0_bits)) {
        throw std::invalid_argument("network entity id tier0 bits must be in [1, 22]");
    }
    if (options_.fixed_dt_seconds <= 0.0 || !std::isfinite(options_.fixed_dt_seconds)) {
        throw std::invalid_argument("fixed dt seconds must be finite and positive");
    }
    if (options_.connect_resend_interval_seconds <= 0.0 ||
        !std::isfinite(options_.connect_resend_interval_seconds)) {
        throw std::invalid_argument("connect resend interval seconds must be finite and positive");
    }
    if (options_.idle_client_timeout_seconds < 0.0 ||
        !std::isfinite(options_.idle_client_timeout_seconds)) {
        throw std::invalid_argument("idle client timeout seconds must be finite and non-negative");
    }
}

void ReplicationServer::set_transport(TransportFn transport) {
    options_.transport = std::move(transport);
}

bool ReplicationServer::add_client(ClientId client) {
    return add_client_for_peer(client, client, true);
}

bool ReplicationServer::add_client_for_peer(ClientId peer, ClientId client, bool ready_for_updates) {
    if (client == invalid_client_id ||
        client > max_client_entity_network_id_client ||
        peer == invalid_client_id ||
        client_to_index_.find(client) != client_to_index_.end() ||
        peer_to_index_.find(peer) != peer_to_index_.end()) {
        return false;
    }

    ClientState state;
    state.id = client;
    state.peer = peer;
    state.ready_for_updates = ready_for_updates;
    state.connect_resend_accumulator_seconds = options_.connect_resend_interval_seconds;
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
    peer_to_index_[peer] = index;
    next_connect_client_id_ = std::max(next_connect_client_id_, client + 1U);
    return true;
}

bool ReplicationServer::remove_client(ClientId client) {
    const auto found = client_to_index_.find(client);
    if (found == client_to_index_.end()) {
        return false;
    }

    const std::size_t index = found->second;
    const ClientId removed_peer = clients_[index].peer;
    for (ClientEntityState& entity_state : clients_[index].entity_states) {
        clear_client_entity_state(entity_state);
    }

    const std::size_t last = clients_.size() - 1;
    if (index != last) {
        clients_[index] = std::move(clients_[last]);
        client_to_index_[clients_[index].id] = index;
        peer_to_index_[clients_[index].peer] = index;
    }

    clients_.pop_back();
    client_to_index_.erase(found);
    peer_to_index_.erase(removed_peer);
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
    acknowledge_cues(entity_state, frame);
    return true;
}

bool ReplicationServer::process_packet(ClientId client, BitBuffer packet) {
    try {
        if (packet.remaining_bits() < 8U) {
            return false;
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));

        if (message == protocol::client_connect_request_message) {
            std::string token;
            if (!protocol::read_string(packet, token)) {
                return false;
            }
            const auto peer_found = peer_to_index_.find(client);
            if (peer_found != peer_to_index_.end()) {
                clients_[peer_found->second].idle_seconds = 0.0;
                send_connect_response(clients_[peer_found->second]);
                return true;
            }

            ClientId accepted_client = next_connect_client_id_;
            std::string error;
            bool accepted = true;
            if (options_.connect_handler) {
                accepted = options_.connect_handler(token, accepted_client, error);
            }
            if (!accepted || accepted_client == invalid_client_id ||
                accepted_client > max_client_entity_network_id_client) {
                if (accepted && error.empty() && accepted_client > max_client_entity_network_id_client) {
                    error = "client id out of range";
                }
                BitBuffer response;
                response.reserve_bytes(options_.mtu_bytes);
                response.push_bits(protocol::server_connect_response_message, 8U);
                response.push_bool(false);
                protocol::write_string(response, error);
                if (options_.transport) {
                    options_.transport(client, response);
                }
                return accepted_client != invalid_client_id || !accepted;
            }

            if (!add_client_for_peer(client, accepted_client, false)) {
                return false;
            }
            send_connect_response(clients_[client_to_index_[accepted_client]]);
            return true;
        }

        const auto peer_found = peer_to_index_.find(client);
        if (peer_found == peer_to_index_.end()) {
            return false;
        }

        ClientState& state = clients_[peer_found->second];
        state.idle_seconds = 0.0;
        if (message == protocol::client_connect_ack_message) {
            if (packet.remaining_bits() < 64U) {
                return false;
            }
            const ClientId acked_client = static_cast<ClientId>(packet.read_unsigned_bits(64U));
            if (acked_client != state.id) {
                return false;
            }
            state.ready_for_updates = true;
            return true;
        }

        if (message == protocol::client_ping_message) {
            if (packet.remaining_bits() < 64U) {
                return false;
            }
            const auto sequence = static_cast<std::uint32_t>(packet.read_bits(32U));
            const auto send_frame = static_cast<SyncFrame>(packet.read_bits(32U));
            send_pong(state.peer, sequence, send_frame);
            return true;
        }

        if (message != protocol::client_ack_message) {
            return false;
        }

        const std::size_t packet_id_bits = configured_packet_id_bits(options_);
        if (packet.remaining_bits() < 16U) {
            return false;
        }
        const auto ack_count = static_cast<std::uint16_t>(packet.read_bits(16U));
        bool all_valid = true;
        for (std::uint16_t ack = 0; ack < ack_count; ++ack) {
            const auto packet_id = static_cast<std::uint32_t>(packet.read_bits(packet_id_bits));
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
    disconnect_idle_clients();
    refresh_replicated(registry);

    std::vector<std::uint32_t> sent;
    std::vector<std::uint32_t> next_order;
    sent.reserve(active_replicated_count_);
    next_order.reserve(active_replicated_count_);

    for (ClientState& client : clients_) {
        if (!client.ready_for_updates) {
            client.connect_resend_accumulator_seconds += options_.fixed_dt_seconds;
            if (client.connect_resend_accumulator_seconds >= options_.connect_resend_interval_seconds) {
                send_connect_response(client);
                client.connect_resend_accumulator_seconds = 0.0;
            }
            continue;
        }

        ++client.epoch;

        std::size_t remaining = options_.bandwidth_limit_bytes_per_tick;
        refresh_client_priorities(registry, client);
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
            if (slot >= client.entity_states.size() || !client.entity_states[slot].priority_replicate) {
                next_order.push_back(slot);
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

void ReplicationServer::disconnect_idle_clients() {
    if (options_.idle_client_timeout_seconds == 0.0) {
        return;
    }
    for (std::size_t index = 0; index < clients_.size();) {
        ClientState& client = clients_[index];
        client.idle_seconds += options_.fixed_dt_seconds;
        if (client.idle_seconds >= options_.idle_client_timeout_seconds) {
            remove_client(client.id);
            continue;
        }
        ++index;
    }
}

void ReplicationServer::refresh_client_priorities(const ecs::Registry& registry, ClientState& client) {
    if (!options_.prioritizer || options_.prioritizer_interval_frames == 0U) {
        return;
    }
    std::vector<ReplicationPriorityObject> objects;
    std::vector<std::uint32_t> slots;
    objects.reserve(client.order.size());
    slots.reserve(client.order.size());

    for (const std::uint32_t slot : client.order) {
        if (slot >= replicated_.size() || slot >= client.entity_states.size() || !replicated_[slot].active) {
            continue;
        }
        ClientEntityState& entity_state = client.entity_states[slot];
        if (entity_state.priority_frame != 0U &&
            frame_ - entity_state.priority_frame < options_.prioritizer_interval_frames) {
            continue;
        }
        if (!slot_is_replicable(registry, slot)) {
            continue;
        }
        objects.push_back(ReplicationPriorityObject{replicated_[slot].entity});
        slots.push_back(slot);
    }
    if (objects.empty()) {
        return;
    }

    std::vector<ReplicationPriorityDecision> decisions(objects.size());
    options_.prioritizer(client.id, objects, decisions);
    if (decisions.size() != objects.size()) {
        throw std::invalid_argument("replication prioritizer must return one decision per object");
    }

    for (std::size_t index = 0; index < slots.size(); ++index) {
        const std::uint32_t slot = slots[index];
        if (!decisions[index].replicate) {
            hide_slot_for_client(client, slot);
        }
        ClientEntityState& entity_state = client.entity_states[slot];
        entity_state.priority_replicate = decisions[index].replicate;
        entity_state.priority = decisions[index].priority;
        entity_state.component_mask = decisions[index].component_mask;
        entity_state.priority_frame = frame_;
    }
}

void ReplicationServer::tick_serialized(ecs::Registry& registry) {
    disconnect_idle_clients();
    if (options_.serialized_worker_threads > 1U && clients_.size() > 1U) {
        tick_serialized_parallel(registry);
        return;
    }

    refresh_replicated(registry);

    const SyncSettings& settings = registry.get<SyncSettings>();
    capture_dirty_components(registry, settings);
    capture_queued_cues(settings);
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
    const std::size_t update_header_bits = server_update_header_bits(options_);

    for (ClientState& client : clients_) {
        if (!client.ready_for_updates) {
            client.connect_resend_accumulator_seconds += options_.fixed_dt_seconds;
            if (client.connect_resend_accumulator_seconds >= options_.connect_resend_interval_seconds) {
                send_connect_response(client);
                client.connect_resend_accumulator_seconds = 0.0;
            }
            continue;
        }

        ++client.epoch;
        expire_pending_cues(client, frame_);

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

        refresh_client_priorities(registry, client);
        for (const std::uint32_t slot : client.order) {
            if (!slot_is_replicable(registry, slot)) {
                if (slot < replicated_.size() && replicated_[slot].active) {
                    deactivate_slot(slot);
                }
                continue;
            }
            if (slot >= client.entity_states.size() || !client.entity_states[slot].priority_replicate) {
                unsent.push_back(slot);
                continue;
            }
            const std::uint64_t age_priority = slot < client.reset_epochs.size()
                ? client.epoch - client.reset_epochs[slot]
                : 0;
            const ClientEntityState& entity_state = client.entity_states[slot];
            update_candidates.push_back(SerializedCandidate{
                SerializedCandidate::Kind::Update,
                slot,
                0,
                age_priority + entity_state.priority,
                entity_state.component_mask});
        }
        std::stable_sort(
            update_candidates.begin(),
            update_candidates.end(),
            [](const SerializedCandidate& lhs, const SerializedCandidate& rhs) {
                return lhs.priority > rhs.priority;
            });

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
                const std::size_t destroy_bits =
                    destroy_record_bits(destroy.network_id, options_.network_entity_id_tier0_bits);
                const std::size_t next_packet_bits =
                    update_header_bits + records.bit_size() + destroy_bits;
                if (!records.empty() && protocol::bytes_for_bits(next_packet_bits) > options_.mtu_bytes) {
                    const std::size_t packet_bytes =
                        protocol::bytes_for_bits(update_header_bits + records.bit_size());
                    if (packet_bytes > remaining) {
                        break;
                    }
                    send_packet(client, frame_, packet_entities, records, packet_ack_records);
                    remaining -= packet_bytes;
                    records.clear();
                    packet_ack_records.clear();
                    packet_entities = 0;
                }

                const std::size_t packet_bytes =
                    protocol::bytes_for_bits(update_header_bits + records.bit_size() + destroy_bits);
                if (packet_bytes > options_.mtu_bytes || packet_bytes > remaining) {
                    continue;
                }

                destroy.frame = frame_;
                destroy.reset_epoch = client.epoch;
                records.push_bool(true);
                protocol::write_network_entity_id(
                    records,
                    destroy.network_id,
                    options_.network_entity_id_tier0_bits);
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
                    candidate.component_mask,
                    serialized)) {
                unsent.push_back(slot);
                continue;
            }

            const std::size_t next_packet_bits =
                update_header_bits + records.bit_size() + 1U + serialized.payload.bit_size();
            if (!records.empty() && protocol::bytes_for_bits(next_packet_bits) > options_.mtu_bytes) {
                const std::size_t packet_bytes =
                    protocol::bytes_for_bits(update_header_bits + records.bit_size());
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
                update_header_bits + records.bit_size() + 1U + serialized.payload.bit_size();
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
                protocol::bytes_for_bits(update_header_bits + records.bit_size());
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
    capture_queued_cues(settings);
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
        if (!client.ready_for_updates) {
            client.connect_resend_accumulator_seconds += options_.fixed_dt_seconds;
            if (client.connect_resend_accumulator_seconds >= options_.connect_resend_interval_seconds) {
                send_connect_response(client);
                client.connect_resend_accumulator_seconds = 0.0;
            }
            continue;
        }
        ++client.epoch;
        expire_pending_cues(client, frame_);

        PreparedClient& prepared_client = prepared[client_index];
        prepared_client.candidates.clear();
        prepared_client.candidates.reserve(client.order.size() + client.pending_destroys.size());
        update_candidates.clear();
        update_candidates.reserve(client.order.size());
        destroy_order.clear();
        destroy_order.reserve(client.pending_destroys.size());

        refresh_client_priorities(registry, client);
        for (const std::uint32_t slot : client.order) {
            if (!slot_is_replicable(registry, slot)) {
                continue;
            }
            const std::uint64_t priority = slot < client.reset_epochs.size()
                ? client.epoch - client.reset_epochs[slot]
                : 0;
            if (slot >= client.entity_states.size() || !client.entity_states[slot].priority_replicate) {
                continue;
            }
            const ClientEntityState& entity_state = client.entity_states[slot];
            update_candidates.push_back(SerializedCandidate{
                SerializedCandidate::Kind::Update,
                slot,
                0,
                priority + entity_state.priority,
                entity_state.component_mask});
        }
        std::stable_sort(
            update_candidates.begin(),
            update_candidates.end(),
            [](const SerializedCandidate& lhs, const SerializedCandidate& rhs) {
                return lhs.priority > rhs.priority;
            });

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
        if (!client.ready_for_updates) {
            return;
        }
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
            const std::uint32_t packet_id = allocate_packet_id(client);
            packets_by_client[client_index].push_back(OutboundPacket{
                client.peer,
                make_server_packet(
                    options_.mtu_bytes,
                    configured_packet_id_bits(options_),
                    frame_,
                    packet_id,
                    packet_entities,
                    records)});
            track_packet_ack(client, packet_id, packet_ack_records);
            enforce_pending_packet_ack_limit(client);
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
                const std::size_t update_header_bits = server_update_header_bits(options_);
                const std::size_t destroy_bits =
                    destroy_record_bits(destroy.network_id, options_.network_entity_id_tier0_bits);
                const std::size_t next_packet_bits =
                    update_header_bits + records.bit_size() + destroy_bits;
                if (!records.empty() && protocol::bytes_for_bits(next_packet_bits) > options_.mtu_bytes) {
                    const std::size_t packet_bytes =
                        protocol::bytes_for_bits(update_header_bits + records.bit_size());
                    if (packet_bytes > remaining) {
                        break;
                    }
                    emit_records();
                    remaining -= packet_bytes;
                }

                const std::size_t packet_bytes =
                    protocol::bytes_for_bits(update_header_bits + records.bit_size() + destroy_bits);
                if (packet_bytes > options_.mtu_bytes || packet_bytes > remaining) {
                    continue;
                }

                destroy.frame = frame_;
                destroy.reset_epoch = client.epoch;
                records.push_bool(true);
                protocol::write_network_entity_id(
                    records,
                    destroy.network_id,
                    options_.network_entity_id_tier0_bits);
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
            write_entity_record(
                registry,
                settings,
                client,
                slot,
                quantized_frames_[serialized.quantized_frame],
                candidate.component_mask,
                serialized.payload);
            if (serialized.payload.empty()) {
                release_prepared_quantized_frame(serialized.quantized_frame);
                unsent.push_back(slot);
                continue;
            }

            const std::size_t next_packet_bits =
                server_update_header_bits(options_) + records.bit_size() + 1U + serialized.payload.bit_size();
            if (!records.empty() && protocol::bytes_for_bits(next_packet_bits) > options_.mtu_bytes) {
                const std::size_t packet_bytes =
                    protocol::bytes_for_bits(server_update_header_bits(options_) + records.bit_size());
                if (packet_bytes > remaining) {
                    release_prepared_quantized_frame(serialized.quantized_frame);
                    unsent.push_back(slot);
                    continue;
                }
                emit_records();
                remaining -= packet_bytes;
            }

            const std::size_t single_packet_bits =
                server_update_header_bits(options_) + records.bit_size() + 1U + serialized.payload.bit_size();
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
                protocol::bytes_for_bits(server_update_header_bits(options_) + records.bit_size());
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

void ReplicationServer::capture_queued_cues(const SyncSettings& settings) {
    if (!settings.cue_queue) {
        return;
    }

    std::vector<QueuedSyncCue> cues;
    {
        std::lock_guard<std::mutex> lock(settings.cue_queue->mutex);
        cues.swap(settings.cue_queue->cues);
    }

    for (const QueuedSyncCue& cue : cues) {
        const auto found = entity_to_slot_.find(cue.entity.value);
        if (found == entity_to_slot_.end()) {
            continue;
        }
        attach_cue_to_clients(found->second, cue);
    }
}

void ReplicationServer::attach_cue_to_clients(std::uint32_t slot, const QueuedSyncCue& cue) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }
    const SyncFrame relevance_frames = static_cast<SyncFrame>(
        std::ceil(static_cast<double>(cue.relevance_seconds) / options_.fixed_dt_seconds));
    const SyncFrame expire_frame = cue.frame + relevance_frames;
    for (ClientState& client : clients_) {
        if (slot >= client.entity_states.size()) {
            continue;
        }
        ClientEntityState& state = client.entity_states[slot];
        if (!state.priority_replicate) {
            continue;
        }
        state.pending_cues.push_back(ClientEntityState::PendingCue{
            cue.frame,
            expire_frame,
            cue.type,
            cue.relevance_seconds,
            cue.payload});
    }
}

void ReplicationServer::expire_pending_cues(ClientState& client, SyncFrame frame) {
    for (ClientEntityState& state : client.entity_states) {
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
    std::uint64_t component_mask,
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
    write_entity_record(
        registry,
        settings,
        client,
        slot,
        quantized_frames_[quantized_frame],
        component_mask,
        out.payload);
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
            if (ops.quantize == nullptr) {
                return invalid_quantized_frame_id;
            }
            ops.quantize(component_value, destination);
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
    state.network_id = 0;
    state.network_version = 0;
    state.priority = 0;
    state.component_mask = std::numeric_limits<std::uint64_t>::max();
    state.priority_frame = 0;
    state.priority_replicate = true;
    state.has_network_id = false;
    state.pending_cues.clear();
}

void ReplicationServer::acknowledge_cues(ClientEntityState& state, SyncFrame frame) {
    state.pending_cues.erase(
        std::remove_if(
            state.pending_cues.begin(),
            state.pending_cues.end(),
            [frame](const ClientEntityState::PendingCue& cue) {
                return cue.frame <= frame;
            }),
        state.pending_cues.end());
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

    const std::uint32_t network_id = found->network_id;
    client.pending_destroys.erase(found);
    free_network_id(client, network_id);
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

std::uint32_t ReplicationServer::allocate_packet_id(ClientState& client) {
    const std::size_t packet_id_bits = configured_packet_id_bits(options_);
    const std::uint32_t max_packet_id = protocol::packet_id_mask(packet_id_bits);
    if (client.next_packet_id == 0U || client.next_packet_id > max_packet_id) {
        client.next_packet_id = 1U;
    }

    const std::uint32_t packet_id = client.next_packet_id;
    client.next_packet_id = packet_id == max_packet_id ? 1U : packet_id + 1U;
    client.pending_packet_acks.erase(
        std::remove_if(
            client.pending_packet_acks.begin(),
            client.pending_packet_acks.end(),
            [packet_id](const PendingPacketAck& pending) {
                return pending.packet_id == packet_id;
            }),
        client.pending_packet_acks.end());
    return packet_id;
}

void ReplicationServer::enforce_pending_packet_ack_limit(ClientState& client) {
    while (client.pending_packet_acks.size() > options_.max_pending_packet_acks_per_client) {
        client.pending_packet_acks.erase(client.pending_packet_acks.begin());
    }
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
    ClientState& client,
    std::uint32_t slot,
    const QuantizedFrame& quantized_frame,
    std::uint64_t component_mask,
    BitBuffer& out) {
    const ClientEntityState& entity_state = client.entity_states[slot];
    bool delta = entity_state.baseline != invalid_quantized_frame_id &&
        entity_state.baseline < quantized_frames_.size() &&
        quantized_frames_[entity_state.baseline].active &&
        quantized_frames_[entity_state.baseline].archetype == quantized_frame.archetype;
    if (delta) {
        const QuantizedFrame& baseline = quantized_frames_[entity_state.baseline];
        delta = baseline.data.present_mask == quantized_frame.data.present_mask;
    }

    const std::uint32_t network_id = network_id_for_slot(client, slot);
    if (network_id == 0U) {
        return;
    }
    protocol::write_network_entity_id(out, network_id, options_.network_entity_id_tier0_bits);
    out.push_bool(!delta);
    if (!delta) {
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
            if (!frame_has_component(quantized_frame.data, component_index) ||
                (component_mask & (std::uint64_t{1} << component_index)) == 0U) {
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
            ops.serialize(previous, current, out);
        }
        const std::size_t cue_count = std::min(entity_state.pending_cues.size(), max_cues_per_entity_record);
        out.push_bool(cue_count != 0U);
        if (cue_count != 0U) {
            out.push_bits(static_cast<std::int64_t>(cue_count), 16U);
            for (std::size_t cue_index = 0; cue_index < cue_count; ++cue_index) {
                const ClientEntityState::PendingCue& cue = entity_state.pending_cues[cue_index];
                out.push_bits(cue.frame, 32U);
                out.push_bits(cue.type, 16U);
                out.push_bytes(reinterpret_cast<const char*>(&cue.relevance_seconds), sizeof(cue.relevance_seconds));
                out.push_bits(static_cast<std::int64_t>(cue.payload.bit_size()), 16U);
                out.push_buffer_bits(cue.payload);
            }
        }
        (void)registry;
        return;
    }

    std::uint16_t component_count = 0;
    std::uint64_t present_sync_slots = 0;
    if (has_tag_slot(archetype)) {
        ++component_count;
        present_sync_slots |= sync_slot_bit(0);
    }
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (frame_has_component(quantized_frame.data, component_index) &&
            (component_mask & (std::uint64_t{1} << component_index)) != 0U) {
            ++component_count;
            present_sync_slots |= sync_slot_bit(component_index + 1U);
        }
    }

    const std::size_t sync_slots = sync_slot_count(archetype);
    const std::size_t sync_slot_bits = protocol::bits_for_range(sync_slot_count(archetype));
    const std::size_t slot_list_bits = 16U + static_cast<std::size_t>(component_count) * sync_slot_bits;
    const bool use_presence_mask = sync_slots < slot_list_bits;
    out.push_bool(use_presence_mask);
    if (use_presence_mask) {
        out.push_unsigned_bits(present_sync_slots, sync_slots);
    } else {
        out.push_bits(static_cast<std::int64_t>(component_count), 16U);
    }

    if (has_tag_slot(archetype)) {
        if (!use_presence_mask) {
            out.push_bits(0, sync_slot_bits);
        }
        out.push_unsigned_bits(quantized_frame.data.tag_mask, archetype.tags.size());
    }
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (!frame_has_component(quantized_frame.data, component_index) ||
            (component_mask & (std::uint64_t{1} << component_index)) == 0U) {
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

        if (!use_presence_mask) {
            out.push_bits(static_cast<std::int64_t>(component_index + 1U), sync_slot_bits);
        }
        ops.serialize(nullptr, current, out);
    }

    const std::size_t cue_count = std::min(entity_state.pending_cues.size(), max_cues_per_entity_record);
    out.push_bool(cue_count != 0U);
    if (cue_count != 0U) {
        out.push_bits(static_cast<std::int64_t>(cue_count), 16U);
        for (std::size_t cue_index = 0; cue_index < cue_count; ++cue_index) {
            const ClientEntityState::PendingCue& cue = entity_state.pending_cues[cue_index];
            out.push_bits(cue.frame, 32U);
            out.push_bits(cue.type, 16U);
            out.push_bytes(reinterpret_cast<const char*>(&cue.relevance_seconds), sizeof(cue.relevance_seconds));
            out.push_bits(static_cast<std::int64_t>(cue.payload.bit_size()), 16U);
            out.push_buffer_bits(cue.payload);
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

    const std::uint32_t packet_id = allocate_packet_id(client);
    BitBuffer packet =
        make_server_packet(options_.mtu_bytes, configured_packet_id_bits(options_), frame, packet_id, entity_count, records);
    track_packet_ack(client, packet_id, ack_records);
    enforce_pending_packet_ack_limit(client);
    options_.transport(client.peer, packet);
}

void ReplicationServer::send_connect_response(ClientState& client) {
    if (!options_.transport) {
        return;
    }
    BitBuffer packet;
    packet.reserve_bytes(options_.mtu_bytes);
    packet.push_bits(protocol::server_connect_response_message, 8U);
    packet.push_bool(true);
    packet.push_unsigned_bits(client.id, 64U);
    options_.transport(client.peer, packet);
}

void ReplicationServer::send_pong(ClientId peer, std::uint32_t sequence, SyncFrame send_frame) {
    if (!options_.transport) {
        return;
    }
    BitBuffer packet;
    packet.reserve_bytes(options_.mtu_bytes);
    packet.push_bits(protocol::server_pong_message, 8U);
    packet.push_bits(sequence, 32U);
    packet.push_bits(send_frame, 32U);
    options_.transport(peer, packet);
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
                const std::uint32_t network_id = client.entity_states[found->second].network_id;
                const std::uint32_t network_version = client.entity_states[found->second].network_version;
                const bool has_network_id = client.entity_states[found->second].has_network_id;
                clear_client_entity_state(client.entity_states[found->second]);
                client.entity_states[found->second].network_id = network_id;
                client.entity_states[found->second].network_version = network_version;
                client.entity_states[found->second].has_network_id = has_network_id;
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
    if (!free_replicated_slots_.empty()) {
        const std::uint32_t slot = free_replicated_slots_.back();
        free_replicated_slots_.pop_back();
        replicated_[slot] =
            ReplicatedSlot{entity, archetype, {}, {}, invalid_quantized_frame_id, 0, false, true};
        return slot;
    }

    if (replicated_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::length_error("kage sync replicated slot space exhausted");
    }

    const std::uint32_t slot = static_cast<std::uint32_t>(replicated_.size());
    replicated_.push_back(
        ReplicatedSlot{entity, archetype, {}, {}, invalid_quantized_frame_id, 0, false, true});
    return slot;
}

std::uint32_t ReplicationServer::allocate_network_id(ClientState& client, std::uint32_t slot) {
    if (client.network_ids.empty()) {
        client.network_ids.push_back(ClientState::NetworkIdEntry{});
    }
    std::uint32_t network_id = 0;
    if (client.free_network_id != 0U) {
        network_id = client.free_network_id;
        ClientState::NetworkIdEntry& entry = client.network_ids[network_id];
        client.free_network_id = entry.slot_or_next_free;
        entry.slot_or_next_free = slot;
        entry.active = true;
        entry.pending_destroy = false;
    } else {
        if (client.network_ids.size() > max_client_local_wire_network_id) {
            throw std::length_error("kage sync client-local network id space exhausted");
        }
        network_id = static_cast<std::uint32_t>(client.network_ids.size());
        client.network_ids.push_back(ClientState::NetworkIdEntry{slot, 1U, true, false});
    }
    ClientEntityState& state = client.entity_states[slot];
    state.network_id = network_id;
    state.network_version = client.network_ids[network_id].version;
    state.has_network_id = true;
    return network_id;
}

void ReplicationServer::free_network_id(ClientState& client, std::uint32_t network_id) {
    if (network_id == 0U || network_id >= client.network_ids.size()) {
        return;
    }
    ClientState::NetworkIdEntry& entry = client.network_ids[network_id];
    if (!entry.pending_destroy) {
        return;
    }
    ++entry.version;
    if (entry.version == 0U) {
        entry.version = 1U;
    }
    entry.slot_or_next_free = client.free_network_id;
    entry.active = false;
    entry.pending_destroy = false;
    client.free_network_id = network_id;
}

std::uint32_t ReplicationServer::network_id_for_slot(ClientState& client, std::uint32_t slot) {
    if (slot >= client.entity_states.size()) {
        return 0U;
    }
    ClientEntityState& state = client.entity_states[slot];
    if (state.has_network_id) {
        return state.network_id;
    }
    return allocate_network_id(client, slot);
}

void ReplicationServer::deactivate_slot(std::uint32_t slot) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }

    const ecs::Entity entity = replicated_[slot].entity;
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
        std::uint32_t network_id = 0;
        std::uint32_t network_version = 0;
        if (slot < client.entity_states.size()) {
            network_id = client.entity_states[slot].network_id;
            network_version = client.entity_states[slot].network_version;
            clear_client_entity_state(client.entity_states[slot]);
        }
        if (network_id == 0U || network_id >= client.network_ids.size()) {
            continue;
        }
        ClientState::NetworkIdEntry& network = client.network_ids[network_id];
        network.active = false;
        network.pending_destroy = true;
        network.slot_or_next_free = invalid_slot_or_free;
        const auto found_destroy = std::find_if(
            client.pending_destroys.begin(),
            client.pending_destroys.end(),
            [entity](const ClientDestroyState& pending) {
                return pending.entity == entity;
            });
        if (found_destroy == client.pending_destroys.end()) {
            client.pending_destroys.push_back(ClientDestroyState{entity, frame_, 0, network_id, network_version});
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

void ReplicationServer::hide_slot_for_client(ClientState& client, std::uint32_t slot) {
    if (slot >= replicated_.size() || slot >= client.entity_states.size()) {
        return;
    }

    ClientEntityState& state = client.entity_states[slot];
    if (!state.has_network_id || state.network_id == 0U || state.network_id >= client.network_ids.size()) {
        return;
    }

    const ecs::Entity entity = replicated_[slot].entity;
    const std::uint32_t network_id = state.network_id;
    const std::uint32_t network_version = state.network_version;
    clear_client_entity_state(state);

    ClientState::NetworkIdEntry& network = client.network_ids[network_id];
    network.active = false;
    network.pending_destroy = true;
    network.slot_or_next_free = invalid_slot_or_free;

    const auto found_destroy = std::find_if(
        client.pending_destroys.begin(),
        client.pending_destroys.end(),
        [entity, network_id](const ClientDestroyState& pending) {
            return pending.entity == entity && pending.network_id == network_id;
        });
    if (found_destroy == client.pending_destroys.end()) {
        client.pending_destroys.push_back(ClientDestroyState{entity, frame_, 0, network_id, network_version});
    }
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
