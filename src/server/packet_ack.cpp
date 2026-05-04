#include "kage/sync/server.hpp"

#include "server/packet.hpp"
#include "server/state.hpp"

#include <algorithm>

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
    const server_detail::ServerPacketIdAllocator allocator(options_.max_pending_packet_acks_per_client);
    const std::uint32_t packet_id = allocator.allocate(client.next_packet_id);
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

void ReplicationServer::track_packet_ack(
    ClientState& client,
    std::uint32_t packet_id,
    const std::vector<PacketAckRecord>& records) {
    if (records.empty()) {
        return;
    }
    client.pending_packet_acks.push_back(PendingPacketAck{packet_id, records});
}

}  // namespace kage::sync
