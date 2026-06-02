#include "app.hpp"

#include "game/components.hpp"
#include "game/cues.hpp"
#include "game/jobs.hpp"
#include "game/math.hpp"
#include "game/schema.hpp"
#include "net.hpp"
#include "raylib_frontend/listen_server_frontend.hpp"
#include "replay.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fps {

ashiato::sync::SyncFrame g_server_frame = 0;

struct ServerPlayer {
    ashiato::sync::ClientId client = ashiato::sync::invalid_client_id;
    ashiato::Entity entity;
};

void spawn_bots(ashiato::Registry& registry, const SyncSchema& schema, int count) {
    for (int i = 0; i < count; ++i) {
        const float angle = static_cast<float>(i) * 1.7f;
        const bool stun_bot = (i & 1) != 0;
        const ashiato::Entity bot = spawn_character(
            registry,
            schema,
            Vector3{std::sin(angle) * 4.5f, stun_bot ? stun_bot_hover_y : 0.0f, std::cos(angle) * 4.5f},
            stun_bot ? Color{110, 220, 255, 255} : Color{230, 170, 70, 255});
        if (stun_bot) {
            FpsVisual& visual = registry.write<FpsVisual>(bot);
            visual.radius = stun_bot_radius;
            visual.height = stun_bot_height;
            visual.style = 1;
            FpsVelocity& velocity = registry.write<FpsVelocity>(bot);
            velocity.grounded = 0;
        }
        registry.add<BotBrain>(
            bot,
            BotBrain{
                angle,
                random_spawn_position(),
                random_spawn_float(3.0f, 8.0f),
                static_cast<std::uint8_t>(stun_bot ? 1U : 0U)});
        registry.add<ashiato::sync::NoSimulate>(bot);
    }
}

ashiato::sync::ReplicationServerOptions make_fps_server_options(
    const AppConfig& config,
    std::unordered_map<ashiato::sync::PeerId, sockaddr_in>& peers,
    std::unordered_map<ashiato::sync::PeerId, ashiato::sync::examples::NetworkSimulator<sockaddr_in>>& downstream_links,
    double& link_time_seconds,
    std::vector<ashiato::sync::ClientId>& pending_spawns) {
    ashiato::sync::ReplicationServerOptions options;
    const auto bytes_per_second = [](double kbps) {
        return static_cast<std::size_t>(std::max(1.0, kbps * 1000.0 / 8.0));
    };
    options.bandwidth_limit_bytes_per_tick =
        static_cast<std::size_t>(std::max(1.0, config.bandwidth_limit_kbps * 1000.0 / 8.0 * fixed_dt));
    options.mtu_bytes = 1200;
    options.bandwidth.enabled = config.dynamic_bandwidth;
    options.bandwidth.min_bytes_per_second = bytes_per_second(config.bandwidth_min_kbps);
    options.bandwidth.max_bytes_per_second = bytes_per_second(config.bandwidth_max_kbps);
    options.bandwidth.initial_bytes_per_second = std::clamp(
        bytes_per_second(config.bandwidth_initial_kbps),
        options.bandwidth.min_bytes_per_second,
        options.bandwidth.max_bytes_per_second);
    options.bandwidth.max_burst_bytes = options.mtu_bytes * 4U;
    options.fixed_dt_seconds = fixed_dt;
    options.connect_handler = [&pending_spawns](const std::string&, ashiato::sync::ClientId& client, std::string&) {
        pending_spawns.push_back(client);
        return true;
    };
    const ashiato::sync::examples::NetworkSimulatorSettings link_settings{
        std::max(0.0, config.latency_ms),
        std::max(0.0, config.jitter_ms),
        std::clamp(config.loss_percent, 0.0, 100.0),
        std::max(0.0, config.link_bandwidth_kbps),
        static_cast<std::size_t>(std::max(0.0, config.link_queue_kb) * 1024.0)};
    options.transport = [&peers, &downstream_links, &link_time_seconds, link_settings](ashiato::sync::PeerId peer, const ashiato::BitBuffer& packet) {
        const auto found = peers.find(peer);
        if (found != peers.end()) {
            auto& link = downstream_links.try_emplace(peer, link_settings, static_cast<std::uint32_t>(peer)).first->second;
            (void)link.enqueue(found->second, packet, link_time_seconds);
        }
    };
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    options.trace = make_trace_options(config);
#else
    (void)config;
#endif
    return options;
}

