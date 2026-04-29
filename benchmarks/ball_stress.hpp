#pragma once

#include "kage/sync/sync.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace kage::sync::stress {

struct BallPosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

}  // namespace kage::sync::stress

namespace kage::sync {

template <>
struct SyncComponentTraits<stress::BallPosition> {
    using Quantized = stress::BallPosition;

    static Quantized quantize(const stress::BallPosition& value) {
        return value;
    }

    static stress::BallPosition dequantize(const Quantized& value) {
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

namespace kage::sync::stress {

struct BallVisual {
    float radius = 0.25f;
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    std::uint8_t a = 255;
};

struct BallHealth {
    std::int32_t value = 100;
};

struct BallPoison {
    std::int32_t remaining = 0;
    float tick_accumulator = 0.0f;
};

struct SyncSchema {
    SyncArchetypeId ball;
};

struct StressConfig {
    double duration_seconds = 10.0;
    std::uint32_t clients = 4;
    std::uint32_t max_balls = 4096;
    double spawn_interval_ms = 5.0;
    std::int32_t poison_min = 1;
    std::int32_t poison_max = 8;
    std::int32_t health_min = 20;
    std::int32_t health_max = 80;
    double tick_rate = 30.0;
    std::size_t mtu = 1200;
    std::size_t bandwidth_limit = 1024U * 1024U;
    std::size_t server_worker_threads = 1;
    std::uint32_t seed = 0xC0FFEEU;
    double latency_ms = 0.0;
    double jitter_ms = 0.0;
    double loss_percent = 0.0;
    double server_to_client_latency_ms = -1.0;
    double client_to_server_latency_ms = -1.0;
    double server_to_client_jitter_ms = -1.0;
    double client_to_server_jitter_ms = -1.0;
    double server_to_client_loss_percent = -1.0;
    double client_to_server_loss_percent = -1.0;
    ReplicationClientMode client_mode = ReplicationClientMode::Snap;
    SyncFrame interpolation_buffer_frames = 2;
    std::size_t interpolation_buffer_capacity_frames = 64;
    double time_dilation_min = 0.95;
    double time_dilation_max = 1.05;
    double time_dilation_gain = 0.05;
    bool json = false;
};

struct ServerBall {
    ecs::Entity entity;
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;
};

enum class Direction {
    ServerToClient,
    ClientToServer
};

enum class PacketType {
    ServerUpdate,
    ClientAck,
    ClientHello,
    Unknown
};

struct PacketBreakdown {
    PacketType type = PacketType::Unknown;
    std::uint32_t full_upserts = 0;
    std::uint32_t delta_upserts = 0;
    std::uint32_t destroys = 0;
};

struct DirectionStats {
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;
    std::uint64_t delivered_packets = 0;
    std::uint64_t delivered_bytes = 0;
    std::uint64_t dropped_packets = 0;
    std::uint64_t dropped_bytes = 0;
    std::uint64_t server_update_packets = 0;
    std::uint64_t server_update_bytes = 0;
    std::uint64_t client_ack_packets = 0;
    std::uint64_t client_ack_bytes = 0;
    std::uint64_t client_hello_packets = 0;
    std::uint64_t client_hello_bytes = 0;
    std::uint64_t unknown_packets = 0;
    std::uint64_t unknown_bytes = 0;
    std::uint64_t full_upserts = 0;
    std::uint64_t delta_upserts = 0;
    std::uint64_t destroys = 0;
};

struct TimingStats {
    double server_simulation_seconds = 0.0;
    double server_replication_seconds = 0.0;
    double client_receive_seconds = 0.0;
    double ack_processing_seconds = 0.0;
    double wall_seconds = 0.0;
};

struct MemoryStats {
    std::size_t rss_start_bytes = 0;
    std::size_t rss_peak_bytes = 0;
    std::size_t rss_end_bytes = 0;
    std::size_t server_retained_quantized_frame_count = 0;
    std::size_t server_retained_quantized_frame_bytes = 0;
    std::size_t server_replicated_count = 0;
    std::size_t client_local_entities = 0;
    std::size_t client_pending_acks = 0;
};

struct ClientTimingSummary {
    std::uint64_t sample_count = 0;
    double average_latency_frames = 0.0;
    double average_jitter_frames = 0.0;
    double average_measured_interpolation_buffer_frames = 0.0;
    double average_time_dilation = 0.0;
    SyncFrame max_desired_interpolation_buffer_frames = 0;
    SyncFrame max_target_interpolation_buffer_frames = 0;
    SyncFrame max_current_interpolation_buffer_frames = 0;
};

struct StressReport {
    StressConfig config;
    TimingStats timing;
    MemoryStats memory;
    ClientTimingSummary client_timing;
    DirectionStats server_to_clients;
    DirectionStats clients_to_server;
    std::uint64_t ticks = 0;
    std::uint64_t spawned = 0;
    std::uint64_t despawned = 0;
    std::uint64_t poison_components_added = 0;
    std::uint64_t poison_components_removed = 0;
    std::uint64_t poison_ticks = 0;
    std::uint32_t live_balls = 0;
};

struct QueuedPacket {
    ClientId client = invalid_client_id;
    BitBuffer packet;
    double deliver_at = 0.0;
};

struct SimulatedLink {
    double latency_ms = 0.0;
    double jitter_ms = 0.0;
    double loss_percent = 0.0;
    std::deque<QueuedPacket> queued;
    std::mt19937 rng{0};
};

class ScopedTimer {
public:
    explicit ScopedTimer(double& out)
        : out_(out), begin_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        const auto end = std::chrono::steady_clock::now();
        out_ += std::chrono::duration<double>(end - begin_).count();
    }

private:
    double& out_;
    std::chrono::steady_clock::time_point begin_;
};

inline const char* packet_type_name(PacketType type) {
    switch (type) {
    case PacketType::ServerUpdate:
        return "server_update";
    case PacketType::ClientAck:
        return "client_ack";
    case PacketType::ClientHello:
        return "client_hello";
    case PacketType::Unknown:
        return "unknown";
    }
    return "unknown";
}

inline const char* client_mode_name(ReplicationClientMode mode) {
    switch (mode) {
    case ReplicationClientMode::Snap:
        return "snap";
    case ReplicationClientMode::BufferedInterpolation:
        return "buffered-interpolation";
    }
    return "snap";
}

inline std::size_t current_rss_bytes() {
#if defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::size_t pages = 0;
    std::size_t resident = 0;
    statm >> pages >> resident;
    const long page_size = 4096;
    return resident * static_cast<std::size_t>(page_size);
#else
    return 0;
#endif
}

inline void add_packet_stats(DirectionStats& stats, const BitBuffer& packet, const PacketBreakdown& breakdown) {
    const std::uint64_t bytes = packet.byte_size();
    ++stats.packets;
    stats.bytes += bytes;
    switch (breakdown.type) {
    case PacketType::ServerUpdate:
        ++stats.server_update_packets;
        stats.server_update_bytes += bytes;
        stats.full_upserts += breakdown.full_upserts;
        stats.delta_upserts += breakdown.delta_upserts;
        stats.destroys += breakdown.destroys;
        break;
    case PacketType::ClientAck:
        ++stats.client_ack_packets;
        stats.client_ack_bytes += bytes;
        break;
    case PacketType::ClientHello:
        ++stats.client_hello_packets;
        stats.client_hello_bytes += bytes;
        break;
    case PacketType::Unknown:
        ++stats.unknown_packets;
        stats.unknown_bytes += bytes;
        break;
    }
}

inline PacketBreakdown classify_packet(BitBuffer packet) {
    PacketBreakdown result;
    try {
        if (packet.remaining_bits() < 8U) {
            return result;
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message == protocol::client_ack_message) {
            result.type = PacketType::ClientAck;
            return result;
        }
        if (message == protocol::client_hello_message) {
            result.type = PacketType::ClientHello;
            return result;
        }
        if (message != protocol::server_update_message) {
            return result;
        }

        result.type = PacketType::ServerUpdate;
        const auto frame = static_cast<std::uint32_t>(packet.read_bits(32U));
        const auto entity_count = static_cast<std::uint16_t>(packet.read_bits(16U));
        for (std::uint16_t entity_index = 0; entity_index < entity_count; ++entity_index) {
            const bool destroy = packet.read_bool();
            packet.read_unsigned_bits(64U);
            if (destroy) {
                ++result.destroys;
                continue;
            }

            const bool full = packet.read_bool();
            if (full) {
                ++result.full_upserts;
                packet.read_bits(32U);
                const auto component_count = static_cast<std::uint16_t>(packet.read_bits(16U));
                for (std::uint16_t component = 0; component < component_count; ++component) {
                    const auto component_index = static_cast<std::uint16_t>(packet.read_bits(16U));
                    switch (component_index) {
                        case 0:
                            packet.skip_bits(sizeof(BallPosition) * 8U);
                            break;
                        case 1:
                            packet.skip_bits(sizeof(BallVisual) * 8U);
                            break;
                        case 2:
                            packet.skip_bits(sizeof(BallHealth) * 8U);
                            break;
                        case 3:
                            packet.skip_bits(sizeof(BallPoison) * 8U);
                            break;
                        default:
                            result = PacketBreakdown{};
                            return result;
                    }
                }
            } else {
                ++result.delta_upserts;
                std::uint32_t baseline_frame = 0;
                if (!protocol::read_baseline_frame(packet, frame, baseline_frame)) {
                    result = PacketBreakdown{};
                    return result;
                }
                bool changed[4] = {};
                for (std::size_t component_index = 0; component_index < 4U; ++component_index) {
                    changed[component_index] = packet.read_bool();
                }
                for (std::size_t component_index = 0; component_index < 4U; ++component_index) {
                    if (!changed[component_index]) {
                        continue;
                    }
                    switch (component_index) {
                        case 0:
                            packet.skip_bits(sizeof(BallPosition) * 8U);
                            break;
                        case 1:
                            packet.skip_bits(sizeof(BallVisual) * 8U);
                            break;
                        case 2:
                            packet.skip_bits(sizeof(BallHealth) * 8U);
                            break;
                        case 3:
                            packet.skip_bits(sizeof(BallPoison) * 8U);
                            break;
                    }
                }
            }
        }
    } catch (const std::exception&) {
        result = PacketBreakdown{};
    }
    return result;
}

inline bool drops_packet(SimulatedLink& link) {
    if (link.loss_percent <= 0.0) {
        return false;
    }
    std::uniform_real_distribution<double> distribution(0.0, 100.0);
    return distribution(link.rng) < link.loss_percent;
}

inline double latency_seconds(SimulatedLink& link) {
    double latency_ms = std::max(0.0, link.latency_ms);
    if (link.jitter_ms > 0.0) {
        std::uniform_real_distribution<double> distribution(-link.jitter_ms, link.jitter_ms);
        latency_ms = std::max(0.0, latency_ms + distribution(link.rng));
    }
    return latency_ms / 1000.0;
}

inline void enqueue_packet(
    SimulatedLink& link,
    DirectionStats& stats,
    ClientId client,
    const BitBuffer& packet,
    double now_seconds) {
    const PacketBreakdown breakdown = classify_packet(packet);
    add_packet_stats(stats, packet, breakdown);

    if (drops_packet(link)) {
        ++stats.dropped_packets;
        stats.dropped_bytes += packet.byte_size();
        return;
    }

    link.queued.push_back(QueuedPacket{client, packet, now_seconds + latency_seconds(link)});
}

template <typename Fn>
void deliver_ready(SimulatedLink& link, DirectionStats& stats, double now_seconds, Fn&& fn) {
    auto packet = link.queued.begin();
    while (packet != link.queued.end()) {
        if (packet->deliver_at > now_seconds) {
            ++packet;
            continue;
        }
        ++stats.delivered_packets;
        stats.delivered_bytes += packet->packet.byte_size();
        fn(packet->client, packet->packet);
        packet = link.queued.erase(packet);
    }
}

inline SyncSchema define_schema(ecs::Registry& registry, bool interpolate_position = false) {
    const ecs::Entity position = register_sync_component<BallPosition>(registry, "BallPosition");
    const ecs::Entity visual = register_sync_component<BallVisual>(registry, "BallVisual");
    const ecs::Entity health = register_sync_component<BallHealth>(registry, "BallHealth");
    const ecs::Entity poison = register_sync_component<BallPoison>(registry, "BallPoison");
    return SyncSchema{define_archetype(
        registry,
        "StressBall",
        {
            {position,
             ReplicationAudience::All,
             interpolate_position ? ComponentInterpolation::Interpolate : ComponentInterpolation::Step},
            {visual, ReplicationAudience::All},
            {health, ReplicationAudience::All},
            {poison, ReplicationAudience::All},
        })};
}

inline void validate_config(const StressConfig& config) {
    if (config.clients == 0) {
        throw std::invalid_argument("--clients must be greater than zero");
    }
    if (config.max_balls == 0) {
        throw std::invalid_argument("--max-balls must be greater than zero");
    }
    if (config.duration_seconds <= 0.0) {
        throw std::invalid_argument("--duration-seconds must be greater than zero");
    }
    if (config.tick_rate <= 0.0) {
        throw std::invalid_argument("--tick-rate must be greater than zero");
    }
    if (config.spawn_interval_ms <= 0.0) {
        throw std::invalid_argument("--spawn-interval-ms must be greater than zero");
    }
    if (config.poison_min < 0 || config.poison_max < config.poison_min) {
        throw std::invalid_argument("poison range is invalid");
    }
    if (config.health_min <= 0 || config.health_max < config.health_min) {
        throw std::invalid_argument("health range is invalid");
    }
    if (config.mtu == 0 || config.bandwidth_limit == 0) {
        throw std::invalid_argument("--mtu and --bandwidth-limit must be greater than zero");
    }
    if (config.server_worker_threads == 0U) {
        throw std::invalid_argument("--server-worker-threads must be greater than zero");
    }
    if (config.latency_ms < 0.0 ||
        config.jitter_ms < 0.0 ||
        config.server_to_client_latency_ms < 0.0 ||
        config.client_to_server_latency_ms < 0.0 ||
        config.server_to_client_jitter_ms < 0.0 ||
        config.client_to_server_jitter_ms < 0.0) {
        throw std::invalid_argument("latency and jitter values must be non-negative");
    }
    if (config.interpolation_buffer_capacity_frames == 0 ||
        (config.interpolation_buffer_capacity_frames & (config.interpolation_buffer_capacity_frames - 1U)) != 0U) {
        throw std::invalid_argument("--interpolation-buffer-capacity-frames must be a nonzero power of two");
    }
    if (config.interpolation_buffer_frames >= config.interpolation_buffer_capacity_frames) {
        throw std::invalid_argument("--interpolation-buffer-frames must be smaller than capacity");
    }
    if (config.time_dilation_min <= 0.0 || config.time_dilation_min > 1.0) {
        throw std::invalid_argument("--time-dilation-min must be in the range (0, 1]");
    }
    if (config.time_dilation_max < 1.0) {
        throw std::invalid_argument("--time-dilation-max must be at least 1");
    }
    if (config.time_dilation_gain < 0.0) {
        throw std::invalid_argument("--time-dilation-gain must be non-negative");
    }
}

inline void spawn_ball(
    ecs::Registry& registry,
    std::vector<ServerBall>& balls,
    SyncArchetypeId archetype,
    const StressConfig& config,
    std::mt19937& rng,
    StressReport& report) {
    std::uniform_real_distribution<float> position_x(-9.0f, 9.0f);
    std::uniform_real_distribution<float> position_y(-4.5f, 4.5f);
    std::uniform_real_distribution<float> position_z(-4.5f, 4.5f);
    std::uniform_real_distribution<float> velocity(-8.0f, 8.0f);
    std::uniform_real_distribution<float> radius(0.12f, 0.35f);
    std::uniform_int_distribution<int> color(64, 255);
    std::uniform_int_distribution<std::int32_t> health(config.health_min, config.health_max);

    const ecs::Entity entity = registry.create();
    registry.add<BallPosition>(entity, BallPosition{position_x(rng), position_y(rng), position_z(rng)});
    registry.add<BallVisual>(
        entity,
        BallVisual{
            radius(rng),
            static_cast<std::uint8_t>(color(rng)),
            static_cast<std::uint8_t>(color(rng)),
            static_cast<std::uint8_t>(color(rng)),
            255});
    registry.add<BallHealth>(entity, BallHealth{health(rng)});
    registry.add<Replicated>(entity, Replicated{archetype});

    auto non_zero_velocity = [&](float value) {
        if (value >= 0.0f && value < 1.0f) {
            return 1.0f;
        }
        if (value < 0.0f && value > -1.0f) {
            return -1.0f;
        }
        return value;
    };
    balls.push_back(ServerBall{
        entity,
        non_zero_velocity(velocity(rng)),
        non_zero_velocity(velocity(rng)),
        non_zero_velocity(velocity(rng)),
    });
    ++report.spawned;
}

inline void add_poison(
    ecs::Registry& registry,
    ecs::Entity entity,
    const StressConfig& config,
    std::mt19937& rng,
    StressReport& report) {
    if (config.poison_max == 0) {
        return;
    }
    std::uniform_int_distribution<std::int32_t> poison(config.poison_min, config.poison_max);
    const std::int32_t amount = poison(rng);
    if (amount <= 0) {
        return;
    }

    if (const BallPoison* existing = registry.try_get<BallPoison>(entity)) {
        BallPoison value = *existing;
        value.remaining += amount;
        registry.add<BallPoison>(entity, value);
        return;
    }

    registry.add<BallPoison>(entity, BallPoison{amount, 0.0f});
    ++report.poison_components_added;
}

inline void update_server_world(
    ecs::Registry& registry,
    std::vector<ServerBall>& balls,
    SyncSchema schema,
    const StressConfig& config,
    double dt,
    double& spawn_accumulator,
    std::mt19937& rng,
    StressReport& report) {
    spawn_accumulator += dt;
    const double spawn_interval_seconds = config.spawn_interval_ms / 1000.0;
    while (spawn_accumulator >= spawn_interval_seconds && balls.size() < config.max_balls) {
        spawn_accumulator -= spawn_interval_seconds;
        spawn_ball(registry, balls, schema.ball, config, rng, report);
    }

    constexpr float min_x = -10.0f;
    constexpr float max_x = 10.0f;
    constexpr float min_y = -5.0f;
    constexpr float max_y = 5.0f;
    constexpr float min_z = -5.0f;
    constexpr float max_z = 5.0f;

    for (ServerBall& ball : balls) {
        BallPosition& position = registry.write<BallPosition>(ball.entity);
        position.x += ball.vx * static_cast<float>(dt);
        position.y += ball.vy * static_cast<float>(dt);
        position.z += ball.vz * static_cast<float>(dt);

        bool bounced = false;
        if (position.x < min_x || position.x > max_x) {
            position.x = std::max(min_x, std::min(max_x, position.x));
            ball.vx = -ball.vx;
            bounced = true;
        }
        if (position.y < min_y || position.y > max_y) {
            position.y = std::max(min_y, std::min(max_y, position.y));
            ball.vy = -ball.vy;
            bounced = true;
        }
        if (position.z < min_z || position.z > max_z) {
            position.z = std::max(min_z, std::min(max_z, position.z));
            ball.vz = -ball.vz;
            bounced = true;
        }
        if (bounced) {
            add_poison(registry, ball.entity, config, rng, report);
        }

        if (const BallPoison* poison = registry.try_get<BallPoison>(ball.entity)) {
            BallPoison next = *poison;
            next.tick_accumulator += static_cast<float>(dt);
            BallHealth& health = registry.write<BallHealth>(ball.entity);
            while (next.remaining > 0 && next.tick_accumulator >= 0.25f) {
                next.tick_accumulator -= 0.25f;
                --next.remaining;
                --health.value;
                ++report.poison_ticks;
            }
            if (next.remaining <= 0) {
                registry.remove<BallPoison>(ball.entity);
                ++report.poison_components_removed;
            } else {
                registry.add<BallPoison>(ball.entity, next);
            }
        }
    }

    balls.erase(
        std::remove_if(
            balls.begin(),
            balls.end(),
            [&](const ServerBall& ball) {
                const BallHealth* health = registry.try_get<BallHealth>(ball.entity);
                if (health != nullptr && health->value > 0) {
                    return false;
                }
                if (registry.destroy(ball.entity)) {
                    ++report.despawned;
                }
                return true;
            }),
        balls.end());
}

inline std::size_t count_client_entities(ecs::Registry& registry) {
    std::size_t count = 0;
    registry.view<const BallPosition>().each([&](ecs::Entity, const BallPosition&) {
        ++count;
    });
    return count;
}

inline StressReport run_stress(const StressConfig& input_config) {
    StressConfig config = input_config;
    if (config.server_to_client_latency_ms < 0.0) {
        config.server_to_client_latency_ms = config.latency_ms;
    }
    if (config.client_to_server_latency_ms < 0.0) {
        config.client_to_server_latency_ms = config.latency_ms;
    }
    if (config.server_to_client_jitter_ms < 0.0) {
        config.server_to_client_jitter_ms = config.jitter_ms;
    }
    if (config.client_to_server_jitter_ms < 0.0) {
        config.client_to_server_jitter_ms = config.jitter_ms;
    }
    if (config.server_to_client_loss_percent < 0.0) {
        config.server_to_client_loss_percent = config.loss_percent;
    }
    if (config.client_to_server_loss_percent < 0.0) {
        config.client_to_server_loss_percent = config.loss_percent;
    }
    validate_config(config);

    StressReport report;
    report.config = config;
    report.memory.rss_start_bytes = current_rss_bytes();
    report.memory.rss_peak_bytes = report.memory.rss_start_bytes;

    ecs::Registry server_registry;
    configure_server(server_registry);
    const SyncSchema server_schema = define_schema(server_registry);

    std::vector<ecs::Registry> client_registries(config.clients);
    for (std::uint32_t client_index = 0; client_index < config.clients; ++client_index) {
        configure_client(client_registries[client_index], static_cast<ClientId>(client_index + 1U));
        define_schema(
            client_registries[client_index],
            config.client_mode == ReplicationClientMode::BufferedInterpolation);
    }

    SimulatedLink server_to_clients;
    server_to_clients.latency_ms = config.server_to_client_latency_ms;
    server_to_clients.jitter_ms = config.server_to_client_jitter_ms;
    server_to_clients.loss_percent = config.server_to_client_loss_percent;
    server_to_clients.rng.seed(config.seed ^ 0x5EED1234U);

    SimulatedLink clients_to_server;
    clients_to_server.latency_ms = config.client_to_server_latency_ms;
    clients_to_server.jitter_ms = config.client_to_server_jitter_ms;
    clients_to_server.loss_percent = config.client_to_server_loss_percent;
    clients_to_server.rng.seed(config.seed ^ 0xA11CE55U);

    ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = config.bandwidth_limit;
    server_options.mtu_bytes = config.mtu;
    server_options.serialized_worker_threads = config.server_worker_threads;
    server_options.transport = [&](ClientId client, const BitBuffer& packet) {
        enqueue_packet(server_to_clients, report.server_to_clients, client, packet, static_cast<double>(report.ticks) / config.tick_rate);
    };
    ReplicationServer server(server_options);
    for (std::uint32_t client_index = 0; client_index < config.clients; ++client_index) {
        server.add_client(static_cast<ClientId>(client_index + 1U));
    }

    std::vector<ReplicationClient> clients;
    clients.reserve(config.clients);
    for (std::uint32_t client_index = 0; client_index < config.clients; ++client_index) {
        clients.emplace_back(ReplicationClientOptions{
            config.mtu,
            config.client_mode,
            config.interpolation_buffer_frames,
            config.interpolation_buffer_capacity_frames,
            true,
            1,
            2.0f,
            0.1f,
            static_cast<float>(config.time_dilation_min),
            static_cast<float>(config.time_dilation_max),
            static_cast<float>(config.time_dilation_gain)});
    }

    std::vector<ServerBall> balls;
    balls.reserve(config.max_balls);
    std::vector<double> client_accumulators(config.clients, 0.0);
    std::vector<SyncFrame> client_frames(config.clients, 0);
    std::mt19937 simulation_rng(config.seed);
    double spawn_accumulator = config.spawn_interval_ms / 1000.0;
    const double dt = 1.0 / config.tick_rate;
    const std::uint64_t total_ticks = static_cast<std::uint64_t>(config.duration_seconds * config.tick_rate);
    const auto wall_begin = std::chrono::steady_clock::now();

    for (std::uint64_t tick = 0; tick < total_ticks; ++tick) {
        report.ticks = tick;
        const double now = static_cast<double>(tick) * dt;

        {
            ScopedTimer timer(report.timing.client_receive_seconds);
            deliver_ready(server_to_clients, report.server_to_clients, now, [&](ClientId client_id, const BitBuffer& packet) {
                const std::size_t index = static_cast<std::size_t>(client_id - 1U);
                if (index < clients.size()) {
                    clients[index].receive(
                        client_registries[index],
                        packet,
                        static_cast<SyncFrame>(tick),
                        client_frames[index]);
                }
            });
            if (config.client_mode == ReplicationClientMode::BufferedInterpolation) {
                for (std::size_t index = 0; index < clients.size(); ++index) {
                    client_accumulators[index] += dt * clients[index].timing_stats().time_dilation;
                    while (client_accumulators[index] >= dt) {
                        client_accumulators[index] -= dt;
                        ++client_frames[index];
                        clients[index].apply_frame(client_registries[index], client_frames[index]);
                    }
                }
            }
        }

        {
            ScopedTimer timer(report.timing.ack_processing_seconds);
            for (std::size_t index = 0; index < clients.size(); ++index) {
                for (const BitBuffer& ack : clients[index].drain_ack_packets()) {
                    enqueue_packet(
                        clients_to_server,
                        report.clients_to_server,
                        static_cast<ClientId>(index + 1U),
                        ack,
                        now);
                }
            }
            deliver_ready(clients_to_server, report.clients_to_server, now, [&](ClientId client_id, const BitBuffer& packet) {
                server.process_packet(client_id, packet);
            });
        }

        {
            ScopedTimer timer(report.timing.server_simulation_seconds);
            update_server_world(server_registry, balls, server_schema, config, dt, spawn_accumulator, simulation_rng, report);
        }

        {
            ScopedTimer timer(report.timing.server_replication_seconds);
            server.tick(server_registry);
            server_registry.clear_all_dirty<BallPosition>();
            server_registry.clear_all_dirty<BallVisual>();
            server_registry.clear_all_dirty<BallHealth>();
            server_registry.clear_all_dirty<BallPoison>();
        }

        if ((tick % 16U) == 0U) {
            report.memory.rss_peak_bytes = std::max(report.memory.rss_peak_bytes, current_rss_bytes());
        }
    }

    const double end_time = static_cast<double>(total_ticks) * dt + 60.0;
    deliver_ready(server_to_clients, report.server_to_clients, end_time, [&](ClientId client_id, const BitBuffer& packet) {
        const std::size_t index = static_cast<std::size_t>(client_id - 1U);
        if (index < clients.size()) {
            clients[index].receive(client_registries[index], packet);
            if (config.client_mode == ReplicationClientMode::BufferedInterpolation) {
                clients[index].apply_frame(client_registries[index], client_frames[index]);
            }
        }
    });
    for (std::size_t index = 0; index < clients.size(); ++index) {
        for (const BitBuffer& ack : clients[index].drain_ack_packets()) {
            enqueue_packet(clients_to_server, report.clients_to_server, static_cast<ClientId>(index + 1U), ack, end_time);
        }
    }
    deliver_ready(clients_to_server, report.clients_to_server, end_time + 60.0, [&](ClientId client_id, const BitBuffer& packet) {
        server.process_packet(client_id, packet);
    });

    const auto wall_end = std::chrono::steady_clock::now();
    report.timing.wall_seconds = std::chrono::duration<double>(wall_end - wall_begin).count();
    report.memory.rss_end_bytes = current_rss_bytes();
    report.memory.rss_peak_bytes = std::max(report.memory.rss_peak_bytes, report.memory.rss_end_bytes);
    report.memory.server_retained_quantized_frame_count = server.retained_quantized_frame_count();
    report.memory.server_retained_quantized_frame_bytes = server.retained_quantized_frame_bytes();
    report.memory.server_replicated_count = server.replicated_count();
    for (std::size_t index = 0; index < clients.size(); ++index) {
        report.memory.client_local_entities += count_client_entities(client_registries[index]);
        report.memory.client_pending_acks += clients[index].pending_ack_count();
        const ReplicationClientTimingStats& timing = clients[index].timing_stats();
        report.client_timing.sample_count += timing.sample_count;
        report.client_timing.average_latency_frames += timing.latency_frames;
        report.client_timing.average_jitter_frames += timing.jitter_frames;
        report.client_timing.average_measured_interpolation_buffer_frames +=
            timing.measured_interpolation_buffer_frames;
        report.client_timing.average_time_dilation += timing.time_dilation;
        report.client_timing.max_desired_interpolation_buffer_frames = std::max(
            report.client_timing.max_desired_interpolation_buffer_frames,
            timing.desired_interpolation_buffer_frames);
        report.client_timing.max_target_interpolation_buffer_frames = std::max(
            report.client_timing.max_target_interpolation_buffer_frames,
            timing.target_interpolation_buffer_frames);
        report.client_timing.max_current_interpolation_buffer_frames = std::max(
            report.client_timing.max_current_interpolation_buffer_frames,
            timing.current_interpolation_buffer_frames);
    }
    if (!clients.empty()) {
        report.client_timing.average_latency_frames /= static_cast<double>(clients.size());
        report.client_timing.average_jitter_frames /= static_cast<double>(clients.size());
        report.client_timing.average_measured_interpolation_buffer_frames /= static_cast<double>(clients.size());
        report.client_timing.average_time_dilation /= static_cast<double>(clients.size());
    }
    report.live_balls = static_cast<std::uint32_t>(balls.size());
    report.ticks = total_ticks;
    return report;
}

inline std::string bytes_string(std::uint64_t bytes) {
    std::ostringstream out;
    out << bytes;
    return out.str();
}

inline void write_direction_text(std::ostream& out, const char* name, const DirectionStats& stats) {
    out << name << " packets=" << stats.packets << " bytes=" << stats.bytes
        << " delivered_packets=" << stats.delivered_packets << " delivered_bytes=" << stats.delivered_bytes
        << " dropped_packets=" << stats.dropped_packets << " dropped_bytes=" << stats.dropped_bytes << '\n';
    out << "  server_update packets=" << stats.server_update_packets << " bytes=" << stats.server_update_bytes
        << " full_upserts=" << stats.full_upserts << " delta_upserts=" << stats.delta_upserts
        << " destroys=" << stats.destroys << '\n';
    out << "  client_ack packets=" << stats.client_ack_packets << " bytes=" << stats.client_ack_bytes << '\n';
    out << "  client_hello packets=" << stats.client_hello_packets << " bytes=" << stats.client_hello_bytes << '\n';
    out << "  unknown packets=" << stats.unknown_packets << " bytes=" << stats.unknown_bytes << '\n';
}

inline void write_report_text(std::ostream& out, const StressReport& report) {
    out << "kage-sync ball stress\n";
    out << "ticks=" << report.ticks << " clients=" << report.config.clients
        << " live_balls=" << report.live_balls << " spawned=" << report.spawned
        << " despawned=" << report.despawned << '\n';
    out << "client_mode=" << client_mode_name(report.config.client_mode)
        << " interpolation_buffer_frames=" << report.config.interpolation_buffer_frames
        << " interpolation_buffer_capacity_frames=" << report.config.interpolation_buffer_capacity_frames << '\n';
    out << "network latency_ms=" << report.config.latency_ms
        << " jitter_ms=" << report.config.jitter_ms
        << " server_to_client_latency_ms=" << report.config.server_to_client_latency_ms
        << " server_to_client_jitter_ms=" << report.config.server_to_client_jitter_ms
        << " client_to_server_latency_ms=" << report.config.client_to_server_latency_ms
        << " client_to_server_jitter_ms=" << report.config.client_to_server_jitter_ms << '\n';
    out << "client_timing samples=" << report.client_timing.sample_count
        << " average_latency_frames=" << report.client_timing.average_latency_frames
        << " average_jitter_frames=" << report.client_timing.average_jitter_frames
        << " average_measured_buffer_frames="
        << report.client_timing.average_measured_interpolation_buffer_frames
        << " average_time_dilation=" << report.client_timing.average_time_dilation
        << " desired_interpolation_buffer_frames="
        << report.client_timing.max_desired_interpolation_buffer_frames
        << " target_interpolation_buffer_frames=" << report.client_timing.max_target_interpolation_buffer_frames
        << " current_interpolation_buffer_frames=" << report.client_timing.max_current_interpolation_buffer_frames << '\n';
    out << "poison added_components=" << report.poison_components_added
        << " removed_components=" << report.poison_components_removed
        << " ticks=" << report.poison_ticks << '\n';
    out << std::fixed << std::setprecision(6);
    out << "timing wall=" << report.timing.wall_seconds
        << " server_sim=" << report.timing.server_simulation_seconds
        << " server_replication=" << report.timing.server_replication_seconds
        << " client_receive=" << report.timing.client_receive_seconds
        << " ack_processing=" << report.timing.ack_processing_seconds << '\n';
    write_direction_text(out, "server_to_clients", report.server_to_clients);
    write_direction_text(out, "clients_to_server", report.clients_to_server);
    out << "memory rss_start_bytes=" << report.memory.rss_start_bytes
        << " rss_peak_bytes=" << report.memory.rss_peak_bytes
        << " rss_end_bytes=" << report.memory.rss_end_bytes
        << " server_retained_quantized_frame_count=" << report.memory.server_retained_quantized_frame_count
        << " server_retained_quantized_frame_bytes=" << report.memory.server_retained_quantized_frame_bytes
        << " server_replicated_count=" << report.memory.server_replicated_count
        << " client_local_entities=" << report.memory.client_local_entities
        << " client_pending_acks=" << report.memory.client_pending_acks << '\n';
    out << "memory_note=combined_process_rss_cannot_be_exactly_split_by_server_and_client\n";
}

inline void write_direction_json(std::ostream& out, const DirectionStats& stats) {
    out << "{"
        << "\"packets\":" << stats.packets << ","
        << "\"bytes\":" << stats.bytes << ","
        << "\"delivered_packets\":" << stats.delivered_packets << ","
        << "\"delivered_bytes\":" << stats.delivered_bytes << ","
        << "\"dropped_packets\":" << stats.dropped_packets << ","
        << "\"dropped_bytes\":" << stats.dropped_bytes << ","
        << "\"server_update\":{\"packets\":" << stats.server_update_packets << ",\"bytes\":" << stats.server_update_bytes
        << ",\"full_upserts\":" << stats.full_upserts << ",\"delta_upserts\":" << stats.delta_upserts
        << ",\"destroys\":" << stats.destroys << "},"
        << "\"client_ack\":{\"packets\":" << stats.client_ack_packets << ",\"bytes\":" << stats.client_ack_bytes << "},"
        << "\"client_hello\":{\"packets\":" << stats.client_hello_packets << ",\"bytes\":" << stats.client_hello_bytes << "},"
        << "\"unknown\":{\"packets\":" << stats.unknown_packets << ",\"bytes\":" << stats.unknown_bytes << "}"
        << "}";
}

inline void write_report_json(std::ostream& out, const StressReport& report) {
    out << std::fixed << std::setprecision(9);
    out << "{";
    out << "\"ticks\":" << report.ticks << ",";
    out << "\"clients\":" << report.config.clients << ",";
    out << "\"client_mode\":\"" << client_mode_name(report.config.client_mode) << "\",";
    out << "\"interpolation_buffer_frames\":" << report.config.interpolation_buffer_frames << ",";
    out << "\"interpolation_buffer_capacity_frames\":"
        << report.config.interpolation_buffer_capacity_frames << ",";
    out << "\"network\":{\"latency_ms\":" << report.config.latency_ms
        << ",\"jitter_ms\":" << report.config.jitter_ms
        << ",\"server_to_client_latency_ms\":" << report.config.server_to_client_latency_ms
        << ",\"server_to_client_jitter_ms\":" << report.config.server_to_client_jitter_ms
        << ",\"client_to_server_latency_ms\":" << report.config.client_to_server_latency_ms
        << ",\"client_to_server_jitter_ms\":" << report.config.client_to_server_jitter_ms << "},";
    out << "\"client_timing\":{\"sample_count\":" << report.client_timing.sample_count
        << ",\"average_latency_frames\":" << report.client_timing.average_latency_frames
        << ",\"average_jitter_frames\":" << report.client_timing.average_jitter_frames
        << ",\"average_measured_interpolation_buffer_frames\":"
        << report.client_timing.average_measured_interpolation_buffer_frames
        << ",\"average_time_dilation\":" << report.client_timing.average_time_dilation
        << ",\"desired_interpolation_buffer_frames\":"
        << report.client_timing.max_desired_interpolation_buffer_frames
        << ",\"target_interpolation_buffer_frames\":"
        << report.client_timing.max_target_interpolation_buffer_frames
        << ",\"current_interpolation_buffer_frames\":"
        << report.client_timing.max_current_interpolation_buffer_frames << "},";
    out << "\"live_balls\":" << report.live_balls << ",";
    out << "\"spawned\":" << report.spawned << ",";
    out << "\"despawned\":" << report.despawned << ",";
    out << "\"poison\":{\"added_components\":" << report.poison_components_added
        << ",\"removed_components\":" << report.poison_components_removed
        << ",\"ticks\":" << report.poison_ticks << "},";
    out << "\"timing\":{\"wall_seconds\":" << report.timing.wall_seconds
        << ",\"server_simulation_seconds\":" << report.timing.server_simulation_seconds
        << ",\"server_replication_seconds\":" << report.timing.server_replication_seconds
        << ",\"client_receive_seconds\":" << report.timing.client_receive_seconds
        << ",\"ack_processing_seconds\":" << report.timing.ack_processing_seconds << "},";
    out << "\"bandwidth\":{\"server_to_clients\":";
    write_direction_json(out, report.server_to_clients);
    out << ",\"clients_to_server\":";
    write_direction_json(out, report.clients_to_server);
    out << "},";
    out << "\"memory\":{\"rss_start_bytes\":" << report.memory.rss_start_bytes
        << ",\"rss_peak_bytes\":" << report.memory.rss_peak_bytes
        << ",\"rss_end_bytes\":" << report.memory.rss_end_bytes
        << ",\"server_retained_quantized_frame_count\":" << report.memory.server_retained_quantized_frame_count
        << ",\"server_retained_quantized_frame_bytes\":" << report.memory.server_retained_quantized_frame_bytes
        << ",\"server_replicated_count\":" << report.memory.server_replicated_count
        << ",\"client_local_entities\":" << report.memory.client_local_entities
        << ",\"client_pending_acks\":" << report.memory.client_pending_acks
        << ",\"note\":\"combined_process_rss_cannot_be_exactly_split_by_server_and_client\"}";
    out << "}\n";
}

inline std::uint64_t parse_u64(const std::string& name, const std::string& value) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        throw std::invalid_argument("invalid value for " + name + ": " + value);
    }
    return static_cast<std::uint64_t>(parsed);
}

inline std::int32_t parse_i32(const std::string& name, const std::string& value) {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' ||
        parsed < std::numeric_limits<std::int32_t>::min() ||
        parsed > std::numeric_limits<std::int32_t>::max()) {
        throw std::invalid_argument("invalid value for " + name + ": " + value);
    }
    return static_cast<std::int32_t>(parsed);
}

