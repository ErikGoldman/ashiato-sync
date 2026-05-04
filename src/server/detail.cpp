#include "server/detail.hpp"

namespace kage::sync::server_detail {

#ifdef KAGE_SYNC_ENABLE_TRACING
SyncTraceEvent make_server_trace_event(SyncTraceEventType type, ClientId client, SyncFrame frame) {
    SyncTraceEvent event;
    event.type = type;
    event.role = SyncTraceRole::Server;
    event.client = client;
    event.frame = frame;
    return event;
}
#endif

std::uint64_t boosted_candidate_priority(
    std::uint64_t age,
    std::uint64_t priority,
    bool reference_boost_pending) noexcept {
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
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

}  // namespace kage::sync::server_detail
