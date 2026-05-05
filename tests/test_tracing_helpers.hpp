#pragma once

#include "test_protocol.hpp"

#ifdef KAGE_SYNC_ENABLE_TRACING

#include <algorithm>
#include <string>
#include <vector>

namespace kage_sync_tests {

inline bool has_event(
    const std::vector<kage::sync::SyncTraceEvent>& events,
    kage::sync::SyncTraceEventType type) {
    return std::any_of(events.begin(), events.end(), [type](const kage::sync::SyncTraceEvent& event) {
        return event.type == type;
    });
}

inline bool has_cue_event(
    const std::vector<kage::sync::SyncTraceEvent>& events,
    kage::sync::SyncTraceEventType type,
    kage::sync::SyncCueTypeId cue_type) {
    return std::any_of(events.begin(), events.end(), [type, cue_type](const kage::sync::SyncTraceEvent& event) {
        return event.type == type && event.cue_type == cue_type;
    });
}

inline bool has_cue_event_data(
    const std::vector<kage::sync::SyncTraceEvent>& events,
    kage::sync::SyncTraceEventType type,
    const std::string& data) {
    return std::any_of(events.begin(), events.end(), [type, &data](const kage::sync::SyncTraceEvent& event) {
        return event.type == type && event.data.find(data) != std::string::npos;
    });
}

inline kage::sync::SyncArchetypeId define_networked_archetype(ecs::Registry& registry) {
    const ecs::Entity position =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    return kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position, kage::sync::ReplicationAudience::All}});
}

inline const kage::sync::KTraceFrameCell* find_trace_cell(
    const kage::sync::KTraceFrameRun& run,
    kage::sync::SyncFrame frame) {
    const auto found = std::find_if(run.frames.begin(), run.frames.end(), [frame](const kage::sync::KTraceFrameCell& cell) {
        return cell.frame == frame;
    });
    return found != run.frames.end() ? &*found : nullptr;
}

inline const kage::sync::KTraceFrameCell* find_trace_cell(
    const kage::sync::KTraceComponentRow& row,
    kage::sync::SyncFrame frame) {
    for (const kage::sync::KTraceFrameRun& run : row.runs) {
        if (const kage::sync::KTraceFrameCell* cell = find_trace_cell(run, frame)) {
            return cell;
        }
    }
    return nullptr;
}

}  // namespace kage_sync_tests

#endif
