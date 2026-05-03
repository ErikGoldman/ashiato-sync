#include "app.hpp"

#include <exception>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#define NOMMIDS
#include <winsock2.h>
#endif

int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        const fps::AppConfig config = fps::parse_args(argc, argv);
        if (config.server) {
            fps::run_server(config);
        } else if (config.client) {
            fps::run_client(config);
        } else {
            fps::run_launcher(config);
        }
#ifdef _WIN32
        WSACleanup();
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::cerr << "usage: kage_sync_fps_example --server [--port N] [--bots N] [--trace-dir DIR]\n"
                  << "       kage_sync_fps_example --client [--host A.B.C.D] [--port N] [--latency-ms N] [--jitter-ms N] [--trace-dir DIR]\n"
                  << "       kage_sync_fps_example --clients N [--host A.B.C.D] [--port N] [--bots N] [--latency-ms N] [--jitter-ms N] [--trace-dir DIR]\n"
                  << "       trace options: [--trace-frame-data on|off] [--trace-packet-logs on|off]\n";
        return 1;
    }
    return 0;
}
