#include "app.hpp"

#ifdef KAGE_SYNC_ENABLE_TRACING

namespace fps {

kage::sync::TraceOptions make_trace_options(const AppConfig& config) {
    kage::sync::TraceOptions options;
    options.enabled = !config.trace_dir.empty();
    options.directory = config.trace_dir;
    options.frame_data = config.trace_frame_data;
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    options.packet_logs = config.trace_packet_logs;
#endif
    return options;
}

}  // namespace fps

#endif
