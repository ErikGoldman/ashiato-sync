#include "kage/sync/sync.hpp"

#include "network_simulator.hpp"

#include <raylib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
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
constexpr std::uint8_t example_client_hello_message = 250;
constexpr int min_ball_count = 0;
constexpr int max_ball_count = 512;
constexpr float ball_contact_skin = 0.18f;
constexpr float ball_contact_line_radius = 0.025f;
constexpr float ball_bounds_x = 4.0f;
constexpr float ball_bounds_y = 2.5f;
constexpr float ball_bounds_z = 2.5f;

struct BallPosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct BallVelocity {
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

    static void serialize(const Quantized*, const Quantized& current, ecs::BitBuffer& out) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(ecs::BitBuffer& in, const Quantized*, Quantized& out) {
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

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        TRACE_ROLLBACK_IF(std::fabs(predicted.x - authoritative.x) > 0.001f, "BallPosition.x");
        TRACE_ROLLBACK_IF(std::fabs(predicted.y - authoritative.y) > 0.001f, "BallPosition.y");
        TRACE_ROLLBACK_IF(std::fabs(predicted.z - authoritative.z) > 0.001f, "BallPosition.z");
        return false;
    }

#ifdef KAGE_SYNC_ENABLE_TRACING
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("x=");
        out.append_number(value.x);
        out.append(" y=");
        out.append_number(value.y);
        out.append(" z=");
        out.append_number(value.z);
    }
#endif
};

template <>
struct SyncComponentTraits<BallVelocity> {
    using Quantized = BallVelocity;

    static Quantized quantize(const BallVelocity& value) {
        return value;
    }

    static BallVelocity dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ecs::BitBuffer& out) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(ecs::BitBuffer& in, const Quantized*, Quantized& out) {
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
        return true;
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        TRACE_ROLLBACK_IF(std::fabs(predicted.x - authoritative.x) > 0.001f, "BallVelocity.x");
        TRACE_ROLLBACK_IF(std::fabs(predicted.y - authoritative.y) > 0.001f, "BallVelocity.y");
        TRACE_ROLLBACK_IF(std::fabs(predicted.z - authoritative.z) > 0.001f, "BallVelocity.z");
        return false;
    }

#ifdef KAGE_SYNC_ENABLE_TRACING
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("x=");
        out.append_number(value.x);
        out.append(" y=");
        out.append_number(value.y);
        out.append(" z=");
        out.append_number(value.z);
    }
#endif
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

struct BallBounceCue {
    std::uint32_t sequence = 0;
    float strength = 1.0f;
};

struct BallCueFlash {
    float seconds = 0.0f;
    float strength = 0.0f;
};

struct BallContact {
    kage::sync::EntityReference target{};
};

}  // namespace

namespace kage::sync {

template <>
struct SyncCueTraits<BallBounceCue> {
    static void serialize(const BallBounceCue& cue, ecs::BitBuffer& out) {
        out.push_bits(cue.sequence, 32U);
        out.push_bytes(reinterpret_cast<const char*>(&cue.strength), sizeof(cue.strength));
    }

    static bool deserialize(ecs::BitBuffer& in, BallBounceCue& out) {
        out.sequence = static_cast<std::uint32_t>(in.read_bits(32U));
        in.read_bytes(reinterpret_cast<char*>(&out.strength), sizeof(out.strength));
        return true;
    }

    static bool play(ecs::Registry& registry, ecs::Entity owner, const BallBounceCue& cue, float late_seconds) {
        const float duration = std::max(0.05f, 0.22f - late_seconds);
        registry.add<BallCueFlash>(owner, BallCueFlash{duration, cue.strength});
        return true;
    }

    static bool rollback(ecs::Registry& registry, ecs::Entity owner, const BallBounceCue&) {
        registry.remove<BallCueFlash>(owner);
        return true;
    }

    static bool equals_cue(const BallBounceCue& lhs, const BallBounceCue& rhs) {
        return lhs.sequence == rhs.sequence;
    }

#ifdef KAGE_SYNC_ENABLE_TRACING
    static void trace(const BallBounceCue& cue, SyncTraceStringBuilder& out) {
        out.append("sequence=");
        out.append_number(cue.sequence);
        out.append(" strength=");
        out.append_number(cue.strength);
    }
#endif
};

template <>
struct SyncComponentTraits<BallVisual> {
    using Quantized = BallVisual;

    static Quantized quantize(const BallVisual& value) {
        return value;
    }

    static BallVisual dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ecs::BitBuffer& out) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(ecs::BitBuffer& in, const Quantized*, Quantized& out) {
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
        return true;
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        TRACE_ROLLBACK_IF(predicted.radius != authoritative.radius, "BallVisual.radius");
        TRACE_ROLLBACK_IF(predicted.r != authoritative.r, "BallVisual.r");
        TRACE_ROLLBACK_IF(predicted.g != authoritative.g, "BallVisual.g");
        TRACE_ROLLBACK_IF(predicted.b != authoritative.b, "BallVisual.b");
        TRACE_ROLLBACK_IF(predicted.a != authoritative.a, "BallVisual.a");
        return false;
    }

#ifdef KAGE_SYNC_ENABLE_TRACING
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("radius=");
        out.append_number(value.radius);
        out.append(" rgba=");
        out.append_number(static_cast<int>(value.r));
        out.append(",");
        out.append_number(static_cast<int>(value.g));
        out.append(",");
        out.append_number(static_cast<int>(value.b));
        out.append(",");
        out.append_number(static_cast<int>(value.a));
    }
