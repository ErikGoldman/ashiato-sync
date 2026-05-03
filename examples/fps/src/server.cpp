#include "app.hpp"

#include "game/components.hpp"
#include "game/jobs.hpp"
#include "game/math.hpp"
#include "game/schema.hpp"
#include "net.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <vector>

namespace fps {

kage::sync::SyncFrame g_server_frame = 0;

void run_server(const AppConfig& config) {
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const SyncSchema schema = define_schema(registry);
    register_game_jobs(registry);

    SocketHandle socket = make_udp_socket(config.port);
    std::unordered_map<kage::sync::ClientId, sockaddr_in> peers;
    std::vector<kage::sync::ClientId> pending_spawns;
    struct ServerPlayer {
        kage::sync::ClientId client = kage::sync::invalid_client_id;
        ecs::Entity entity;
    };
    std::vector<ServerPlayer> players;

    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 64 * 1024;
    options.mtu_bytes = 1200;
    options.fixed_dt_seconds = fixed_dt;
    options.connect_handler = [&pending_spawns](const std::string&, kage::sync::ClientId& client, std::string&) {
        pending_spawns.push_back(client);
        return true;
    };
    options.transport = [&peers, socket](kage::sync::ClientId peer, const kage::sync::BitBuffer& packet) {
        const auto found = peers.find(peer);
        if (found != peers.end()) {
            send_packet(socket, found->second, packet);
        }
    };
    kage::sync::ReplicationServer server(options);
#ifdef KAGE_SYNC_ENABLE_TRACING
    std::unique_ptr<kage::sync::KTraceDirectoryWriter> trace_writer = make_trace_writer(config);
    if (trace_writer != nullptr) {
        server.set_tracer(&trace_writer->tracer());
    }
#endif

    for (int i = 0; i < config.bots; ++i) {
        const float angle = static_cast<float>(i) * 1.7f;
        const ecs::Entity bot = spawn_character(
            registry,
            schema,
            Vector3{std::sin(angle) * 4.5f, 0.0f, std::cos(angle) * 4.5f},
            Color{230, 170, 70, 255});
        registry.add<BotBrain>(bot, BotBrain{angle, random_spawn_position(), random_spawn_float(3.0f, 8.0f)});
        registry.add<kage::sync::NoSimulate>(bot);
    }

    std::cout << "kage_sync_fps_example server listening on UDP " << config.port << '\n';
    auto previous = std::chrono::steady_clock::now();
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - previous).count();
        previous = now;

        kage::sync::BitBuffer packet;
        sockaddr_in sender{};
        while (receive_packet(socket, packet, &sender)) {
            const kage::sync::ClientId peer = peer_id(sender);
            peers[peer] = sender;
            server.receive_packet(peer, packet);
        }

        (void)server.tick(registry, dt);
        g_server_frame = server.frame();

        for (const kage::sync::ClientId client : pending_spawns) {
            const float phase = static_cast<float>(client) * 1.31f;
            const ecs::Entity entity = spawn_character(
                registry,
                schema,
                Vector3{std::sin(phase) * 3.0f, 0.0f, std::cos(phase) * 3.0f},
                Color{
                    static_cast<unsigned char>(80 + (client * 53U) % 150U),
                    static_cast<unsigned char>(120 + (client * 89U) % 110U),
                    static_cast<unsigned char>(170 + (client * 37U) % 80U),
                    255},
                client);
            players.push_back(ServerPlayer{client, entity});
            std::cout << "client " << client << " joined\n";
        }
        pending_spawns.clear();
    }
}

}  // namespace fps