void receive_server_packets(
    SocketHandle socket,
    std::unordered_map<ashiato::sync::PeerId, sockaddr_in>& peers,
    ashiato::sync::ReplicationServer& server) {
    ashiato::BitBuffer packet;
    sockaddr_in sender{};
    while (receive_packet(socket, packet, &sender)) {
        const ashiato::sync::PeerId peer = peer_id(sender);
        peers[peer] = sender;
        server.receive_packet(peer, packet);
    }
}

ashiato::Entity spawn_remote_player(
    ashiato::Registry& registry,
    const SyncSchema& schema,
    ashiato::sync::ClientId client) {
    const float phase = static_cast<float>(client) * 1.31f;
    return spawn_character(
        registry,
        schema,
        Vector3{std::sin(phase) * 3.0f, 0.0f, std::cos(phase) * 3.0f},
        Color{
            static_cast<unsigned char>(80 + (client * 53U) % 150U),
            static_cast<unsigned char>(120 + (client * 89U) % 110U),
            static_cast<unsigned char>(170 + (client * 37U) % 80U),
            255},
        client);
}

void spawn_pending_remote_players(
    ashiato::Registry& registry,
    const SyncSchema& schema,
    std::vector<ashiato::sync::ClientId>& pending_spawns,
    std::vector<ServerPlayer>* players = nullptr) {
    for (const ashiato::sync::ClientId client : pending_spawns) {
        const ashiato::Entity entity = spawn_remote_player(registry, schema, client);
        if (players != nullptr) {
            players->push_back(ServerPlayer{client, entity});
        }
        std::cout << "client " << client << " joined\n";
    }
    pending_spawns.clear();
}

void run_server_mode(const AppConfig& config, bool listen_mode) {
    ashiato::Registry registry;
    const SyncSchema schema = define_schema(registry);
    register_game_jobs(registry);
    FpsReplayRecorder replay_recorder(config.replay_dir);
    FpsReplayServer replay_server(replay_recorder, config.replay_port);

    SocketHandle socket = make_udp_socket(config.port);
    std::unordered_map<ashiato::sync::PeerId, sockaddr_in> peers;
    std::unordered_map<ashiato::sync::PeerId, ashiato::sync::examples::NetworkSimulator<sockaddr_in>> downstream_links;
    double link_time_seconds = 0.0;
    std::vector<ashiato::sync::ClientId> pending_spawns;
    std::vector<ServerPlayer> players;

    ashiato::sync::ReplicationServer server(
        registry,
        make_fps_server_options(config, peers, downstream_links, link_time_seconds, pending_spawns));
    replay_recorder.attach(server);
    replay_server.attach(server);

    std::unique_ptr<ListenServerFrontend> listen;
    if (listen_mode) {
        listen = std::make_unique<ListenServerFrontend>(config, registry, schema, server);
    }
    spawn_bots(registry, schema, config.bots);

    if (listen_mode) {
        std::cout << "ashiato_sync_fps_example listen server hosting on UDP " << config.port
                  << " as client " << listen->host_client() << '\n';
    } else {
        std::cout << "ashiato_sync_fps_example server listening on UDP " << config.port << '\n';
    }

    auto previous = std::chrono::steady_clock::now();
    while (!listen_mode || !listen->window_should_close()) {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - previous).count();
        previous = now;
        link_time_seconds += dt;

        receive_server_packets(socket, peers, server);

        if (listen_mode) {
            listen->update_input(registry, server);
        }

        (void)server.tick(registry, dt);
        for (auto& entry : downstream_links) {
            entry.second.deliver_ready(link_time_seconds, [socket](const sockaddr_in& target, const ashiato::BitBuffer& packet) {
                send_packet(socket, target, packet);
            });
        }
        g_server_frame = server.frame();

        if (listen_mode) {
            listen->capture_display(registry, server);
        }
        replay_server.tick(dt);

        spawn_pending_remote_players(registry, schema, pending_spawns, listen_mode ? nullptr : &players);

        if (listen_mode) {
            listen->update_effects(registry, static_cast<float>(dt));
            listen->render(registry, server);
        }
    }
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    server.close_trace();
#endif
    close_socket(socket);
}

void run_server(const AppConfig& config) {
    run_server_mode(config, false);
}

void run_listen_server(const AppConfig& config) {
    run_server_mode(config, true);
}

}  // namespace fps