#endif
};

template <>
struct SyncComponentTraits<BallContact> {
    using Quantized = BallContact;

    static Quantized quantize(const BallContact& value) {
        return value;
    }

    static BallContact dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(
        const Quantized*,
        const Quantized& current,
        ecs::BitBuffer& out,
        EntityReferenceContext& references) {
        (void)write_entity_reference(out, current.target, references);
    }

    static bool deserialize(
        ecs::BitBuffer& in,
        const Quantized*,
        Quantized& out,
        EntityReferenceContext& references) {
        return read_entity_reference(in, references, out.target);
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        (void)predicted;
        (void)authoritative;
        return false;
    }
};

}  // namespace kage::sync

namespace {

struct BallSpawnTagged {};
struct BallBounced {};

struct ServerBall {
    ecs::Entity entity;
    Vector3 velocity{};
    float radius = 0.25f;
    float age = 0.0f;
    float lifetime = 5.0f;
};

struct SyncSchema {
    kage::sync::SyncArchetypeId ball;
    ecs::Entity spawn_tagged;
    ecs::Entity bounced;
};

using LinkSettings = kage::sync::examples::NetworkSimulatorSettings;
using LinkSimulator = kage::sync::examples::NetworkSimulator<sockaddr_in>;

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
    int rendered_entities = 0;
    int server_entities = 0;
    int client_entities = 0;
    kage::sync::SyncFrame frame = 0;
    kage::sync::SyncFrame receive_frame = 0;
    kage::sync::SyncFrame client_frame = 0;
};

struct RenderedBall {
    kage::sync::ClientEntityNetworkId network_id = kage::sync::invalid_client_entity_network_id;
    ecs::Entity local_entity;
    Vector3 position{};
    BallVisual visual{};
    BallContact contact{};
    bool spawn_tagged = false;
    bool bounced = false;
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

void send_packet(SocketHandle socket, const sockaddr_in& target, const ecs::BitBuffer& packet) {
    const auto* data = reinterpret_cast<const char*>(packet.data());
    sendto(socket, data, static_cast<int>(packet.byte_size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
}

void queue_packet(
    LinkSimulator& link,
    SocketHandle socket,
    const sockaddr_in& target,
    const ecs::BitBuffer& packet,
    RuntimeStats& stats,
    bool downstream) {
    const double now = GetTime();
    if (!link.enqueue(target, packet, now)) {
        ++stats.dropped_packets;
        return;
    }

    if (downstream) {
        stats.down_bytes_window += static_cast<float>(packet.byte_size());
    } else {
        stats.up_bytes_window += static_cast<float>(packet.byte_size());
    }

    link.deliver_ready(now, [&](const sockaddr_in& packet_target, const ecs::BitBuffer& queued_packet) {
        send_packet(socket, packet_target, queued_packet);
    });
}

void flush_link(LinkSimulator& link, SocketHandle socket) {
    const double now = GetTime();
    link.deliver_ready(now, [&](const sockaddr_in& target, const ecs::BitBuffer& packet) {
        send_packet(socket, target, packet);
    });
}

bool receive_packet(SocketHandle socket, ecs::BitBuffer& packet, sockaddr_in* sender = nullptr) {
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
    const ecs::Entity velocity = kage::sync::register_sync_component<BallVelocity>(registry, "BallVelocity");
    const ecs::Entity visual = kage::sync::register_sync_component<BallVisual>(registry, "BallVisual");
    const ecs::Entity contact = kage::sync::register_sync_component<BallContact>(registry, "BallContact");
    const ecs::Entity spawn_tagged = registry.register_component<BallSpawnTagged>("BallSpawnTagged");
    const ecs::Entity bounced = registry.register_component<BallBounced>("BallBounced");
    registry.register_component<BallCueFlash>("BallCueFlash");
    kage::sync::register_sync_cue<BallBounceCue>(registry);
    if (interpolate_position) {
        kage::sync::set_fractional_tick_sampled(registry, position);
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
                    {velocity, kage::sync::ReplicationAudience::All},
                    {visual, kage::sync::ReplicationAudience::All},
                    {contact, kage::sync::ReplicationAudience::All},
                }}),
        spawn_tagged,
        bounced};
}

void register_client_prediction_jobs(ecs::Registry& registry, kage::sync::ReplicationClient& client) {
    client.simulation_job<BallPosition, BallVelocity>(registry, 0).each(
        [](ecs::Entity, BallPosition& position, BallVelocity& velocity) {
            constexpr float fixed_dt = 1.0f / 30.0f;
            position.x += velocity.x * fixed_dt;
            position.y += velocity.y * fixed_dt;
            position.z += velocity.z * fixed_dt;

            if (std::fabs(position.x) > ball_bounds_x) {
                velocity.x = -velocity.x;
            }
            if (std::fabs(position.y) > ball_bounds_y) {
                velocity.y = -velocity.y;
            }
            if (std::fabs(position.z) > ball_bounds_z) {
                velocity.z = -velocity.z;
            }
        });
}

