#pragma once

#include "game/constants.hpp"

#include "ashiato/sync/sync.hpp"

#include "../../network_simulator.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace fps {

using ClientLinkSimulator = ashiato::sync::examples::NetworkSimulator<int>;

struct AppConfig {
    bool server = false;
    bool client = false;
    bool launcher = false;
    bool listen = false;
    bool trace_frame_data = true;
#ifdef ASHIATO_SYNC_TRACE_PACKET_LOGS
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
    double loss_percent = 0.0;
    double link_bandwidth_kbps = 0.0;
    double link_queue_kb = 64.0;
    bool dynamic_bandwidth = true;
    double bandwidth_limit_kbps = 64.0 * 1024.0 * 8.0 / 1000.0;
    double bandwidth_min_kbps = 512.0;
    double bandwidth_initial_kbps = 2048.0;
    double bandwidth_max_kbps = 8192.0;
    int bots = 0;
    int clients = 0;
};

AppConfig parse_args(int argc, char** argv);
void run_server(const AppConfig& config);
void run_client(const AppConfig& config);
void run_listen_server(const AppConfig& config);
void run_launcher(const AppConfig& config);

class RemoteClientLauncher {
public:
    explicit RemoteClientLauncher(const AppConfig& config);
    ~RemoteClientLauncher();

    RemoteClientLauncher(const RemoteClientLauncher&) = delete;
    RemoteClientLauncher& operator=(const RemoteClientLauncher&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef ASHIATO_SYNC_ENABLE_TRACING
ashiato::sync::TraceOptions make_trace_options(const AppConfig& config);
#endif

}  // namespace fps
