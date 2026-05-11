#include "server/detail.hpp"

namespace ashiato::sync::server_detail {

#ifdef ASHIATO_SYNC_ENABLE_TRACING
SyncTraceEvent make_server_trace_event(SyncTraceEventType type, ClientId client, SyncFrame frame) {
    SyncTraceEvent event;
    event.type = type;
    event.role = SyncTraceRole::Server;
    event.client = client;
    event.frame = frame;
    return event;
}
#endif

float boosted_candidate_priority(
    float age,
    float priority,
    bool reference_boost_pending) noexcept {
    const float max = std::numeric_limits<float>::max();
    if (max - age < priority) {
        age = max;
    } else {
        age += priority;
    }
    if (reference_boost_pending) {
        if (max - age < reference_priority_boost) {
            age = max;
        } else {
            age += reference_priority_boost;
        }
    }
    return age;
}

}  // namespace ashiato::sync::server_detail
