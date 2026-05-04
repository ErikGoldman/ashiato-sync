#pragma once

#include "kage/sync/server.hpp"
#include "server/input_buffer.hpp"

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

struct ReplicationServer::ClientEntityState {
    struct PendingQuantizedFrame {
        std::uint32_t quantized_frame = invalid_quantized_frame_id;
        SyncFrame frame = 0;
    };

    struct PendingCue {
        SyncFrame frame = 0;
        SyncFrame expire_frame = 0;
        SyncCueTypeId type = 0;
        float relevance_seconds = 0.0f;
        ecs::BitBuffer payload;
    };

    std::uint32_t baseline = invalid_quantized_frame_id;
    std::uint32_t network_id = 0;
    std::uint32_t network_version = 0;
    std::uint64_t priority = 0;
    std::uint64_t component_mask = std::numeric_limits<std::uint64_t>::max();
    SyncFrame priority_frame = 0;
    bool priority_replicate = true;
    bool reference_priority_boost_pending = false;
    bool has_network_id = false;
    std::vector<PendingQuantizedFrame> pending;
    std::vector<PendingCue> pending_cues;
};

struct ReplicationServer::ClientDestroyState {
    ecs::Entity entity;
    SyncFrame frame = 0;
    std::uint64_t reset_epoch = 0;
    std::uint32_t network_id = 0;
    std::uint32_t network_version = 0;
};

struct ReplicationServer::PacketAckRecord {
    struct CueSummary {
        SyncFrame frame = 0;
        SyncCueTypeId type = 0;
        std::string data;
    };

    ecs::Entity entity;
    SyncFrame frame = 0;
    bool destroy = false;
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    std::vector<CueSummary> cues{};
#endif
};

struct ReplicationServer::PendingPacketAck {
    std::uint32_t packet_id = 0;
    std::vector<PacketAckRecord> records;
};

struct ReplicationServer::ClientState {
    ClientId id = invalid_client_id;
    ClientId peer = invalid_client_id;
    std::uint64_t epoch = 0;
    std::uint32_t next_packet_id = 1;
    bool ready_for_updates = true;
    double connect_resend_accumulator_seconds = 0.0;
    double idle_seconds = 0.0;
    std::vector<std::uint32_t> order;
    std::vector<std::uint64_t> reset_epochs;
    std::vector<ClientEntityState> entity_states;
    std::vector<ClientDestroyState> pending_destroys;
    std::vector<PendingPacketAck> pending_packet_acks;
    server_detail::ServerInputBuffer input;
    struct NetworkIdEntry {
        std::uint32_t slot_or_next_free = 0;
        std::uint32_t version = 0;
        bool active = false;
        bool pending_destroy = false;
    };
    std::vector<NetworkIdEntry> network_ids;
    std::uint32_t free_network_id = 0;
};

struct ReplicationServer::PendingInboundPacket {
    ClientId client = invalid_client_id;
    ecs::BitBuffer packet;
};

struct ReplicationServer::SerializedEntity {
    std::uint32_t quantized_frame = invalid_quantized_frame_id;
    ecs::BitBuffer payload;
};

struct ReplicationServer::SerializedCandidate {
    enum class Kind {
        Update,
        Destroy
    };

    Kind kind = Kind::Update;
    std::uint32_t slot = 0;
    std::size_t destroy_index = 0;
    std::uint64_t priority = 0;
    std::uint64_t component_mask = std::numeric_limits<std::uint64_t>::max();
};

}  // namespace kage::sync
