#include "kage/sync/server.hpp"

#include "server/packet.hpp"
#include "server/packet_ack_tracker.hpp"
#include "server/state.hpp"

#include <algorithm>
#include <vector>

namespace kage::sync {

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
    if (options_.bandwidth.enabled) {
        client.bandwidth.packet_acked(options_.bandwidth, found->sent_frame, frame_, found->charged_bytes);
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
                const bool stale = std::none_of(
                    pending_packet.records.begin(),
                    pending_packet.records.end(),
                    [&](const PacketAckRecord& record) {
                        return packet_ack_record_pending(client, record);
                    });
                if (stale && options_.bandwidth.enabled) {
                    client.bandwidth.packet_lost(options_.bandwidth, frame_, pending_packet.charged_bytes);
                }
                return stale;
            }),
        client.pending_packet_acks.end());
}

std::uint32_t ReplicationServer::allocate_packet_id(ClientState& client) {
    const server_detail::ServerPacketIdAllocator allocator(options_.protocol.max_pending_packet_acks_per_client);
    const std::uint32_t packet_id = allocator.allocate(client.next_packet_id);
    client.pending_packet_acks.erase(
        std::remove_if(
            client.pending_packet_acks.begin(),
            client.pending_packet_acks.end(),
            [&](const PendingPacketAck& pending) {
                if (pending.packet_id != packet_id) {
                    return false;
                }
                if (options_.bandwidth.enabled) {
                    client.bandwidth.packet_lost(options_.bandwidth, frame_, pending.charged_bytes);
                }
                return true;
            }),
        client.pending_packet_acks.end());
    return packet_id;
}

void ReplicationServer::enforce_pending_packet_ack_limit(ClientState& client) {
    const std::size_t max_pending = options_.protocol.max_pending_packet_acks_per_client;
    if (client.pending_packet_acks.size() <= max_pending) {
        return;
    }
    const std::size_t drop_count = client.pending_packet_acks.size() - max_pending;
    if (options_.bandwidth.enabled) {
        for (std::size_t index = 0; index < drop_count; ++index) {
            client.bandwidth.packet_lost(options_.bandwidth, frame_, client.pending_packet_acks[index].charged_bytes);
        }
    }
    client.pending_packet_acks.erase(
        client.pending_packet_acks.begin(),
        client.pending_packet_acks.begin() +
            static_cast<std::vector<PendingPacketAck>::difference_type>(drop_count));
}

std::size_t ReplicationServer::begin_client_bandwidth_tick(ClientState& client) {
    if (!options_.bandwidth.enabled) {
        return options_.bandwidth_limit_bytes_per_tick;
    }
    return client.bandwidth.begin_tick(options_.bandwidth, options_.fixed_dt_seconds);
}

std::size_t ReplicationServer::charged_packet_bytes(std::size_t payload_bytes) const noexcept {
    return payload_bytes + (options_.bandwidth.enabled ? options_.bandwidth.transport_overhead_bytes_per_packet : 0U);
}

void ReplicationServer::track_packet_ack(
    ClientState& client,
    std::uint32_t packet_id,
    SyncFrame sent_frame,
    std::size_t charged_bytes,
    const std::vector<PacketAckRecord>& records) {
    if (records.empty()) {
        return;
    }
    server_detail::track_packet_ack(client.pending_packet_acks, packet_id, sent_frame, charged_bytes, records);
    if (charged_bytes != 0U) {
        client.bandwidth.packet_sent(charged_bytes);
    }
}

}  // namespace kage::sync
