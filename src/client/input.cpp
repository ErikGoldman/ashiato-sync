#include "ashiato/sync/client.hpp"

#include "client/store/input_buffer.hpp"

#include <vector>

namespace ashiato::sync {

bool ReplicationClient::set_input_bytes(ashiato::Registry& registry, ashiato::Entity component, const void* input) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    return input_->set_latest(registry, settings, component, input);
}

bool ReplicationClient::record_input_frame(ashiato::Registry& registry, const SyncSettings& settings, SyncFrame frame) {
    client_detail::ClientInputRecord recorded;
    const bool ok = input_->record_frame(settings, options_.prediction.input_buffer_capacity_frames, frame, &recorded);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    if (ok && recorded.bytes != nullptr) {
        trace_input_components(registry, settings, recorded.frame, recorded.component, recorded.bytes);
    }
#else
    (void)registry;
#endif
    return ok;
}

bool ReplicationClient::fill_input_frames_through(
    ashiato::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame) {
    std::vector<client_detail::ClientInputRecord> recorded;
    const bool ok = input_->fill_frames_through(settings, options_.prediction.input_buffer_capacity_frames, frame, recorded);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    if (ok) {
        for (const client_detail::ClientInputRecord& input_record : recorded) {
            trace_input_components(registry, settings, input_record.frame, input_record.component, input_record.bytes);
        }
    }
#else
    (void)registry;
#endif
    return ok;
}

bool ReplicationClient::apply_input_frame(ashiato::Registry& registry, const SyncSettings& settings, SyncFrame frame) {
    return input_->apply_frame(registry, settings, frame);
}

}  // namespace ashiato::sync
