#pragma once

#include "test_protocol.hpp"

#ifdef ASHIATO_SYNC_ENABLE_TRACING

#include <algorithm>
#include <string>
#include <vector>

namespace ashiato_sync_tests {

inline bool has_event(
    const std::vector<ashiato::sync::SyncTraceEvent>& events,
    ashiato::sync::SyncTraceEventType type) {
    return std::any_of(events.begin(), events.end(), [type](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == type;
    });
}

inline bool has_cue_event(
    const std::vector<ashiato::sync::SyncTraceEvent>& events,
    ashiato::sync::SyncTraceEventType type,
    ashiato::sync::SyncCueTypeId cue_type) {
    return std::any_of(events.begin(), events.end(), [type, cue_type](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == type && event.cue_type == cue_type;
    });
}

inline bool has_cue_event_data(
    const std::vector<ashiato::sync::SyncTraceEvent>& events,
    ashiato::sync::SyncTraceEventType type,
    const std::string& data) {
    return std::any_of(events.begin(), events.end(), [type, &data](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == type && event.data.find(data) != std::string::npos;
    });
}

inline ashiato::sync::SyncArchetypeId define_networked_archetype(ashiato::Registry& registry) {
    const ashiato::Entity position =
        ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    return ashiato::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position, ashiato::sync::ReplicationAudience::All}});
}

inline const ashiato::sync::KTraceFrameCell* find_trace_cell(
    const ashiato::sync::KTraceFrameRun& run,
    ashiato::sync::SyncFrame frame) {
    const auto found = std::find_if(run.frames.begin(), run.frames.end(), [frame](const ashiato::sync::KTraceFrameCell& cell) {
        return cell.frame == frame;
    });
    return found != run.frames.end() ? &*found : nullptr;
}

inline const ashiato::sync::KTraceFrameCell* find_trace_cell(
    const ashiato::sync::KTraceComponentRow& row,
    ashiato::sync::SyncFrame frame) {
    for (const ashiato::sync::KTraceFrameRun& run : row.runs) {
        if (const ashiato::sync::KTraceFrameCell* cell = find_trace_cell(run, frame)) {
            return cell;
        }
    }
    return nullptr;
}

}  // namespace ashiato_sync_tests

#endif