void spawn_ball(ecs::Registry& registry, std::vector<ServerBall>& balls, SyncSchema schema, int index) {
    const ecs::Entity entity = registry.create();
    const float lane = static_cast<float>((index % 9) - 4);
    const float phase = static_cast<float>(index) * 0.73f;
    registry.add<BallPosition>(entity, BallPosition{lane * 0.75f, std::sin(phase) * 1.5f, std::cos(phase) * 1.5f});
    const Vector3 velocity{std::cos(phase) * 1.2f, std::sin(phase * 1.7f) * 0.8f, std::sin(phase) * 1.2f};
    registry.add<BallVelocity>(entity, BallVelocity{velocity.x, velocity.y, velocity.z});
    const float radius = 0.18f + 0.06f * static_cast<float>(index % 4);
    registry.add<BallVisual>(
        entity,
        BallVisual{
            radius,
            static_cast<std::uint8_t>(80 + (index * 47) % 170),
            static_cast<std::uint8_t>(100 + (index * 83) % 140),
            static_cast<std::uint8_t>(120 + (index * 31) % 120),
            255});
    registry.add<BallContact>(entity, BallContact{});
    if (((index * 1103515245U + 12345U) & 3U) == 0U) {
        registry.add_tag(entity, schema.spawn_tagged);
    }
    registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{schema.ball});
    balls.push_back(ServerBall{
        entity,
        velocity,
        radius,
        0.0f,
        3.5f + static_cast<float>(index % 6) * 0.45f});
}

void update_ball_contacts(ecs::Registry& registry, std::vector<ServerBall>& balls) {
    constexpr float grid_min_x = -4.5f;
    constexpr float grid_min_y = -3.0f;
    constexpr float grid_min_z = -3.0f;
    constexpr float inverse_cell_size = 1.0f / 0.75f;
    constexpr int grid_x = 12;
    constexpr int grid_y = 8;
    constexpr int grid_z = 8;
    constexpr int cell_count = grid_x * grid_y * grid_z;

    std::array<int, cell_count> heads{};
    std::array<int, max_ball_count> next{};
    std::array<std::uint8_t, max_ball_count> assigned{};
    std::array<BallPosition, max_ball_count> positions{};
    heads.fill(-1);

    auto cell_axis = [](float value, float min_value, int cells) {
        const int cell = static_cast<int>((value - min_value) * inverse_cell_size);
        return std::clamp(cell, 0, cells - 1);
    };
    auto cell_index = [&](const BallPosition& position) {
        const int x = cell_axis(position.x, grid_min_x, grid_x);
        const int y = cell_axis(position.y, grid_min_y, grid_y);
        const int z = cell_axis(position.z, grid_min_z, grid_z);
        return x + y * grid_x + z * grid_x * grid_y;
    };
    auto cell_index_from_axes = [](int x, int y, int z) {
        return x + y * grid_x + z * grid_x * grid_y;
    };

    const std::size_t count = std::min(balls.size(), static_cast<std::size_t>(max_ball_count));
    for (std::size_t index = 0; index < count; ++index) {
        positions[index] = registry.get<BallPosition>(balls[index].entity);
        registry.write<BallContact>(balls[index].entity) = BallContact{};

        const int cell = cell_index(positions[index]);
        next[index] = heads[cell];
        heads[cell] = static_cast<int>(index);
    }

    for (std::size_t index = 0; index < count; ++index) {
        if (assigned[index] != 0U) {
            continue;
        }

        const BallPosition& position = positions[index];
        const int center_x = cell_axis(position.x, grid_min_x, grid_x);
        const int center_y = cell_axis(position.y, grid_min_y, grid_y);
        const int center_z = cell_axis(position.z, grid_min_z, grid_z);

        const int min_z = std::max(center_z - 1, 0);
        const int max_z = std::min(center_z + 1, grid_z - 1);
        const int min_y = std::max(center_y - 1, 0);
        const int max_y = std::min(center_y + 1, grid_y - 1);
        const int min_x = std::max(center_x - 1, 0);
        const int max_x = std::min(center_x + 1, grid_x - 1);

        for (int z = min_z; z <= max_z && assigned[index] == 0U; ++z) {
            for (int y = min_y; y <= max_y && assigned[index] == 0U; ++y) {
                for (int x = min_x; x <= max_x && assigned[index] == 0U; ++x) {
                    for (int other = heads[cell_index_from_axes(x, y, z)]; other >= 0; other = next[other]) {
                        const std::size_t other_index = static_cast<std::size_t>(other);
                        if (other_index <= index || assigned[other_index] != 0U) {
                            continue;
                        }

                        const BallPosition& other_position = positions[other_index];
                        const float dx = position.x - other_position.x;
                        const float dy = position.y - other_position.y;
                        const float dz = position.z - other_position.z;
                        const float radius = balls[index].radius + balls[other_index].radius + ball_contact_skin;
                        if (dx * dx + dy * dy + dz * dz > radius * radius) {
                            continue;
                        }

                        registry.write<BallContact>(balls[index].entity) =
                            BallContact{kage::sync::EntityReference{balls[other_index].entity}};
                        registry.write<BallContact>(balls[other_index].entity) =
                            BallContact{kage::sync::EntityReference{balls[index].entity}};
                        assigned[index] = 1U;
                        assigned[other_index] = 1U;
                        break;
                    }
                }
            }
        }
    }
}

