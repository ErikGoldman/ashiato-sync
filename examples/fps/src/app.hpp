#pragma once

#include "game/constants.hpp"

#include "kage/sync/simulated_link.hpp"
#include "kage/sync/sync.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace fps {

using ClientLinkSimulator = kage::sync::SimulatedLink<ecs::BitBuffer, int>;

struct AppConfig {
    bool server = false;
    bool client = false;
    bool launcher = false;
    bool trace_frame_data = true;
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    bool trace_packet_logs = false;
#endif
    std::string host = "127.0.0.1";
    std::string executable;
    std::string replay_dir = "fps_replay_frames";
    std::string trace_dir;
    std::uint16_t port = default_port;
    std::uint16_t replay_port = 0;
    double latency_ms = 100.0;
    double jitter_ms = 30.0;
    int bots = 0;
    int clients = 0;
};

AppConfig parse_args(int argc, char** argv);
void run_server(const AppConfig& config);
void run_client(const AppConfig& config);
void run_launcher(const AppConfig& config);

#ifdef KAGE_SYNC_ENABLE_TRACING
std::unique_ptr<kage::sync::KTraceDirectoryWriter> make_trace_writer(const AppConfig& config);
#endif

}  // namespace fps
