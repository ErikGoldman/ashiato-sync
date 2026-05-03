#include "app.hpp"

#ifdef KAGE_SYNC_ENABLE_TRACING

namespace fps {

std::unique_ptr<kage::sync::KTraceDirectoryWriter> make_trace_writer(const AppConfig& config) {
    if (config.trace_dir.empty()) {
        return nullptr;
    }
    auto writer = std::make_unique<kage::sync::KTraceDirectoryWriter>(
        kage::sync::KTraceDirectoryWriterOptions{config.trace_dir});
    writer->tracer().set_frame_data_enabled(config.trace_frame_data);
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    writer->tracer().set_packet_logs_enabled(config.trace_packet_logs);
#endif
    return writer;
}

}  // namespace fps

#endif
