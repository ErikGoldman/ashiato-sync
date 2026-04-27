#include "kage/sync/sync.hpp"

#include <raylib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
inline constexpr SocketHandle invalid_socket_handle = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
inline constexpr SocketHandle invalid_socket_handle = -1;
#endif

namespace {

constexpr kage::sync::ClientId client_id = 1;
constexpr std::uint16_t server_port = 37042;

struct BallPosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

}  // namespace

namespace kage::sync {

template <>
struct SyncComponentTraits<BallPosition> {
    using Quantized = BallPosition;

    static Quantized quantize(const BallPosition& value) {
        return value;
    }

    static BallPosition dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, BitBuffer& out) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(BitBuffer& in, const Quantized*, Quantized& out) {
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
        return true;
    }

    static Quantized interpolate(const Quantized& from, const Quantized& to, float alpha) {
        return Quantized{
            from.x + (to.x - from.x) * alpha,
            from.y + (to.y - from.y) * alpha,
            from.z + (to.z - from.z) * alpha,
        };
    }
};

}  // namespace kage::sync

namespace {

struct BallVisual {
    float radius = 0.25f;
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    std::uint8_t a = 255;
};

struct ServerBall {
    ecs::Entity entity;
    Vector3 velocity{};
    float age = 0.0f;
    float lifetime = 5.0f;
};

struct SyncSchema {
    kage::sync::SyncArchetypeId ball;
};

struct LinkSettings {
    float latency_ms = 0.0f;
    float loss_percent = 0.0f;
};

struct QueuedPacket {
    kage::sync::BitBuffer packet;
    sockaddr_in target{};
    double deliver_time = 0.0;
};

struct LinkSimulator {
    LinkSettings settings;
    std::deque<QueuedPacket> queued;
    std::mt19937 rng{0xC0FFEE};

    bool drops_packet() {
        if (settings.loss_percent <= 0.0f) {
            return false;
        }
        std::uniform_real_distribution<float> distribution(0.0f, 100.0f);
        return distribution(rng) < settings.loss_percent;
    }
};

struct SampleHistory {
    static constexpr std::size_t capacity = 180;
    std::array<float, capacity> values{};
    std::size_t next = 0;
    std::size_t count = 0;

    void push(float value) {
        values[next] = value;
        next = (next + 1U) % capacity;
        count = std::min(count + 1U, capacity);
    }

    float at(std::size_t index) const {
        const std::size_t begin = (next + capacity - count) % capacity;
        return values[(begin + index) % capacity];
    }