void update_server_world(
    ecs::Registry& registry,
    std::vector<ServerBall>& balls,
    SyncSchema schema,
    kage::sync::SyncFrame frame,
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
        if (std::fabs(position.x) > ball_bounds_x) {
            ball.velocity.x = -ball.velocity.x;
            bounced = true;
        }
        if (std::fabs(position.y) > ball_bounds_y) {
            ball.velocity.y = -ball.velocity.y;
            bounced = true;
        }
        if (std::fabs(position.z) > ball_bounds_z) {
            ball.velocity.z = -ball.velocity.z;
            bounced = true;
        }
        if (bounced) {
            registry.write<BallVelocity>(ball.entity) =
                BallVelocity{ball.velocity.x, ball.velocity.y, ball.velocity.z};
            if (!registry.has(ball.entity, schema.bounced)) {
                registry.add_tag(ball.entity, schema.bounced);
            }
            static std::uint32_t bounce_sequence = 1;
            (void)kage::sync::emit_cue(
                registry,
                ball.entity,
                frame,
                BallBounceCue{bounce_sequence++, 1.0f},
                0.35f);
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

    update_ball_contacts(registry, balls);
}

void send_hello(SocketHandle client_socket, const sockaddr_in& server_address) {
    ecs::BitBuffer hello;
    hello.push_bits(example_client_hello_message, 8U);
    hello.push_unsigned_bits(client_id, 64U);
    send_packet(client_socket, server_address, hello);
}

void update_hotkeys(LinkSettings& settings) {
    if (IsKeyPressed(KEY_RIGHT_BRACKET)) {
        settings.latency_ms = std::min(settings.latency_ms + 25.0, 500.0);
    }
    if (IsKeyPressed(KEY_LEFT_BRACKET)) {
        settings.latency_ms = std::max(settings.latency_ms - 25.0, 0.0);
    }
    if (IsKeyPressed(KEY_APOSTROPHE)) {
        settings.jitter_ms = std::min(settings.jitter_ms + 10.0, 500.0);
    }
    if (IsKeyPressed(KEY_SEMICOLON)) {
        settings.jitter_ms = std::max(settings.jitter_ms - 10.0, 0.0);
    }
    if (IsKeyPressed(KEY_EQUAL)) {
        settings.loss_percent = std::min(settings.loss_percent + 2.5, 50.0);
    }
    if (IsKeyPressed(KEY_MINUS)) {
        settings.loss_percent = std::max(settings.loss_percent - 2.5, 0.0);
    }
}

void update_client_mode_hotkeys(
    kage::sync::ReplicationClient& client,
    ecs::Registry& client_registry,
    std::vector<kage::sync::ClientEntityNetworkId>& known_entities,
    kage::sync::ReplicationClientMode& client_mode) {
    auto set_mode = [&](kage::sync::ReplicationClientMode mode) {
        client_mode = mode;
        (void)client.set_default_entity_mode(mode);
        known_entities.erase(
            std::remove_if(
                known_entities.begin(),
                known_entities.end(),
                [&](kage::sync::ClientEntityNetworkId network_id) {
                    try {
                        client.set_entity_mode(client_registry, network_id, mode);
                        return false;
                    } catch (const kage::sync::ClientError& error) {
                        if (error.status() == kage::sync::ClientStatus::EntityNotFound ||
                            error.status() == kage::sync::ClientStatus::EntityUnavailable) {
                            return true;
                        }
                        throw;
                    }
                }),
            known_entities.end());
    };

    if (IsKeyPressed(KEY_M)) {
        if (client_mode == kage::sync::ReplicationClientMode::Snap) {
            set_mode(kage::sync::ReplicationClientMode::BufferedInterpolation);
        } else if (client_mode == kage::sync::ReplicationClientMode::BufferedInterpolation) {
            set_mode(kage::sync::ReplicationClientMode::Predict);
        } else {
            set_mode(kage::sync::ReplicationClientMode::Snap);
        }
    }
    if (IsKeyPressed(KEY_ONE)) {
        set_mode(kage::sync::ReplicationClientMode::Snap);
    }
    if (IsKeyPressed(KEY_TWO)) {
        set_mode(kage::sync::ReplicationClientMode::BufferedInterpolation);
    }
    if (IsKeyPressed(KEY_THREE)) {
        set_mode(kage::sync::ReplicationClientMode::Predict);
    }
}

void remember_client_entity(
    std::vector<kage::sync::ClientEntityNetworkId>& known_entities,
    kage::sync::ClientEntityNetworkId network_id) {
    if (network_id == kage::sync::invalid_client_entity_network_id) {
        return;
    }
    if (std::find(known_entities.begin(), known_entities.end(), network_id) == known_entities.end()) {
        known_entities.push_back(network_id);
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

kage::sync::ReplicationPrioritizerFn make_sphere_prioritizer(ecs::Registry& registry) {
    static constexpr float inner_filter_radius_sq = 0.75f * 0.75f;
    static constexpr float priority_radius_sq = 12.0f * 12.0f;
    static constexpr std::uint64_t priority_scale = 1000;

    return [&registry](
               kage::sync::ClientId,
               const std::vector<kage::sync::ReplicationPriorityObject>& objects,
               std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        decisions.resize(objects.size());
        for (std::size_t index = 0; index < objects.size(); ++index) {
            kage::sync::ReplicationPriorityDecision& decision = decisions[index];
            decision.replicate = true;
            decision.component_mask = std::numeric_limits<std::uint64_t>::max();

            const BallPosition* position = registry.try_get<BallPosition>(objects[index].entity);
            if (position == nullptr) {
                decision.priority = 0;
                continue;
            }

            const float distance_sq =
                position->x * position->x + position->y * position->y + position->z * position->z;
            if (distance_sq <= inner_filter_radius_sq) {
                decision.replicate = false;
                decision.priority = 0;
                continue;
            }

            const float clamped_distance_sq = distance_sq < priority_radius_sq ? distance_sq : priority_radius_sq;
            const float normalized = (priority_radius_sq - clamped_distance_sq) /
                (priority_radius_sq - inner_filter_radius_sq);
            decision.priority = 1U + static_cast<std::uint64_t>(normalized * static_cast<float>(priority_scale));
        }
    };
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

void draw_contact_link(Vector3 from, Vector3 to) {
    const float dx = from.x - to.x;
    const float dy = from.y - to.y;
    const float dz = from.z - to.z;
    if (dx * dx + dy * dy + dz * dz < 0.0001f) {
        return;
    }
    DrawCylinderEx(from, to, ball_contact_line_radius, ball_contact_line_radius, 8, Color{255, 245, 90, 230});
}

const char* client_mode_name(kage::sync::ReplicationClientMode mode) {
    switch (mode) {
    case kage::sync::ReplicationClientMode::Snap:
        return "snap";
    case kage::sync::ReplicationClientMode::BufferedInterpolation:
        return "buffered";
    case kage::sync::ReplicationClientMode::Predict:
        return "predict";
    }
    return "snap";
}

void draw_stats_overlay(
    const RuntimeStats& stats,
    const LinkSettings& link,
    kage::sync::ReplicationClientMode client_mode,
    int target_ball_count,
    kage::sync::SyncFrame buffer_frames,
    const kage::sync::ReplicationServer::ClientBandwidthStats& bandwidth,
    const kage::sync::ReplicationClientTimingStats& timing
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    ,
    const kage::sync::ReplicationClientInterpolationDiagnostics& interpolation_diagnostics
#endif
) {
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    const Rectangle panel{16.0f, 16.0f, 560.0f, 306.0f};
#else
    const Rectangle panel{16.0f, 16.0f, 560.0f, 280.0f};
#endif
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
        TextFormat("rendered %d", stats.rendered_entities),
        430,
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
        TextFormat("loss %.1f%%  - / =   link %.0f kbps queue %.0f KB",
            link.loss_percent,
            link.bandwidth_kbps,
            static_cast<double>(link.max_queue_bytes) / 1024.0),
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
        TextFormat(
            "%s target %.0f kbps  avail %.0f KB  inflight %.0f KB  loss %.1f%%",
            bandwidth.dynamic ? "dynamic" : "static",
            bandwidth.target_bytes_per_second * 8.0 / 1000.0,
            bandwidth.available_bytes / 1024.0,
            static_cast<double>(bandwidth.in_flight_bytes) / 1024.0,
            bandwidth.loss_rate * 100.0f),
        28,
        164,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat("packets down %d  up %d  dropped %d", stats.server_packets, stats.client_packets, stats.dropped_packets),
        28,
        184,
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
        204,
        16,
        Color{215, 220, 230, 255});
    DrawText(
        TextFormat(
            "measured %.1f frames latency  %.1f jitter  %.3fx",
            timing.latency_frames,
            timing.jitter_frames,
            timing.time_dilation),
        28,
        224,
        16,
        Color{215, 220, 230, 255});

#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    DrawText(
        TextFormat(
            "recent interpolation starvation %.2f%%  %llu / %llu",
            interpolation_diagnostics.interpolated_entity_starvation_percent(),
            static_cast<unsigned long long>(
                interpolation_diagnostics.window_interpolated_entity_frame_starvations),
            static_cast<unsigned long long>(
                interpolation_diagnostics.window_interpolated_entity_frame_checks)),
        28,
        244,
        16,
        Color{215, 220, 230, 255});
#endif

    const float scale = std::max(stats.down_kbps.max_value(), stats.up_kbps.max_value());
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    const Rectangle graph{28.0f, 266.0f, 404.0f, 34.0f};
#else
    const Rectangle graph{28.0f, 246.0f, 404.0f, 34.0f};
#endif
    draw_graph(graph, stats.down_kbps, Color{76, 190, 255, 255}, scale);
    draw_graph(graph, stats.up_kbps, Color{255, 190, 76, 255}, scale);
    DrawText(TextFormat("scale %.1f kbps", scale), 310, static_cast<int>(graph.y + 4.0f), 14, Color{215, 220, 230, 255});
    DrawText("down", 32, static_cast<int>(graph.y + graph.height + 2.0f), 14, Color{76, 190, 255, 255});
    DrawText("up", 82, static_cast<int>(graph.y + graph.height + 2.0f), 14, Color{255, 190, 76, 255});
}

}  // namespace

int main(int argc, char** argv) {
    constexpr double server_fixed_dt_seconds = 1.0 / 30.0;
    constexpr float server_fixed_dt = static_cast<float>(server_fixed_dt_seconds);
    kage::sync::ReplicationClientMode client_mode = kage::sync::ReplicationClientMode::Snap;
    kage::sync::SyncFrame interpolation_buffer_frames = 2;
    bool auto_interpolation_buffer_frames = true;
    float time_dilation_min = 0.95f;
    float time_dilation_max = 1.05f;
    float time_dilation_gain = 0.05f;
    LinkSettings link_settings;
    bool dynamic_bandwidth = true;
    std::size_t static_bandwidth_limit_bytes_per_tick = 32U * 1024U;
    std::size_t bandwidth_min_bytes_per_second = 64U * 1024U;
    std::size_t bandwidth_initial_bytes_per_second = 512U * 1024U;
    std::size_t bandwidth_max_bytes_per_second = 2U * 1024U * 1024U;
    int target_ball_count = 96;
    std::string trace_dir;
    bool trace_frame_data = true;
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    bool trace_packet_logs = false;
#endif
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
            } else if (value == "predict") {
                client_mode = kage::sync::ReplicationClientMode::Predict;
            } else {
                throw std::runtime_error("--client-mode must be snap, buffered-interpolation, or predict");
            }
        } else if (arg == "--latency-ms") {
            link_settings.latency_ms = std::stof(require_value());
        } else if (arg == "--jitter-ms") {
            link_settings.jitter_ms = std::stof(require_value());
        } else if (arg == "--loss-percent") {
            link_settings.loss_percent = std::stof(require_value());
        } else if (arg == "--link-bandwidth-kbps") {
            link_settings.bandwidth_kbps = std::max(0.0, std::stod(require_value()));
        } else if (arg == "--link-queue-kb") {
            link_settings.max_queue_bytes =
                static_cast<std::size_t>(std::max(0.0, std::stod(require_value())) * 1024.0);
        } else if (arg == "--bandwidth-mode") {
            const std::string value = require_value();
            if (value == "dynamic") {
                dynamic_bandwidth = true;
            } else if (value == "static") {
                dynamic_bandwidth = false;
            } else {
                throw std::runtime_error("--bandwidth-mode must be dynamic or static");
            }
        } else if (arg == "--bandwidth-limit-kbps") {
            const double kbps = std::stod(require_value());
            static_bandwidth_limit_bytes_per_tick =
                static_cast<std::size_t>(std::max(1.0, kbps * 1000.0 / 8.0 * server_fixed_dt_seconds));
        } else if (arg == "--bandwidth-min-kbps") {
            bandwidth_min_bytes_per_second =
                static_cast<std::size_t>(std::max(1.0, std::stod(require_value()) * 1000.0 / 8.0));
            dynamic_bandwidth = true;
        } else if (arg == "--bandwidth-initial-kbps") {
            bandwidth_initial_bytes_per_second =
                static_cast<std::size_t>(std::max(1.0, std::stod(require_value()) * 1000.0 / 8.0));
            dynamic_bandwidth = true;
        } else if (arg == "--bandwidth-max-kbps") {
            bandwidth_max_bytes_per_second =
                static_cast<std::size_t>(std::max(1.0, std::stod(require_value()) * 1000.0 / 8.0));
            dynamic_bandwidth = true;
        } else if (arg == "--entities") {
            target_ball_count = std::clamp(std::stoi(require_value()), min_ball_count, max_ball_count);
        } else if (arg == "--trace-dir") {
#ifdef KAGE_SYNC_ENABLE_TRACING
            trace_dir = require_value();
#else
            (void)require_value();
            throw std::runtime_error("--trace-dir requires a build with KAGE_SYNC_ENABLE_TRACING=ON");
#endif
        } else if (arg == "--trace-frame-data") {
            const std::string value = require_value();
            if (value == "on" || value == "true" || value == "1") {
                trace_frame_data = true;
            } else if (value == "off" || value == "false" || value == "0") {
                trace_frame_data = false;
            } else {
                throw std::runtime_error("--trace-frame-data must be on or off");
            }
        } else if (arg == "--trace-packet-logs") {
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
            const std::string value = require_value();
            if (value == "on" || value == "true" || value == "1") {
                trace_packet_logs = true;
            } else if (value == "off" || value == "false" || value == "0") {
                trace_packet_logs = false;
            } else {
                throw std::runtime_error("--trace-packet-logs must be on or off");
            }
#else
            (void)require_value();
            throw std::runtime_error("--trace-packet-logs requires a build with KAGE_SYNC_TRACE_PACKET_LOGS=ON");
#endif
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
    downstream_link.settings = link_settings;
    upstream_link.settings = link_settings;
    RuntimeStats stats;

    kage::sync::ReplicationServerOptions server_options;
    bandwidth_initial_bytes_per_second =
        std::clamp(bandwidth_initial_bytes_per_second, bandwidth_min_bytes_per_second, bandwidth_max_bytes_per_second);
    server_options.bandwidth_limit_bytes_per_tick = static_bandwidth_limit_bytes_per_tick;
    server_options.mtu_bytes = 1200;
    server_options.fixed_dt_seconds = server_fixed_dt_seconds;
    server_options.bandwidth.enabled = dynamic_bandwidth;
    server_options.bandwidth.min_bytes_per_second = bandwidth_min_bytes_per_second;
    server_options.bandwidth.initial_bytes_per_second = bandwidth_initial_bytes_per_second;
    server_options.bandwidth.max_bytes_per_second = bandwidth_max_bytes_per_second;
    server_options.bandwidth.max_burst_bytes = std::max(
        server_options.mtu_bytes * 4U,
        static_cast<std::size_t>(
            std::ceil(static_cast<double>(server_options.bandwidth.max_bytes_per_second) * server_fixed_dt_seconds)));
    server_options.prioritizer = make_sphere_prioritizer(server_registry);
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        if (client_connected) {
            ++stats.server_packets;
            queue_packet(downstream_link, server_socket, client_address, packet, stats, true);
        }
    };
