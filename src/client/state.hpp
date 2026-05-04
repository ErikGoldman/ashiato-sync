#pragma once

#include "kage/sync/client.hpp"

namespace kage::sync::client_detail {

struct EntityCue {
    SyncFrame frame = 0;
    SyncCueTypeId type = 0;
    float relevance_seconds = 0.0f;
    BitBuffer payload;
};

struct EntityPlayedCue {
    SyncFrame frame = 0;
    SyncCueTypeId type = 0;
    BitBuffer payload;
    bool confirmed = false;
    bool seen_in_resim = false;
};

struct EntityFrameBaseline {
    SyncFrame frame = 0;
    bool valid = false;
    QuantizedFrameData baseline;
};

struct EntityComponentError {
    ecs::Entity component;
    SyncComponentOps::QuantizedBytes bytes;
};

struct EntityBufferedFrame {
    SyncFrame frame = 0;
    bool valid = false;
    bool entity_present = false;
    SyncArchetypeId archetype;
    QuantizedFrameData baseline;
    std::vector<EntityCue> cues;
};

struct EntityState {
    ecs::Entity local;
    SyncArchetypeId archetype;
    ReplicationClientMode mode = ReplicationClientMode::Snap;
    SyncFrame frame = 0;
    bool entity_present = true;
    bool mode_selected = false;
    QuantizedFrameData baseline;
    std::vector<EntityFrameBaseline> history;
    std::size_t history_next = 0;

    std::vector<EntityBufferedFrame> buffered_frames;
    std::vector<EntityBufferedFrame> predicted_frames;
    std::vector<EntityPlayedCue> played_cues;
    std::vector<EntityCue> received_cues;
    std::vector<EntityCue> pending_predicted_cues;
    std::uint64_t applied_present_mask = 0;
    std::vector<EntityComponentError> snap_errors;
    ClientEntityNetworkId client_entity_network_id = invalid_client_entity_network_id;
    std::uint32_t wire_network_id = 0;
    std::size_t active_index = std::numeric_limits<std::size_t>::max();
    std::size_t buffered_index = std::numeric_limits<std::size_t>::max();
    std::size_t snap_error_index = std::numeric_limits<std::size_t>::max();
    std::size_t prediction_rollback_index = std::numeric_limits<std::size_t>::max();
    bool prediction_rollback_pending = false;
    SyncFrame prediction_rollback_frame = 0;
};

struct OriginalPredictionCapture {
    std::uint32_t entity_index = std::numeric_limits<std::uint32_t>::max();
    QuantizedFrameData baseline;
};

struct WireNetworkIdState {
    std::uint32_t version = 0;
    std::uint32_t entity_index = std::numeric_limits<std::uint32_t>::max();
    bool alive = false;
};

struct InputFrame {
    SyncFrame frame = 0;
    bool valid = false;
    std::vector<std::uint8_t> bytes;
};

struct PendingPing {
    SyncFrame frame = 0;
    std::uint16_t subframe = 0;
};

}  // namespace kage::sync::client_detail
