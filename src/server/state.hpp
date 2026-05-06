#pragma once

#include "kage/sync/server.hpp"
#include "server/bandwidth_controller.hpp"
#include "server/input_buffer.hpp"
#include "server/server_client_replicator.hpp"

namespace kage::sync {

struct ReplicationServer::ReplicatedSlot {
    ecs::Entity entity;
    SyncArchetypeId archetype;
    std::vector<std::uint32_t> quantized_frames;
    std::vector<std::uint64_t> component_dirty_generations;
    std::uint32_t same_frame_quantized_frame = invalid_quantized_frame_id;
    SyncFrame same_frame_quantized_frame_frame = 0;
    bool same_frame_cacheable = false;
    bool active = false;
};

struct ReplicationServer::QuantizedFrame {
    std::uint32_t slot = 0;
    SyncFrame frame = 0;
    SyncArchetypeId archetype;
    std::uint32_t ref_count = 0;
    bool active = false;
    QuantizedFrameData data;
    std::vector<std::uint64_t> dirty_generations;
};

struct ReplicationServer::ClientState {
    ClientId id = invalid_client_id;
    ClientId peer = invalid_client_id;
    bool local = false;
    bool ready_for_updates = true;
    double connect_resend_accumulator_seconds = 0.0;
    double idle_seconds = 0.0;
    std::unique_ptr<ServerClientReplicator> replication;
    server_detail::ServerInputBuffer input;
};

struct ReplicationServer::PendingInboundPacket {
    ClientId client = invalid_client_id;
    ecs::BitBuffer packet;
};

}  // namespace kage::sync