#ifdef KAGE_SYNC_ENABLE_TRACING
    kage::sync::TraceOptions trace_options;
    trace_options.enabled = !trace_dir.empty();
    trace_options.directory = trace_dir;
    trace_options.frame_data = trace_frame_data;
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    trace_options.packet_logs = trace_packet_logs;
#endif
    server_options.trace = trace_options;
#endif
    kage::sync::ReplicationServer server(server_options);
    kage::sync::ReplicationClientOptions client_options;
    client_options.mtu_bytes = 1200;
    client_options.default_entity_mode = client_mode;
    client_options.interpolation_buffer_frames = interpolation_buffer_frames;
    client_options.interpolation_buffer_capacity_frames = 64;
    client_options.auto_interpolation_buffer_frames = auto_interpolation_buffer_frames;
    client_options.auto_interpolation_jitter_multiplier = 2.0f;
    client_options.auto_interpolation_smoothing = 0.1f;
    client_options.auto_interpolation_time_dilation_min = time_dilation_min;
    client_options.auto_interpolation_time_dilation_max = time_dilation_max;
    client_options.auto_interpolation_time_dilation_gain = time_dilation_gain;
    client_options.entity_mode_selector = [&](const kage::sync::ReplicatedEntityUpdateView&) {
        return client_mode;
    };
    client_options.fixed_dt_seconds = 1.0 / 30.0;
    client_options.rollback_policy = kage::sync::ReplicationRollbackPolicy::OnlyAffected;