inline double parse_double(const std::string& name, const std::string& value) {
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') {
        throw std::invalid_argument("invalid value for " + name + ": " + value);
    }
    return parsed;
}

inline ReplicationClientMode parse_client_mode(const std::string& value) {
    if (value == "snap") {
        return ReplicationClientMode::Snap;
    }
    if (value == "buffered-interpolation") {
        return ReplicationClientMode::BufferedInterpolation;
    }
    throw std::invalid_argument("--client-mode must be snap or buffered-interpolation");
}

inline StressConfig parse_args(int argc, char** argv) {
    StressConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto require_value = [&]() -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument("missing value for " + arg);
            }
            return argv[++index];
        };

        if (arg == "--duration-seconds") {
            config.duration_seconds = parse_double(arg, require_value());
        } else if (arg == "--clients") {
            config.clients = static_cast<std::uint32_t>(parse_u64(arg, require_value()));
        } else if (arg == "--max-balls") {
            config.max_balls = static_cast<std::uint32_t>(parse_u64(arg, require_value()));
        } else if (arg == "--spawn-interval-ms") {
            config.spawn_interval_ms = parse_double(arg, require_value());
        } else if (arg == "--poison-min") {
            config.poison_min = parse_i32(arg, require_value());
        } else if (arg == "--poison-max") {
            config.poison_max = parse_i32(arg, require_value());
        } else if (arg == "--health-min") {
            config.health_min = parse_i32(arg, require_value());
        } else if (arg == "--health-max") {
            config.health_max = parse_i32(arg, require_value());
        } else if (arg == "--tick-rate") {
            config.tick_rate = parse_double(arg, require_value());
        } else if (arg == "--mtu") {
            config.mtu = static_cast<std::size_t>(parse_u64(arg, require_value()));
        } else if (arg == "--bandwidth-limit") {
            config.bandwidth_limit = static_cast<std::size_t>(parse_u64(arg, require_value()));
        } else if (arg == "--server-worker-threads") {
            config.server_worker_threads = static_cast<std::size_t>(parse_u64(arg, require_value()));
        } else if (arg == "--seed") {
            config.seed = static_cast<std::uint32_t>(parse_u64(arg, require_value()));
        } else if (arg == "--latency-ms") {
            config.latency_ms = parse_double(arg, require_value());
        } else if (arg == "--jitter-ms") {
            config.jitter_ms = parse_double(arg, require_value());
        } else if (arg == "--loss-percent") {
            config.loss_percent = parse_double(arg, require_value());
        } else if (arg == "--server-to-client-latency-ms") {
            config.server_to_client_latency_ms = parse_double(arg, require_value());
        } else if (arg == "--client-to-server-latency-ms") {
            config.client_to_server_latency_ms = parse_double(arg, require_value());
        } else if (arg == "--server-to-client-jitter-ms") {
            config.server_to_client_jitter_ms = parse_double(arg, require_value());
        } else if (arg == "--client-to-server-jitter-ms") {
            config.client_to_server_jitter_ms = parse_double(arg, require_value());
        } else if (arg == "--server-to-client-loss-percent") {
            config.server_to_client_loss_percent = parse_double(arg, require_value());
        } else if (arg == "--client-to-server-loss-percent") {
            config.client_to_server_loss_percent = parse_double(arg, require_value());
        } else if (arg == "--client-mode") {
            config.client_mode = parse_client_mode(require_value());
        } else if (arg == "--interpolation-buffer-frames") {
            config.interpolation_buffer_frames = static_cast<SyncFrame>(parse_u64(arg, require_value()));
        } else if (arg == "--interpolation-buffer-capacity-frames") {
            config.interpolation_buffer_capacity_frames = static_cast<std::size_t>(parse_u64(arg, require_value()));
        } else if (arg == "--time-dilation-min") {
            config.time_dilation_min = parse_double(arg, require_value());
        } else if (arg == "--time-dilation-max") {
            config.time_dilation_max = parse_double(arg, require_value());
        } else if (arg == "--time-dilation-gain") {
            config.time_dilation_gain = parse_double(arg, require_value());
        } else if (arg == "--report") {
            const std::string value = require_value();
            if (value == "json") {
                config.json = true;
            } else if (value == "text") {
                config.json = false;
            } else {
                throw std::invalid_argument("--report must be text or json");
            }
        } else if (arg == "--help" || arg == "-h") {
            throw std::runtime_error("help");
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    return config;
}

inline void write_usage(std::ostream& out) {
    out << "Usage: kage_sync_ball_stress [options]\n"
        << "  --duration-seconds N\n"
        << "  --clients N\n"
        << "  --max-balls N\n"
        << "  --spawn-interval-ms N\n"
        << "  --poison-min N --poison-max N\n"
        << "  --health-min N --health-max N\n"
        << "  --tick-rate N\n"
        << "  --mtu N\n"
        << "  --bandwidth-limit N\n"
        << "  --server-worker-threads N\n"
        << "  --seed N\n"
        << "  --latency-ms N --jitter-ms N --loss-percent N\n"
        << "  --server-to-client-latency-ms N --client-to-server-latency-ms N\n"
        << "  --server-to-client-jitter-ms N --client-to-server-jitter-ms N\n"
        << "  --server-to-client-loss-percent N --client-to-server-loss-percent N\n"
        << "  --client-mode snap|buffered-interpolation\n"
        << "  --interpolation-buffer-frames N\n"
        << "  --interpolation-buffer-capacity-frames N\n"
        << "  --time-dilation-min N --time-dilation-max N --time-dilation-gain N\n"
        << "  --report text|json\n";
}

}  // namespace kage::sync::stress