    float max_value() const {
        float result = 1.0f;
        for (std::size_t index = 0; index < count; ++index) {
            result = std::max(result, at(index));
        }
        return result;
    }
};

struct RuntimeStats {
    SampleHistory down_kbps;
    SampleHistory up_kbps;
    float down_bytes_window = 0.0f;
    float up_bytes_window = 0.0f;
    float current_down_kbps = 0.0f;
    float current_up_kbps = 0.0f;
    float sample_timer = 0.0f;
    int server_packets = 0;
    int client_packets = 0;
    int dropped_packets = 0;
    int client_entities = 0;
    kage::sync::SyncFrame frame = 0;
    kage::sync::SyncFrame client_frame = 0;
};

void close_socket(SocketHandle socket) {
    if (socket == invalid_socket_handle) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

void set_nonblocking(SocketHandle socket) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
#endif
}

SocketHandle make_udp_socket(std::uint16_t port) {
    SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket_handle) {
        throw std::runtime_error("failed to create UDP socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(socket);
        throw std::runtime_error("failed to bind UDP socket");
    }
    set_nonblocking(socket);
    return socket;
}

sockaddr_in loopback_address(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return address;
}

void send_packet(SocketHandle socket, const sockaddr_in& target, const kage::sync::BitBuffer& packet) {
    const auto* data = reinterpret_cast<const char*>(packet.data());
    sendto(socket, data, static_cast<int>(packet.byte_size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
}

void queue_packet(
    LinkSimulator& link,
    SocketHandle socket,
    const sockaddr_in& target,
    const kage::sync::BitBuffer& packet,
    RuntimeStats& stats,
    bool downstream) {
    if (link.drops_packet()) {
        ++stats.dropped_packets;
        return;
    }

    if (link.settings.latency_ms <= 0.0f) {
        send_packet(socket, target, packet);
        return;
    }

    link.queued.push_back(QueuedPacket{
        packet,
        target,
        GetTime() + static_cast<double>(link.settings.latency_ms) / 1000.0});
    if (downstream) {
        stats.down_bytes_window += static_cast<float>(packet.byte_size());
    } else {
        stats.up_bytes_window += static_cast<float>(packet.byte_size());
    }
}

void flush_link(LinkSimulator& link, SocketHandle socket) {
    const double now = GetTime();
    while (!link.queued.empty() && link.queued.front().deliver_time <= now) {
        send_packet(socket, link.queued.front().target, link.queued.front().packet);
        link.queued.pop_front();
    }
}

bool receive_packet(SocketHandle socket, kage::sync::BitBuffer& packet, sockaddr_in* sender = nullptr) {
    std::array<char, 2048> bytes{};
    sockaddr_in source{};
#ifdef _WIN32
    int source_size = sizeof(source);
#else
    socklen_t source_size = sizeof(source);
#endif
    const int received = recvfrom(
        socket,
        bytes.data(),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<sockaddr*>(&source),
        &source_size);
    if (received <= 0) {
        return false;
    }
    packet.clear();
    packet.push_bytes(bytes.data(), static_cast<std::size_t>(received));
    if (sender != nullptr) {
        *sender = source;
    }
    return true;
}

SyncSchema define_schema(ecs::Registry& registry, bool interpolate_position = false) {
    const ecs::Entity position = kage::sync::register_sync_component<BallPosition>(registry, "BallPosition");
    const ecs::Entity visual = kage::sync::register_sync_component<BallVisual>(registry, "BallVisual");
    return SyncSchema{kage::sync::define_archetype(
        registry,
        "Ball",
        {
            {position,
             kage::sync::ReplicationAudience::All,
             interpolate_position ? kage::sync::ComponentInterpolation::Interpolate
                                  : kage::sync::ComponentInterpolation::Step},
            {visual, kage::sync::ReplicationAudience::All},
        })};
}

void spawn_ball(ecs::Registry& registry, std::vector<ServerBall>& balls, kage::sync::SyncArchetypeId archetype, int index) {
    const ecs::Entity entity = registry.create();
    const float lane = static_cast<float>((index % 9) - 4);
    const float phase = static_cast<float>(index) * 0.73f;
    registry.add<BallPosition>(entity, BallPosition{lane * 0.75f, std::sin(phase) * 1.5f, std::cos(phase) * 1.5f});
    registry.add<BallVisual>(
        entity,
        BallVisual{
            0.18f + 0.06f * static_cast<float>(index % 4),
            static_cast<std::uint8_t>(80 + (index * 47) % 170),
            static_cast<std::uint8_t>(100 + (index * 83) % 140),
            static_cast<std::uint8_t>(120 + (index * 31) % 120),
            255});
    registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{archetype});
    balls.push_back(ServerBall{
        entity,
        Vector3{std::cos(phase) * 1.2f, std::sin(phase * 1.7f) * 0.8f, std::sin(phase) * 1.2f},
        0.0f,
        3.5f + static_cast<float>(index % 6) * 0.45f});
}

void update_server_world(ecs::Registry& registry, std::vector<ServerBall>& balls, SyncSchema schema, float dt, int& spawn_index) {
    static float spawn_timer = 0.0f;
    spawn_timer += dt;
    while (spawn_timer >= 0.18f && balls.size() < 96) {
        spawn_timer -= 0.18f;
        spawn_ball(registry, balls, schema.ball, spawn_index++);
    }

    for (ServerBall& ball : balls) {
        ball.age += dt;
        BallPosition& position = registry.write<BallPosition>(ball.entity);
        position.x += ball.velocity.x * dt;
        position.y += ball.velocity.y * dt;
        position.z += ball.velocity.z * dt;

        if (std::fabs(position.x) > 4.0f) {
            ball.velocity.x = -ball.velocity.x;
        }
        if (std::fabs(position.y) > 2.5f) {
            ball.velocity.y = -ball.velocity.y;
        }
        if (std::fabs(position.z) > 2.5f) {
            ball.velocity.z = -ball.velocity.z;
        }
    }

    balls.erase(
        std::remove_if(
            balls.begin(),
            balls.end(),
            [&](const ServerBall& ball) {
                if (ball.age < ball.lifetime) {
                    return false;
                }
                registry.destroy(ball.entity);
                return true;
            }),
        balls.end());
}

void send_hello(SocketHandle client_socket, const sockaddr_in& server_address) {
    kage::sync::BitBuffer hello;
    hello.push_bits(kage::sync::protocol::client_hello_message, 8U);
    hello.push_unsigned_bits(client_id, 64U);
    send_packet(client_socket, server_address, hello);
}

void update_hotkeys(LinkSettings& settings) {
    if (IsKeyPressed(KEY_RIGHT_BRACKET)) {
        settings.latency_ms = std::min(settings.latency_ms + 25.0f, 500.0f);
    }
    if (IsKeyPressed(KEY_LEFT_BRACKET)) {
        settings.latency_ms = std::max(settings.latency_ms - 25.0f, 0.0f);
    }
    if (IsKeyPressed(KEY_EQUAL)) {
        settings.loss_percent = std::min(settings.loss_percent + 2.5f, 50.0f);
    }
    if (IsKeyPressed(KEY_MINUS)) {
        settings.loss_percent = std::max(settings.loss_percent - 2.5f, 0.0f);
    }
}

void update_client_mode_hotkeys(
    kage::sync::ReplicationClient& client,
    kage::sync::ReplicationClientMode& client_mode,
    kage::sync::SyncFrame& buffer_frames) {
    if (IsKeyPressed(KEY_M)) {
        client_mode = client_mode == kage::sync::ReplicationClientMode::Snap
            ? kage::sync::ReplicationClientMode::BufferedInterpolation
            : kage::sync::ReplicationClientMode::Snap;
        client.set_client_mode(client_mode);
    }
    if (IsKeyPressed(KEY_ONE)) {
        client_mode = kage::sync::ReplicationClientMode::Snap;
        client.set_client_mode(client_mode);
    }
    if (IsKeyPressed(KEY_TWO)) {
        client_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
        client.set_client_mode(client_mode);
    }
    if (IsKeyPressed(KEY_COMMA) && buffer_frames > 0) {
        --buffer_frames;
        client.set_interpolation_buffer_frames(buffer_frames);
    }
    if (IsKeyPressed(KEY_PERIOD)) {
        const kage::sync::SyncFrame next = buffer_frames + 1U;
        if (client.set_interpolation_buffer_frames(next)) {
            buffer_frames = next;
        }
    }
}

void update_bandwidth_samples(RuntimeStats& stats, float dt) {
    stats.sample_timer += dt;
    if (stats.sample_timer < 0.25f) {
        return;
    }

    const float seconds = stats.sample_timer;
    stats.current_down_kbps = (stats.down_bytes_window * 8.0f) / (seconds * 1000.0f);
    stats.current_up_kbps = (stats.up_bytes_window * 8.0f) / (seconds * 1000.0f);
    stats.down_kbps.push(stats.current_down_kbps);
    stats.up_kbps.push(stats.current_up_kbps);
    stats.down_bytes_window = 0.0f;
    stats.up_bytes_window = 0.0f;
    stats.sample_timer = 0.0f;
}

void draw_graph(Rectangle bounds, const SampleHistory& history, Color color, float scale_max) {
    DrawRectangleRec(bounds, Color{24, 27, 32, 220});
    DrawRectangleLinesEx(bounds, 1.0f, Color{90, 96, 110, 255});
    if (history.count < 2) {
        return;
    }

    Vector2 previous{};
    for (std::size_t index = 0; index < history.count; ++index) {
        const float x = bounds.x + (static_cast<float>(index) / static_cast<float>(SampleHistory::capacity - 1U)) * bounds.width;
        const float normalized = std::min(history.at(index) / scale_max, 1.0f);
        const float y = bounds.y + bounds.height - normalized * bounds.height;
        const Vector2 point{x, y};
        if (index != 0) {
            DrawLineEx(previous, point, 2.0f, color);
        }
        previous = point;
    }
}

const char* client_mode_name(kage::sync::ReplicationClientMode mode) {
    switch (mode) {
    case kage::sync::ReplicationClientMode::Snap:
        return "snap";
    case kage::sync::ReplicationClientMode::BufferedInterpolation:
        return "buffered";
    }
    return "snap";
}

void draw_stats_overlay(
    const RuntimeStats& stats,
    const LinkSettings& link,
    kage::sync::ReplicationClientMode client_mode,
    kage::sync::SyncFrame buffer_frames) {
    const Rectangle panel{16.0f, 16.0f, 390.0f, 216.0f};
    DrawRectangleRec(panel, Color{16, 18, 22, 220});
    DrawRectangleLinesEx(panel, 1.0f, Color{90, 96, 110, 255});

    DrawText(
        TextFormat("UDP localhost %s client", client_mode_name(client_mode)),
        28,
        28,
        20,
        RAYWHITE);
    DrawText(TextFormat("frame %u   entities %d", stats.frame, stats.client_entities), 28, 56, 16, Color{215, 220, 230, 255});
    DrawText(
        TextFormat("latency %.0f ms  [ / ]     loss %.1f%%  - / =", link.latency_ms, link.loss_percent),
        28,
        78,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat(
            "bandwidth now %.1f down / %.1f up kbps",
            stats.current_down_kbps,
            stats.current_up_kbps),
        28,
        100,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat("packets down %d  up %d  dropped %d", stats.server_packets, stats.client_packets, stats.dropped_packets),
        28,
        120,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat("client frame %u   buffer %u   mode M/1/2  buf ,/.", stats.client_frame, buffer_frames),
        28,
        140,
        16,
        Color{215, 220, 230, 255});

    const float scale = std::max(stats.down_kbps.max_value(), stats.up_kbps.max_value());
    const Rectangle graph{28.0f, 164.0f, 354.0f, 46.0f};
    draw_graph(graph, stats.down_kbps, Color{76, 190, 255, 255}, scale);
    draw_graph(graph, stats.up_kbps, Color{255, 190, 76, 255}, scale);
    DrawText(TextFormat("scale %.1f kbps", scale), 260, 168, 14, Color{215, 220, 230, 255});
    DrawText("down", 32, 212, 14, Color{76, 190, 255, 255});
    DrawText("up", 82, 212, 14, Color{255, 190, 76, 255});
}

}  // namespace

int main(int argc, char** argv) {
    kage::sync::ReplicationClientMode client_mode = kage::sync::ReplicationClientMode::Snap;
    kage::sync::SyncFrame interpolation_buffer_frames = 2;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto require_value = [&]() -> std::string {
            if (index + 1 >= argc) {
                throw std::runtime_error("missing value for " + arg);
            }
            return argv[++index];
        };
        if (arg == "--client-mode") {
            const std::string value = require_value();
            if (value == "snap") {
                client_mode = kage::sync::ReplicationClientMode::Snap;
            } else if (value == "buffered-interpolation") {
                client_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
            } else {
                throw std::runtime_error("--client-mode must be snap or buffered-interpolation");
            }
        } else if (arg == "--interpolation-buffer-frames") {
            interpolation_buffer_frames = static_cast<kage::sync::SyncFrame>(std::stoul(require_value()));
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif

    ecs::Registry server_registry;
    kage::sync::configure_server(server_registry);
    const SyncSchema server_schema = define_schema(server_registry);

    ecs::Registry client_registry;
    kage::sync::configure_client(client_registry, client_id);
    define_schema(client_registry, true);

    SocketHandle server_socket = make_udp_socket(server_port);
    SocketHandle client_socket = make_udp_socket(0);
    const sockaddr_in server_address = loopback_address(server_port);
    sockaddr_in client_address{};
    bool client_connected = false;
    LinkSimulator downstream_link;
    LinkSimulator upstream_link;
    RuntimeStats stats;

    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 32 * 1024;
    server_options.mtu_bytes = 1200;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        if (client_connected) {
            ++stats.server_packets;
            if (downstream_link.settings.latency_ms <= 0.0f) {
                stats.down_bytes_window += static_cast<float>(packet.byte_size());
            }
            queue_packet(downstream_link, server_socket, client_address, packet, stats, true);
        }
    };
    kage::sync::ReplicationServer server(server_options);
    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        client_mode,
        interpolation_buffer_frames,
        64});

    InitWindow(1280, 720, "kage-sync localhost balls");
    SetTargetFPS(60);
    Camera3D camera{};
    camera.position = Vector3{0.0f, 5.5f, 9.0f};
    camera.target = Vector3{0.0f, 0.0f, 0.0f};
    camera.up = Vector3{0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    std::vector<ServerBall> balls;
    int spawn_index = 0;
    float server_accumulator = 0.0f;
    send_hello(client_socket, server_address);

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();
        server_accumulator += dt;
        update_hotkeys(downstream_link.settings);
        update_client_mode_hotkeys(client, client_mode, interpolation_buffer_frames);
        upstream_link.settings = downstream_link.settings;
        flush_link(downstream_link, server_socket);
        flush_link(upstream_link, client_socket);
        update_bandwidth_samples(stats, dt);

        kage::sync::BitBuffer received;
        sockaddr_in sender{};
        while (receive_packet(server_socket, received, &sender)) {
            kage::sync::BitBuffer read = received;
            const auto message = static_cast<std::uint8_t>(read.read_bits(8U));
            if (message == kage::sync::protocol::client_hello_message) {
                const auto id = static_cast<kage::sync::ClientId>(read.read_unsigned_bits(64U));
                if (id == client_id) {
                    client_address = sender;
                    client_connected = true;
                    server.add_client(client_id);
                }
            } else {
                server.process_packet(client_id, received);
            }
        }

        while (server_accumulator >= 1.0f / 30.0f) {
            server_accumulator -= 1.0f / 30.0f;
            ++stats.client_frame;
            update_server_world(server_registry, balls, server_schema, 1.0f / 30.0f, spawn_index);
            server.tick(server_registry);
        }

        while (receive_packet(client_socket, received)) {
            kage::sync::BitBuffer read = received;
            if (read.remaining_bits() >= 40U &&
                static_cast<std::uint8_t>(read.read_bits(8U)) == kage::sync::protocol::server_update_message) {
                stats.frame = static_cast<kage::sync::SyncFrame>(read.read_bits(32U));
            }
            client.receive(client_registry, received);
        }
        if (client_mode == kage::sync::ReplicationClientMode::BufferedInterpolation) {
            client.apply_frame(client_registry, stats.client_frame);
        }
        for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
            ++stats.client_packets;
            if (upstream_link.settings.latency_ms <= 0.0f) {
                stats.up_bytes_window += static_cast<float>(ack.byte_size());
            }
            queue_packet(upstream_link, client_socket, server_address, ack, stats, false);
        }

        int visible_entities = 0;
        client_registry.view<const BallPosition, const BallVisual>().each(
            [&](ecs::Entity, const BallPosition&, const BallVisual&) {
                ++visible_entities;
            });
        stats.client_entities = visible_entities;

        BeginDrawing();
        ClearBackground(Color{18, 20, 24, 255});
        BeginMode3D(camera);
        DrawGrid(12, 1.0f);
        client_registry.view<const BallPosition, const BallVisual>().each(
            [](ecs::Entity, const BallPosition& position, const BallVisual& visual) {
                DrawSphere(
                    Vector3{position.x, position.y, position.z},
                    visual.radius,
                    Color{visual.r, visual.g, visual.b, visual.a});
            });
        EndMode3D();
        draw_stats_overlay(stats, downstream_link.settings, client_mode, interpolation_buffer_frames);
        DrawFPS(28, 238);
        EndDrawing();
    }

    CloseWindow();
    close_socket(client_socket);
    close_socket(server_socket);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
