#pragma once

#include "kage/sync/server.hpp"
#include "kage/sync/tracing.hpp"

#include <cstdint>
#include <limits>

namespace kage::sync::server_detail {

constexpr std::uint32_t invalid_slot_or_free = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t max_pending_quantized_frames_per_entity = 64;
constexpr std::size_t max_cues_per_entity_record = 255;
constexpr std::uint64_t reference_priority_boost = std::numeric_limits<std::uint64_t>::max() / 2U;

#ifdef KAGE_SYNC_ENABLE_TRACING
SyncTraceEvent make_server_trace_event(SyncTraceEventType type, ClientId client, SyncFrame frame);
#endif

std::uint64_t boosted_candidate_priority(
    std::uint64_t age,
    std::uint64_t priority,
    bool reference_boost_pending) noexcept;

}  // namespace kage::sync::server_detail
