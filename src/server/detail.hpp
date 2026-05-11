#pragma once

#include "ashiato/sync/server.hpp"
#include "ashiato/sync/tracing.hpp"

#include <cstdint>
#include <limits>

namespace ashiato::sync::server_detail {

constexpr std::uint32_t invalid_replicated_index_or_free_network_id = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t max_pending_quantized_frames_per_entity = 64;
constexpr std::size_t max_cues_per_entity_record = 255;
constexpr float reference_priority_boost = std::numeric_limits<float>::max() / 2.0f;

#ifdef ASHIATO_SYNC_ENABLE_TRACING
SyncTraceEvent make_server_trace_event(SyncTraceEventType type, ClientId client, SyncFrame frame);
#endif

float boosted_candidate_priority(
    float age,
    float priority,
    bool reference_boost_pending) noexcept;

}  // namespace ashiato::sync::server_detail