#ifdef KAGE_SYNC_ENABLE_TRACING
    client_options.trace = trace_options;
#endif
    kage::sync::ReplicationClient client(client_options);
    register_client_prediction_jobs(client_registry, client);

    InitWindow(1280, 720, "kage-sync localhost balls");
    Camera3D camera{};
    camera.position = Vector3{0.0f, 5.5f, 9.0f};
    camera.target = Vector3{0.0f, 0.0f, 0.0f};
    camera.up = Vector3{0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    std::vector<ServerBall> balls;
    std::vector<kage::sync::ClientEntityNetworkId> known_client_entities;
    std::vector<RenderedBall> rendered_balls;
    known_client_entities.reserve(max_ball_count);
    rendered_balls.reserve(max_ball_count);
    int spawn_index = 0;
    float server_accumulator = 0.0f;
    kage::sync::SyncFrame server_frame = 0;
    send_hello(client_socket, server_address);
    client.set_packet_sender([&](const ecs::BitBuffer& packet) {
        ++stats.client_packets;
        queue_packet(upstream_link, client_socket, server_address, packet, stats, false);
    });

    while (!WindowShouldClose()) {
        const float dt = GetFrameTime();
        server_accumulator += dt;
        update_hotkeys(link_settings);
        downstream_link.settings = link_settings;
        upstream_link.settings = link_settings;
        update_entity_count_hotkeys(target_ball_count);
        update_client_mode_hotkeys(client, client_registry, known_client_entities, client_mode);
        flush_link(downstream_link, server_socket);
        flush_link(upstream_link, client_socket);
        update_bandwidth_samples(stats, dt);

        ecs::BitBuffer received;
        sockaddr_in sender{};
        while (receive_packet(server_socket, received, &sender)) {
            ecs::BitBuffer read = received;
            const auto message = static_cast<std::uint8_t>(read.read_bits(8U));
            if (message == example_client_hello_message) {
                const auto id = static_cast<kage::sync::ClientId>(read.read_unsigned_bits(64U));
                if (id == client_id) {
                    client_address = sender;
                    client_connected = true;
                    server.add_client(client_id);
                }
            } else {
                server.receive_packet(client_id, received);
            }
        }

        while (server_accumulator >= server_fixed_dt) {
            server_accumulator -= server_fixed_dt;
            ++server_frame;
            update_server_world(
                server_registry,
                balls,
                server_schema,
                server_frame,
                server_fixed_dt,
                spawn_index,
                target_ball_count);
            server.tick(server_registry, server_fixed_dt);
        }
        stats.server_entities = static_cast<int>(balls.size());

        while (receive_packet(client_socket, received)) {
            ecs::BitBuffer read = received;
            if (read.remaining_bits() >= 40U &&
                static_cast<std::uint8_t>(read.read_bits(8U)) == kage::sync::protocol::server_update_message) {
                stats.frame = static_cast<kage::sync::SyncFrame>(read.read_bits(32U));
            }
            client.receive_packet(received);
            interpolation_buffer_frames = client.current_interpolation_buffer_frames();
        }
        client.tick(client_registry, dt);
        std::vector<ecs::Entity> expired_flashes;
        client_registry.view<BallCueFlash>().each([&](ecs::Entity entity, BallCueFlash& flash) {
            flash.seconds -= dt;
            if (flash.seconds <= 0.0f) {
                expired_flashes.push_back(entity);
            }
        });
        for (ecs::Entity entity : expired_flashes) {
            client_registry.remove<BallCueFlash>(entity);
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
        rendered_balls.clear();
        for (const kage::sync::FractionalTickSample& entity : client.fractional_tick_frame(client_registry).entities) {
            BallPosition position;
            if (!entity.try_get_sampled_value(client_registry, position)) {
                continue;
            }
            const BallVisual* visual = client_registry.try_get<BallVisual>(entity.local_entity);
            if (visual == nullptr) {
                continue;
            }
            const BallContact* contact = client_registry.try_get<BallContact>(entity.local_entity);
            rendered_balls.push_back(RenderedBall{
                entity.client_entity_network_id,
                entity.local_entity,
                Vector3{position.x, position.y, position.z},
                *visual,
                contact != nullptr ? *contact : BallContact{},
                client_registry.has(entity.local_entity, client_schema.spawn_tagged),
                client_registry.has(entity.local_entity, client_schema.bounced),
            });
            remember_client_entity(known_client_entities, entity.client_entity_network_id);
        }

        auto find_rendered_position = [&](kage::sync::ClientEntityNetworkId network_id, Vector3& out) {
            for (const RenderedBall& ball : rendered_balls) {
                if (ball.network_id == network_id) {
                    out = ball.position;
                    return true;
                }
            }

            const ecs::Entity local = client.local_entity(network_id);
            if (!local || !client_registry.alive(local)) {
                return false;
            }
            const BallPosition* position = client_registry.try_get<BallPosition>(local);
            if (position == nullptr) {
                return false;
            }
            out = Vector3{position->x, position->y, position->z};
            return true;
        };

        for (const RenderedBall& ball : rendered_balls) {
            const kage::sync::ClientEntityNetworkId target = ball.contact.target.client_entity_network_id;
            if (target == kage::sync::invalid_client_entity_network_id) {
                continue;
            }
            Vector3 target_position{};
            if (find_rendered_position(target, target_position)) {
                draw_contact_link(ball.position, target_position);
            }
        }

        for (const RenderedBall& ball : rendered_balls) {
            const BallVisual& visual = ball.visual;
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
            if (ball.spawn_tagged) {
                pulse_toward(Color{255, 210, 74, visual.a}, 0.0f);
            }
            if (ball.bounced) {
                pulse_toward(Color{70, 220, 255, visual.a}, 2.1f);
            }
            DrawSphere(ball.position, visual.radius, color);
            if (const BallCueFlash* flash = client_registry.try_get<BallCueFlash>(ball.local_entity)) {
                const float alpha = std::clamp(flash->seconds / 0.22f, 0.0f, 1.0f);
                DrawSphereWires(
                    ball.position,
                    visual.radius + 0.08f + 0.14f * (1.0f - alpha),
                    12,
                    8,
                    Color{255, 245, 150, static_cast<unsigned char>(alpha * 220.0f)});
            }
        }
        stats.rendered_entities = static_cast<int>(rendered_balls.size());
        EndMode3D();
        draw_stats_overlay(
            stats,
            link_settings,
            client_mode,
            target_ball_count,
            interpolation_buffer_frames,
            server.bandwidth_stats(client_id),
            client.timing_stats()
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
                ,
            client.interpolation_diagnostics()
#endif
        );
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
        DrawFPS(28, 306);
#else
        DrawFPS(28, 282);
#endif
        EndDrawing();
    }

    CloseWindow();
#ifdef KAGE_SYNC_ENABLE_TRACING
    client.close_trace();
    server.close_trace();
#endif
    close_socket(client_socket);
    close_socket(server_socket);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
