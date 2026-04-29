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
constexpr int min_ball_count = 0;
constexpr int max_ball_count = 512;

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
    using Error = BallPosition;

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

    static Error compute_error(const Quantized& current, const Quantized& previous) {
        return Error{
            previous.x - current.x,
            previous.y - current.y,
            previous.z - current.z,
        };
    }

    static Quantized apply_error(const Quantized& current, const Error& error) {
        return Quantized{
            current.x + error.x,
            current.y + error.y,
            current.z + error.z,
        };
    }

    static Error blend_out_error(const Error& error, float dt_seconds) {
        const float magnitude = std::sqrt(error.x * error.x + error.y * error.y + error.z * error.z);
        if (magnitude < 0.001f || dt_seconds <= 0.0f) {
            return magnitude < 0.001f ? Error{} : error;
        }

        const float rate = 2.0f + std::min(magnitude * 12.0f, 28.0f);
        const float scale = std::exp(-rate * dt_seconds);
        return Error{
            error.x * scale,
            error.y * scale,
            error.z * scale,
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

struct BallSpawnTagged {};
struct BallBounced {};

struct ServerBall {
    ecs::Entity entity;
    Vector3 velocity{};
    float age = 0.0f;
    float lifetime = 5.0f;
};

struct SyncSchema {
    kage::sync::SyncArchetypeId ball;
    ecs::Entity spawn_tagged;
    ecs::Entity bounced;
};

struct LinkSettings {
    float latency_ms = 0.0f;
    float jitter_ms = 0.0f;
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

    double delay_seconds() {
        float latency = std::max(0.0f, settings.latency_ms);
        if (settings.jitter_ms > 0.0f) {
            std::uniform_real_distribution<float> distribution(-settings.jitter_ms, settings.jitter_ms);
            latency = std::max(0.0f, latency + distribution(rng));
        }
        return static_cast<double>(latency) / 1000.0;
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
    int server_entities = 0;
    int client_entities = 0;
    kage::sync::SyncFrame frame = 0;
    kage::sync::SyncFrame receive_frame = 0;
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

    if (downstream) {
        stats.down_bytes_window += static_cast<float>(packet.byte_size());
    } else {
        stats.up_bytes_window += static_cast<float>(packet.byte_size());
    }

    const double delay = link.delay_seconds();
    if (delay <= 0.0) {
        send_packet(socket, target, packet);
        return;
    }

    link.queued.push_back(QueuedPacket{
        packet,
        target,
        GetTime() + delay});
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
    const ecs::Entity spawn_tagged = registry.register_component<BallSpawnTagged>("BallSpawnTagged");
    const ecs::Entity bounced = registry.register_component<BallBounced>("BallBounced");
    if (interpolate_position) {
        kage::sync::set_display_interpolated(registry, position);
    }
    return SyncSchema{
        kage::sync::define_archetype(
            registry,
            kage::sync::SyncArchetypeDesc{
                "Ball",
                {
                    {spawn_tagged, kage::sync::ReplicationAudience::All},
                    {bounced, kage::sync::ReplicationAudience::All},
                },
                {
                    {position,
                     kage::sync::ReplicationAudience::All,
                     interpolate_position ? kage::sync::ComponentInterpolation::Interpolate
                                          : kage::sync::ComponentInterpolation::Step},
                    {visual, kage::sync::ReplicationAudience::All},
                }}),
        spawn_tagged,
        bounced};
}

void spawn_ball(ecs::Registry& registry, std::vector<ServerBall>& balls, SyncSchema schema, int index) {
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
    if (((index * 1103515245U + 12345U) & 3U) == 0U) {
        registry.add_tag(entity, schema.spawn_tagged);
    }
    registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{schema.ball});
    balls.push_back(ServerBall{
        entity,
        Vector3{std::cos(phase) * 1.2f, std::sin(phase * 1.7f) * 0.8f, std::sin(phase) * 1.2f},
        0.0f,
        3.5f + static_cast<float>(index % 6) * 0.45f});
}

void update_server_world(
    ecs::Registry& registry,
    std::vector<ServerBall>& balls,
    SyncSchema schema,
    float dt,
    int& spawn_index,
    int target_ball_count) {
    const std::size_t target = static_cast<std::size_t>(std::clamp(target_ball_count, min_ball_count, max_ball_count));

    while (balls.size() > target) {
        registry.destroy(balls.back().entity);
        balls.pop_back();
    }

    int spawned = 0;
    while (balls.size() < target && spawned < 16) {
        spawn_ball(registry, balls, schema, spawn_index++);
        ++spawned;
    }

    for (ServerBall& ball : balls) {
        ball.age += dt;
        BallPosition& position = registry.write<BallPosition>(ball.entity);
        position.x += ball.velocity.x * dt;
        position.y += ball.velocity.y * dt;
        position.z += ball.velocity.z * dt;

        bool bounced = false;
        if (std::fabs(position.x) > 4.0f) {
            ball.velocity.x = -ball.velocity.x;
            bounced = true;
        }
        if (std::fabs(position.y) > 2.5f) {
            ball.velocity.y = -ball.velocity.y;
            bounced = true;
        }
        if (std::fabs(position.z) > 2.5f) {
            ball.velocity.z = -ball.velocity.z;
            bounced = true;
        }
        if (bounced && !registry.has(ball.entity, schema.bounced)) {
            registry.add_tag(ball.entity, schema.bounced);
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
    if (IsKeyPressed(KEY_APOSTROPHE)) {
        settings.jitter_ms = std::min(settings.jitter_ms + 10.0f, 500.0f);
    }
    if (IsKeyPressed(KEY_SEMICOLON)) {
        settings.jitter_ms = std::max(settings.jitter_ms - 10.0f, 0.0f);
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
    ecs::Registry& client_registry,
    const std::vector<ServerBall>& balls,
    kage::sync::ReplicationClientMode& client_mode) {
    auto set_mode = [&](kage::sync::ReplicationClientMode mode) {
        client_mode = mode;
        client.set_default_entity_mode(mode);
        for (const ServerBall& ball : balls) {
            client.set_entity_mode(client_registry, ball.entity, mode);
        }
    };

    if (IsKeyPressed(KEY_M)) {
        set_mode(
            client_mode == kage::sync::ReplicationClientMode::Snap
                ? kage::sync::ReplicationClientMode::BufferedInterpolation
                : kage::sync::ReplicationClientMode::Snap);
    }
    if (IsKeyPressed(KEY_ONE)) {
        set_mode(kage::sync::ReplicationClientMode::Snap);
    }
    if (IsKeyPressed(KEY_TWO)) {
        set_mode(kage::sync::ReplicationClientMode::BufferedInterpolation);
    }
}

void update_entity_count_hotkeys(int& target_ball_count) {
    const int small_step = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? 1 : 8;
    const int large_step = 32;
    if (IsKeyPressed(KEY_UP)) {
        target_ball_count = std::min(target_ball_count + small_step, max_ball_count);
    }
    if (IsKeyPressed(KEY_DOWN)) {
        target_ball_count = std::max(target_ball_count - small_step, min_ball_count);
    }
    if (IsKeyPressed(KEY_PAGE_UP)) {
        target_ball_count = std::min(target_ball_count + large_step, max_ball_count);
    }
    if (IsKeyPressed(KEY_PAGE_DOWN)) {
        target_ball_count = std::max(target_ball_count - large_step, min_ball_count);
    }
    if (IsKeyPressed(KEY_HOME)) {
        target_ball_count = max_ball_count;
    }
    if (IsKeyPressed(KEY_END)) {
        target_ball_count = min_ball_count;
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
    int target_ball_count,
    kage::sync::SyncFrame buffer_frames,
    const kage::sync::ReplicationClientTimingStats& timing) {
    const Rectangle panel{16.0f, 16.0f, 460.0f, 260.0f};
    DrawRectangleRec(panel, Color{16, 18, 22, 220});
    DrawRectangleLinesEx(panel, 1.0f, Color{90, 96, 110, 255});

    DrawText(
        TextFormat("UDP localhost %s client", client_mode_name(client_mode)),
        28,
        28,
        20,
        RAYWHITE);
    DrawText(
        TextFormat(
            "frame %u   target %d   server %d   client %d",
            stats.frame,
            target_ball_count,
            stats.server_entities,
            stats.client_entities),
        28,
        56,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat(
            "latency %.0f ms  [ / ]   jitter %.0f ms  ; / '",
            link.latency_ms,
            link.jitter_ms),
        28,
        78,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat("loss %.1f%%  - / =", link.loss_percent),
        28,
        100,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat("entities up/down +/-8   shift +/-1   pg +/-32"),
        28,
        122,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat(
            "bandwidth now %.1f down / %.1f up kbps",
            stats.current_down_kbps,
            stats.current_up_kbps),
        28,
        144,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat("packets down %d  up %d  dropped %d", stats.server_packets, stats.client_packets, stats.dropped_packets),
        28,
        164,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat(
            "net frame %u   client frame %u   buffer %u->%u   mode M/1/2",
            stats.receive_frame,
            stats.client_frame,
            buffer_frames,
            timing.target_interpolation_buffer_frames),
        28,
        184,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat(
            "measured %.1f frames latency  %.1f jitter  %.3fx",
            timing.latency_frames,
            timing.jitter_frames,
            timing.time_dilation),
        28,
        204,
        16,
        Color{215, 220, 230, 255});

    const float scale = std::max(stats.down_kbps.max_value(), stats.up_kbps.max_value());
    const Rectangle graph{28.0f, 226.0f, 404.0f, 34.0f};
    draw_graph(graph, stats.down_kbps, Color{76, 190, 255, 255}, scale);
    draw_graph(graph, stats.up_kbps, Color{255, 190, 76, 255}, scale);
    DrawText(TextFormat("scale %.1f kbps", scale), 310, 230, 14, Color{215, 220, 230, 255});
    DrawText("down", 32, 262, 14, Color{76, 190, 255, 255});
    DrawText("up", 82, 262, 14, Color{255, 190, 76, 255});
}

}  // namespace

int main(int argc, char** argv) {
    kage::sync::ReplicationClientMode client_mode = kage::sync::ReplicationClientMode::Snap;
    kage::sync::SyncFrame interpolation_buffer_frames = 2;
    bool auto_interpolation_buffer_frames = true;
    float time_dilation_min = 0.95f;
    float time_dilation_max = 1.05f;
    float time_dilation_gain = 0.05f;
    float initial_jitter_ms = 0.0f;
    int target_ball_count = 96;
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
        } else if (arg == "--jitter-ms") {
            initial_jitter_ms = std::stof(require_value());
        } else if (arg == "--entities") {
            target_ball_count = std::clamp(std::stoi(require_value()), min_ball_count, max_ball_count);
        } else if (arg == "--auto-interpolation-buffer") {
            const std::string value = require_value();
            if (value == "on" || value == "true" || value == "1") {
                auto_interpolation_buffer_frames = true;
            } else if (value == "off" || value == "false" || value == "0") {
                auto_interpolation_buffer_frames = false;
            } else {
                throw std::runtime_error("--auto-interpolation-buffer must be on or off");
            }
        } else if (arg == "--time-dilation-min") {
            time_dilation_min = std::stof(require_value());
        } else if (arg == "--time-dilation-max") {
            time_dilation_max = std::stof(require_value());
        } else if (arg == "--time-dilation-gain") {
            time_dilation_gain = std::stof(require_value());
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
    const SyncSchema client_schema = define_schema(client_registry, true);

    SocketHandle server_socket = make_udp_socket(server_port);
    SocketHandle client_socket = make_udp_socket(0);
    const sockaddr_in server_address = loopback_address(server_port);
    sockaddr_in client_address{};
    bool client_connected = false;
    LinkSimulator downstream_link;
    LinkSimulator upstream_link;
    downstream_link.settings.jitter_ms = initial_jitter_ms;
    RuntimeStats stats;

    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 32 * 1024;
    server_options.mtu_bytes = 1200;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        if (client_connected) {
            ++stats.server_packets;
            queue_packet(downstream_link, server_socket, client_address, packet, stats, true);
        }
    };
    kage::sync::ReplicationServer server(server_options);
    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        client_mode,
        interpolation_buffer_frames,
        64,
        auto_interpolation_buffer_frames,
        1,
        2.0f,
        0.1f,
        time_dilation_min,
        time_dilation_max,
        time_dilation_gain,
        {},
        1.0 / 30.0});

    InitWindow(1280, 720, "kage-sync localhost balls");
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
        client.tick(client_registry, dt);
        update_hotkeys(downstream_link.settings);
        update_entity_count_hotkeys(target_ball_count);
        update_client_mode_hotkeys(client, client_registry, balls, client_mode);
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
            update_server_world(
                server_registry,
                balls,
                server_schema,
                1.0f / 30.0f,
                spawn_index,
                target_ball_count);
            server.tick(server_registry);
        }
        stats.server_entities = static_cast<int>(balls.size());

        while (receive_packet(client_socket, received)) {
            kage::sync::BitBuffer read = received;
            if (read.remaining_bits() >= 40U &&
                static_cast<std::uint8_t>(read.read_bits(8U)) == kage::sync::protocol::server_update_message) {
                stats.frame = static_cast<kage::sync::SyncFrame>(read.read_bits(32U));
            }
            client.receive(client_registry, received);
            interpolation_buffer_frames = client.options().interpolation_buffer_frames;
        }
        for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
            ++stats.client_packets;
            queue_packet(upstream_link, client_socket, server_address, ack, stats, false);
        }
        stats.receive_frame = client.receive_frame();
        stats.client_frame = client.playback_frame();

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
        const float pulse_time = static_cast<float>(GetTime());
        for (const kage::sync::DisplayEntitySample& entity : client.display_frame(client_registry).entities) {
            BallPosition position;
            BallVisual visual;
            if (!entity.try_get(client_registry, position) || !entity.try_get(client_registry, visual)) {
                continue;
            }
            Color color{visual.r, visual.g, visual.b, visual.a};
            auto pulse_toward = [&](Color target, float phase) {
                const float pulse = 0.35f + 0.25f * (0.5f + 0.5f * std::sin(pulse_time * 5.0f + phase));
                auto blend = [&](std::uint8_t from, std::uint8_t to) {
                    const float value = static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * pulse;
                    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
                };
                color.r = blend(color.r, target.r);
                color.g = blend(color.g, target.g);
                color.b = blend(color.b, target.b);
            };
            if (entity.has_tag(client_registry, client_schema.spawn_tagged)) {
                pulse_toward(Color{255, 210, 74, visual.a}, 0.0f);
            }
            if (entity.has_tag(client_registry, client_schema.bounced)) {
                pulse_toward(Color{70, 220, 255, visual.a}, 2.1f);
            }
            DrawSphere(Vector3{position.x, position.y, position.z}, visual.radius, color);
        }
        EndMode3D();
        draw_stats_overlay(
            stats,
            downstream_link.settings,
            client_mode,
            target_ball_count,
            interpolation_buffer_frames,
            client.timing_stats());
        DrawFPS(28, 282);
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
