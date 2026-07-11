#pragma once

#include "ashiato/sync/simulated_link.hpp"
#include "ashiato/sync/serialization.hpp"
#include "ashiato/sync/sync.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ashiato::sync::stress {

struct BallPosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

}  // namespace ashiato::sync::stress

namespace ashiato::sync {

template <>
struct SyncComponentTraits<stress::BallPosition> {
    using Quantized = stress::BallPosition;

    static void quantize(const stress::BallPosition& value, Quantized& out) {
        out = value;
    }

    static stress::BallPosition dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.write_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
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

}  // namespace ashiato::sync

namespace ashiato::sync::stress {

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

struct BallBounceCue {
    std::uint32_t sequence = 0;
    std::uint8_t energy = 0;
    std::uint8_t padding[7] = {};
};

struct BallSpawnTagged {};
struct BallBounced {};

}  // namespace ashiato::sync::stress

namespace ashiato::sync {

template <>
struct SyncComponentTraits<stress::BallVisual> :
    UnsafeNativeLayoutSyncComponentTraits<stress::BallVisual> {};

template <>
struct SyncComponentTraits<stress::BallHealth> :
    UnsafeNativeLayoutSyncComponentTraits<stress::BallHealth> {};

template <>
struct SyncComponentTraits<stress::BallPoison> :
    UnsafeNativeLayoutSyncComponentTraits<stress::BallPoison> {};

template <>
struct SyncCueTraits<stress::BallBounceCue> {
    static void serialize(
        const stress::BallBounceCue& cue,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext& context) {
        ASHIATO_SERIALIZE_TRACE(out, cue.sequence, 32U, "sequence");
        ASHIATO_SERIALIZE_TRACE(out, cue.energy, 8U, "energy");
        ASHIATO_SERIALIZE_BYTES_TRACE(out, reinterpret_cast<const char*>(cue.padding), sizeof(cue.padding), "padding");
    }

    static bool deserialize(
        ashiato::BitBuffer& in,
        stress::BallBounceCue& out,
        ashiato::ComponentSerializationContext& context) {
        {
            ASHIATO_SERIALIZATION_TRACE_SCOPE("sequence");
            out.sequence = static_cast<std::uint32_t>(in.read_bits(32U));
        }
        {
            ASHIATO_SERIALIZATION_TRACE_SCOPE("energy");
            out.energy = static_cast<std::uint8_t>(in.read_bits(8U));
        }
        {
            ASHIATO_SERIALIZATION_TRACE_SCOPE("padding");
            in.read_bytes(reinterpret_cast<char*>(out.padding), sizeof(out.padding));
        }
        return true;
    }

    static bool play(ashiato::Registry&, ashiato::Entity, const stress::BallBounceCue&, float, SyncFrame) {
        return true;
    }

    static bool rollback(ashiato::Registry&, ashiato::Entity, const stress::BallBounceCue&) {
        return true;
    }

    static bool equals_cue(const stress::BallBounceCue& lhs, const stress::BallBounceCue& rhs) {
        return lhs.sequence == rhs.sequence && lhs.energy == rhs.energy;
    }
};

}  // namespace ashiato::sync

namespace ashiato::sync::stress {

struct SyncSchema {
    SyncArchetypeId ball;
    ashiato::Entity spawn_tagged;
    ashiato::Entity bounced;
};

inline void configure_stress_server_registry(ashiato::Registry& registry) {
    register_components(registry);
    SyncSettings& settings = registry.write<SyncSettings>();
    settings.role = SyncRole::Server;
    settings.local_client = invalid_client_id;
    registry.write<SyncAuthority>().authoritative = true;
}

inline void configure_stress_client_registry(ashiato::Registry& registry, ClientId client) {
    if (client == invalid_client_id || client > max_client_entity_network_id_client) {
        throw std::invalid_argument("client id cannot fit in client entity network ids");
    }
    register_components(registry);
    SyncSettings& settings = registry.write<SyncSettings>();
    settings.role = SyncRole::Client;
    settings.local_client = client;
    registry.write<SyncAuthority>().authoritative = false;
}

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
    SyncFrame buffered_frame_lag = 2;
    double buffered_time_dilation_min = 0.95;
    double buffered_time_dilation_max = 1.05;
    double buffered_time_dilation_gain = 0.05;
    double ping_interval_seconds = 3.0;
    bool wire_diagnostics = false;
    bool json = false;
};

struct ServerBall {
    ashiato::Entity entity;
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
    ClientConnectRequest,
    ServerConnectResponse,
    ClientConnectAck,
    ClientPing,
    ServerPong,
    Unknown
};

struct PacketBreakdown {
    PacketType type = PacketType::Unknown;
    std::uint32_t full_upserts = 0;
    std::uint32_t delta_upserts = 0;
    std::uint32_t destroys = 0;
};

struct WireSlotStats {
    std::uint64_t records = 0;
    std::uint64_t index_bits = 0;
    std::uint64_t payload_bits = 0;
};

struct WireFormatStats {
    static constexpr std::size_t slot_count = 5;

    std::uint64_t packet_bits = 0;
    std::uint64_t padding_bits = 0;
    std::uint64_t server_update_header_bits = 0;
    std::uint64_t server_update_entities = 0;
    std::uint64_t max_server_update_entities_per_packet = 0;
    std::uint64_t max_packet_bytes = 0;
    std::uint64_t destroy_record_bits = 0;
    std::uint64_t full_upsert_bits = 0;
    std::uint64_t full_upsert_payload_bits = 0;
    std::uint64_t full_upsert_slot_list_records = 0;
    std::uint64_t full_upsert_presence_mask_records = 0;
    std::uint64_t full_upsert_presence_mask_bits = 0;
    std::uint64_t delta_upsert_bits = 0;
    std::uint64_t delta_upsert_payload_bits = 0;
    std::uint64_t delta_baseline_bits = 0;
    std::uint64_t delta_baseline_relative = 0;
    std::uint64_t delta_baseline_absolute = 0;
    std::uint64_t delta_change_mask_bits = 0;
    std::uint64_t cue_records = 0;
    std::uint64_t cue_record_bits = 0;
    std::uint64_t cue_payload_bits = 0;
    std::uint64_t ack_header_bits = 0;
    std::uint64_t ack_records = 0;
    std::uint64_t ack_record_bits = 0;
    std::array<WireSlotStats, slot_count> slots{};
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
    std::uint64_t client_connect_request_packets = 0;
    std::uint64_t client_connect_request_bytes = 0;
    std::uint64_t server_connect_response_packets = 0;
    std::uint64_t server_connect_response_bytes = 0;
    std::uint64_t client_connect_ack_packets = 0;
    std::uint64_t client_connect_ack_bytes = 0;
    std::uint64_t client_ping_packets = 0;
    std::uint64_t client_ping_bytes = 0;
    std::uint64_t server_pong_packets = 0;
    std::uint64_t server_pong_bytes = 0;
    std::uint64_t unknown_packets = 0;
    std::uint64_t unknown_bytes = 0;
    std::uint64_t full_upserts = 0;
    std::uint64_t delta_upserts = 0;
    std::uint64_t destroys = 0;
    WireFormatStats wire;
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
    double average_measured_buffered_frame_lag = 0.0;
    double average_buffered_time_dilation = 0.0;
    SyncFrame max_desired_buffered_frame_lag = 0;
    SyncFrame max_target_buffered_frame_lag = 0;
    SyncFrame max_current_buffered_frame_lag = 0;
    std::uint64_t server_update_packets_received = 0;
    std::uint64_t server_update_packets_missing = 0;
    std::uint64_t server_update_packets_reordered_or_duplicate = 0;
    double server_update_packet_loss = 0.0;
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
    std::uint64_t spawn_tags_added = 0;
    std::uint64_t bounce_tags_added = 0;
    std::uint64_t bounce_cues_emitted = 0;
    std::uint32_t live_balls = 0;
};

using SimulatedLink = ::ashiato::sync::SimulatedLink<ashiato::BitBuffer, ClientId>;

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
    case PacketType::ClientConnectRequest:
        return "client_connect_request";
    case PacketType::ServerConnectResponse:
        return "server_connect_response";
    case PacketType::ClientConnectAck:
        return "client_connect_ack";
    case PacketType::ClientPing:
        return "client_ping";
    case PacketType::ServerPong:
        return "server_pong";
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
    case ReplicationClientMode::Predict:
        return "predict";
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

inline void add_packet_stats(DirectionStats& stats, const ashiato::BitBuffer& packet, const PacketBreakdown& breakdown) {
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
    case PacketType::ClientConnectRequest:
        ++stats.client_connect_request_packets;
        stats.client_connect_request_bytes += bytes;
        break;
    case PacketType::ServerConnectResponse:
        ++stats.server_connect_response_packets;
        stats.server_connect_response_bytes += bytes;
        break;
    case PacketType::ClientConnectAck:
        ++stats.client_connect_ack_packets;
        stats.client_connect_ack_bytes += bytes;
        break;
    case PacketType::ClientPing:
        ++stats.client_ping_packets;
        stats.client_ping_bytes += bytes;
        break;
    case PacketType::ServerPong:
        ++stats.server_pong_packets;
        stats.server_pong_bytes += bytes;
        break;
    case PacketType::Unknown:
        ++stats.unknown_packets;
        stats.unknown_bytes += bytes;
        break;
    }
}

inline const char* wire_slot_name(std::size_t slot) {
    switch (slot) {
    case 0:
        return "tags";
    case 1:
        return "BallPosition";
    case 2:
        return "BallVisual";
    case 3:
        return "BallHealth";
    case 4:
        return "BallPoison";
    }
    return "unknown";
}

inline std::size_t stress_slot_payload_bits(std::size_t slot) {
    switch (slot) {
    case 0:
        return 2U;
    case 1:
        return sizeof(BallPosition) * 8U;
    case 2:
        return sizeof(BallVisual) * 8U;
    case 3:
        return sizeof(BallHealth) * 8U;
    case 4:
        return sizeof(BallPoison) * 8U;
    }
    return 0;
}

inline std::size_t stress_slot_index_bits() {
    return protocol::bits_for_range(WireFormatStats::slot_count);
}

inline void record_wire_slot(WireFormatStats& wire, std::size_t slot, bool has_index) {
    if (slot >= WireFormatStats::slot_count) {
        return;
    }
    WireSlotStats& stats = wire.slots[slot];
    ++stats.records;
    if (has_index) {
        stats.index_bits += stress_slot_index_bits();
    }
    stats.payload_bits += stress_slot_payload_bits(slot);
}

inline PacketBreakdown classify_packet(
    ashiato::BitBuffer packet,
    WireFormatStats* wire = nullptr,
    std::size_t network_entity_id_tier0_bits = protocol::default_network_entity_id_tier0_bits) {
    PacketBreakdown result;
    try {
        if (packet.remaining_bits() < protocol::message_bits) {
            return result;
        }
        if (wire != nullptr) {
            wire->packet_bits += packet.bit_size();
            wire->padding_bits += packet.byte_size() * 8U - packet.bit_size();
            wire->max_packet_bytes = std::max<std::uint64_t>(wire->max_packet_bytes, packet.byte_size());
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits));
        if (message == protocol::client_ack_message) {
            result.type = PacketType::ClientAck;
            if (wire != nullptr) {
                if (packet.remaining_bits() < protocol::ack_count_bits) {
                    result = PacketBreakdown{};
                    return result;
                }
                const auto ack_count = static_cast<std::uint16_t>(packet.read_bits(protocol::ack_count_bits));
                packet.skip_bits(static_cast<std::size_t>(ack_count) * protocol::client_ack_record_bits);
                wire->ack_header_bits += protocol::client_ack_header_bits;
                wire->ack_records += ack_count;
                wire->ack_record_bits +=
                    static_cast<std::uint64_t>(ack_count) * protocol::client_ack_record_bits;
            }
            return result;
        }
        if (message == protocol::client_connect_request_message) {
            result.type = PacketType::ClientConnectRequest;
            return result;
        }
        if (message == protocol::server_connect_response_message) {
            result.type = PacketType::ServerConnectResponse;
            return result;
        }
        if (message == protocol::client_connect_ack_message) {
            result.type = PacketType::ClientConnectAck;
            return result;
        }
        if (message == protocol::client_ping_message) {
            result.type = PacketType::ClientPing;
            return result;
        }
        if (message == protocol::server_pong_message) {
            result.type = PacketType::ServerPong;
            return result;
        }
        if (message != protocol::server_update_message) {
            return result;
        }

        result.type = PacketType::ServerUpdate;
        const auto frame = static_cast<std::uint32_t>(packet.read_bits(32U));
        packet.read_bits(protocol::server_packet_id_bits);
        packet.read_bits(32U);
        const auto entity_count = static_cast<std::uint16_t>(packet.read_bits(16U));
        if (wire != nullptr) {
            wire->server_update_header_bits += protocol::server_update_header_bits;
            wire->server_update_entities += entity_count;
            wire->max_server_update_entities_per_packet =
                std::max<std::uint64_t>(wire->max_server_update_entities_per_packet, entity_count);
        }
        for (std::uint16_t entity_index = 0; entity_index < entity_count; ++entity_index) {
            const std::size_t record_start = packet.read_offset_bits();
            const bool destroy = packet.read_bool();
            const std::size_t network_id_start = packet.read_offset_bits();
            std::uint32_t network_id = 0;
            if (!protocol::read_network_entity_id(packet, network_id, network_entity_id_tier0_bits) ||
                network_id == 0U) {
                result = PacketBreakdown{};
                return result;
            }
            const std::uint64_t network_id_bits = packet.read_offset_bits() - network_id_start;
            if (destroy) {
                ++result.destroys;
                if (wire != nullptr) {
                    wire->destroy_record_bits += packet.read_offset_bits() - record_start;
                }
                continue;
            }

            const bool full = packet.read_bool();
            if (full) {
                ++result.full_upserts;
                packet.read_bits(32U);
                const bool uses_presence_mask = packet.read_bool();
                std::uint64_t presence_mask = 0;
                std::uint16_t component_count = 0;
                if (uses_presence_mask) {
                    presence_mask = packet.read_unsigned_bits(WireFormatStats::slot_count);
                    for (std::size_t slot = 0; slot < WireFormatStats::slot_count; ++slot) {
                        if ((presence_mask & (std::uint64_t{1} << slot)) != 0U) {
                            ++component_count;
                        }
                    }
                    if (wire != nullptr) {
                        ++wire->full_upsert_presence_mask_records;
                        wire->full_upsert_presence_mask_bits += WireFormatStats::slot_count;
                    }
                } else {
                    component_count = static_cast<std::uint16_t>(packet.read_bits(16U));
                    if (wire != nullptr) {
                        ++wire->full_upsert_slot_list_records;
                    }
                }

                auto read_full_slot = [&](std::uint16_t component_index) {
                    switch (component_index) {
                        case 0:
                            packet.skip_bits(2U);
                            if (wire != nullptr) {
                                record_wire_slot(*wire, 0, !uses_presence_mask);
                            }
                            return true;
                        case 1:
                            packet.skip_bits(sizeof(BallPosition) * 8U);
                            if (wire != nullptr) {
                                record_wire_slot(*wire, 1, !uses_presence_mask);
                            }
                            return true;
                        case 2:
                            packet.skip_bits(sizeof(BallVisual) * 8U);
                            if (wire != nullptr) {
                                record_wire_slot(*wire, 2, !uses_presence_mask);
                            }
                            return true;
                        case 3:
                            packet.skip_bits(sizeof(BallHealth) * 8U);
                            if (wire != nullptr) {
                                record_wire_slot(*wire, 3, !uses_presence_mask);
                            }
                            return true;
                        case 4:
                            packet.skip_bits(sizeof(BallPoison) * 8U);
                            if (wire != nullptr) {
                                record_wire_slot(*wire, 4, !uses_presence_mask);
                            }
                            return true;
                        default:
                            return false;
                    }
                };

                if (uses_presence_mask) {
                    for (std::size_t slot = 0; slot < WireFormatStats::slot_count; ++slot) {
                        if ((presence_mask & (std::uint64_t{1} << slot)) == 0U) {
                            continue;
                        }
                        if (!read_full_slot(static_cast<std::uint16_t>(slot))) {
                            result = PacketBreakdown{};
                            return result;
                        }
                    }
                } else {
                    for (std::uint16_t component = 0; component < component_count; ++component) {
                        const auto component_index =
                            static_cast<std::uint16_t>(packet.read_bits(stress_slot_index_bits()));
                        if (!read_full_slot(component_index)) {
                            result = PacketBreakdown{};
                            return result;
                        }
                    }
                }
                const std::uint64_t cue_tail_start = packet.read_offset_bits();
                while (packet.read_bool()) {
                        SyncFrame cue_frame = 0;
                        packet.skip_bits(32U);
                        if (!protocol::read_cue_frame(packet, frame, cue_frame)) {
                            result = PacketBreakdown{};
                            return result;
                        }
                        packet.skip_bits(16U);
                        constexpr std::size_t payload_bits = 32U + 8U + sizeof(BallBounceCue::padding) * 8U;
                        packet.skip_bits(payload_bits);
                        if (wire != nullptr) {
                            ++wire->cue_records;
                            wire->cue_payload_bits += payload_bits;
                        }
                }
                const std::uint64_t cue_tail_bits = packet.read_offset_bits() - cue_tail_start;
                if (wire != nullptr) {
                    wire->cue_record_bits += cue_tail_bits;
                }
                if (wire != nullptr) {
                    const std::uint64_t record_bits = packet.read_offset_bits() - record_start;
                    const std::uint64_t full_index_bits = uses_presence_mask
                        ? WireFormatStats::slot_count
                        : 16U + static_cast<std::uint64_t>(component_count) * stress_slot_index_bits();
                    wire->full_upsert_bits += record_bits;
                    wire->full_upsert_payload_bits +=
                        record_bits -
                        (1U + network_id_bits + 1U + 32U + 1U + full_index_bits) -
                        cue_tail_bits;
                }
            } else {
                ++result.delta_upserts;
                bool uses_delta_baseline = false;
                std::uint64_t baseline_value = 0;
                if (!serialization::read_varint2_raw(
                        packet,
                        protocol::baseline_frame_delta_bits,
                        32U,
                        uses_delta_baseline,
                        baseline_value)) {
                    result = PacketBreakdown{};
                    return result;
                }
                if (uses_delta_baseline) {
                    if (wire != nullptr) {
                        wire->delta_baseline_bits += 1U + protocol::baseline_frame_delta_bits;
                        ++wire->delta_baseline_relative;
                    }
                } else {
                    if (wire != nullptr) {
                        wire->delta_baseline_bits += 33U;
                        ++wire->delta_baseline_absolute;
                    }
                }
                if (wire != nullptr) {
                    wire->delta_change_mask_bits += WireFormatStats::slot_count;
                }
                bool changed[WireFormatStats::slot_count] = {};
                for (std::size_t slot = 0; slot < WireFormatStats::slot_count; ++slot) {
                    changed[slot] = packet.read_bool();
                }
                if (changed[0]) {
                    packet.skip_bits(2U);
                    if (wire != nullptr) {
                        record_wire_slot(*wire, 0, false);
                    }
                }
                for (std::size_t slot = 1; slot < WireFormatStats::slot_count; ++slot) {
                    if (!changed[slot]) {
                        continue;
                    }
                    switch (slot) {
                        case 1:
                            packet.skip_bits(sizeof(BallPosition) * 8U);
                            break;
                        case 2:
                            packet.skip_bits(sizeof(BallVisual) * 8U);
                            break;
                        case 3:
                            packet.skip_bits(sizeof(BallHealth) * 8U);
                            break;
                        case 4:
                            packet.skip_bits(sizeof(BallPoison) * 8U);
                            break;
                    }
                    if (wire != nullptr) {
                        record_wire_slot(*wire, slot, false);
                    }
                }
                const std::uint64_t cue_tail_start = packet.read_offset_bits();
                while (packet.read_bool()) {
                        SyncFrame cue_frame = 0;
                        packet.skip_bits(32U);
                        if (!protocol::read_cue_frame(packet, frame, cue_frame)) {
                            result = PacketBreakdown{};
                            return result;
                        }
                        packet.skip_bits(16U);
                        constexpr std::size_t payload_bits = 32U + 8U + sizeof(BallBounceCue::padding) * 8U;
                        packet.skip_bits(payload_bits);
                        if (wire != nullptr) {
                            ++wire->cue_records;
                            wire->cue_payload_bits += payload_bits;
                        }
                }
                const std::uint64_t cue_tail_bits = packet.read_offset_bits() - cue_tail_start;
                if (wire != nullptr) {
                    wire->cue_record_bits += cue_tail_bits;
                }
                if (wire != nullptr) {
                    const std::uint64_t record_bits = packet.read_offset_bits() - record_start;
                    wire->delta_upsert_bits += record_bits;
                    wire->delta_upsert_payload_bits +=
                        record_bits - (1U + network_id_bits + 1U) -
                        (uses_delta_baseline ? 1U + protocol::baseline_frame_delta_bits : 33U) -
                        WireFormatStats::slot_count -
                        cue_tail_bits;
                }
            }
        }
    } catch (const std::exception&) {
        result = PacketBreakdown{};
    }
    return result;
}

inline void enqueue_packet(
    SimulatedLink& link,
    DirectionStats& stats,
    ClientId client,
    const ashiato::BitBuffer& packet,
    double now_seconds,
    bool wire_diagnostics = false) {
    const PacketBreakdown breakdown = classify_packet(packet, wire_diagnostics ? &stats.wire : nullptr);
    add_packet_stats(stats, packet, breakdown);

    if (!link.enqueue(client, packet, now_seconds)) {
        ++stats.dropped_packets;
        stats.dropped_bytes += packet.byte_size();
        return;
    }
}

template <typename Fn>
void deliver_ready(SimulatedLink& link, DirectionStats& stats, double now_seconds, Fn&& fn) {
    link.deliver_ready(now_seconds, [&](ClientId client, const ashiato::BitBuffer& packet) {
        ++stats.delivered_packets;
        stats.delivered_bytes += packet.byte_size();
        fn(client, packet);
    });
}

inline SyncSchema define_schema(ashiato::Registry& registry, bool interpolate_position = false) {
    const ashiato::Entity position = register_sync_component<BallPosition>(registry, "BallPosition");
    const ashiato::Entity visual = register_sync_component<BallVisual>(registry, "BallVisual");
    const ashiato::Entity health = register_sync_component<BallHealth>(registry, "BallHealth");
    const ashiato::Entity poison = register_sync_component<BallPoison>(registry, "BallPoison");
    register_sync_cue<BallBounceCue>(registry);
    const ashiato::Entity spawn_tagged = registry.register_component<BallSpawnTagged>("BallSpawnTagged");
    const ashiato::Entity bounced = registry.register_component<BallBounced>("BallBounced");
    return SyncSchema{
        define_archetype(
            registry,
            SyncArchetypeDesc{
                "StressBall",
                {
                    {spawn_tagged, ReplicationAudience::All},
                    {bounced, ReplicationAudience::All},
                },
                {
                    {position,
                     ReplicationAudience::All,
                     interpolate_position ? ComponentInterpolation::Interpolate : ComponentInterpolation::Step},
                    {visual, ReplicationAudience::All},
                    {health, ReplicationAudience::All},
                    {poison, ReplicationAudience::All},
                }}),
        spawn_tagged,
        bounced};
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
    if (config.latency_ms < 0.0 ||
        config.jitter_ms < 0.0 ||
        config.server_to_client_latency_ms < 0.0 ||
        config.client_to_server_latency_ms < 0.0 ||
        config.server_to_client_jitter_ms < 0.0 ||
        config.client_to_server_jitter_ms < 0.0) {
        throw std::invalid_argument("latency and jitter values must be non-negative");
    }
    if (config.buffered_frame_lag >= ReplicationClient::buffered_frame_capacity) {
        throw std::invalid_argument("--buffered-frame-lag must be smaller than capacity");
    }
    if (config.buffered_time_dilation_min <= 0.0 || config.buffered_time_dilation_min > 1.0) {
        throw std::invalid_argument("--buffered-time-dilation-min must be in the range (0, 1]");
    }
    if (config.buffered_time_dilation_max < 1.0) {
        throw std::invalid_argument("--buffered-time-dilation-max must be at least 1");
    }
    if (config.buffered_time_dilation_gain < 0.0) {
        throw std::invalid_argument("--buffered-time-dilation-gain must be non-negative");
    }
    if (config.ping_interval_seconds <= 0.0) {
        throw std::invalid_argument("--ping-interval-seconds must be positive");
    }
}

inline ReplicationPrioritizerFn make_sphere_prioritizer(ashiato::Registry& registry) {
    static constexpr float inner_filter_radius_sq = 0.75f * 0.75f;
    static constexpr float priority_radius_sq = 12.0f * 12.0f;
    static constexpr float priority_scale = 1000.0f;

    return [&registry](ClientId, ReplicationPriorityObject object) {
        ReplicationPriorityDecision decision;
        decision.component_mask = std::numeric_limits<std::uint64_t>::max();

        const BallPosition* position = registry.try_get<BallPosition>(object.entity);
        if (position == nullptr) {
            decision.priority = 0.0f;
            return decision;
        }

        const float distance_sq =
            position->x * position->x + position->y * position->y + position->z * position->z;
        if (distance_sq <= inner_filter_radius_sq) {
            decision.priority = 0.0f;
            return decision;
        }

        const float clamped_distance_sq = distance_sq < priority_radius_sq ? distance_sq : priority_radius_sq;
        const float normalized = (priority_radius_sq - clamped_distance_sq) /
            (priority_radius_sq - inner_filter_radius_sq);
        decision.priority = 1.0f + normalized * priority_scale;
        return decision;
    };
}

inline void spawn_ball(
    ashiato::Registry& registry,
    std::vector<ServerBall>& balls,
    SyncSchema schema,
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

    const ashiato::Entity entity = registry.create();
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
    std::bernoulli_distribution spawn_tag(0.35);
    if (spawn_tag(rng)) {
        registry.add_tag(entity, schema.spawn_tagged);
        ++report.spawn_tags_added;
    }
    registry.add<Replicated>(entity, Replicated{schema.ball});

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
    ashiato::Registry& registry,
    ashiato::Entity entity,
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
    ashiato::Registry& registry,
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
        spawn_ball(registry, balls, schema, config, rng, report);
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
            if (!registry.has(ball.entity, schema.bounced)) {
                registry.add_tag(ball.entity, schema.bounced);
                ++report.bounce_tags_added;
            }
            BallBounceCue cue;
            cue.sequence = static_cast<std::uint32_t>(++report.bounce_cues_emitted);
            cue.energy = 255U;
            (void)registry.write<CueDispatcher>().emit(
                registry.get<SyncSettings>(),
                static_cast<SyncFrame>(report.ticks + 1U),
                ball.entity,
                cue,
                0.25f);
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

inline std::size_t count_client_entities(ashiato::Registry& registry) {
    std::size_t count = 0;
    registry.view<const BallPosition>().each([&](ashiato::Entity, const BallPosition&) {
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

    ashiato::Registry server_registry;
    configure_stress_server_registry(server_registry);
    const SyncSchema server_schema = define_schema(server_registry);

    std::vector<ashiato::Registry> client_registries(config.clients);
    for (std::uint32_t client_index = 0; client_index < config.clients; ++client_index) {
        configure_stress_client_registry(client_registries[client_index], static_cast<ClientId>(client_index + 1U));
        define_schema(
            client_registries[client_index],
            config.client_mode == ReplicationClientMode::BufferedInterpolation);
    }

    SimulatedLink server_to_clients;
    server_to_clients.settings.latency_ms = config.server_to_client_latency_ms;
    server_to_clients.settings.jitter_ms = config.server_to_client_jitter_ms;
    server_to_clients.settings.loss_percent = config.server_to_client_loss_percent;
    server_to_clients.random_engine().seed(config.seed ^ 0x5EED1234U);

    SimulatedLink clients_to_server;
    clients_to_server.settings.latency_ms = config.client_to_server_latency_ms;
    clients_to_server.settings.jitter_ms = config.client_to_server_jitter_ms;
    clients_to_server.settings.loss_percent = config.client_to_server_loss_percent;
    clients_to_server.random_engine().seed(config.seed ^ 0xA11CE55U);

    ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = config.bandwidth_limit;
    server_options.mtu_bytes = config.mtu;
    server_options.fixed_dt_seconds = 1.0 / config.tick_rate;
    server_options.prioritizer = make_sphere_prioritizer(server_registry);
    server_options.transport = [&](ClientId client, const ashiato::BitBuffer& packet) {
        enqueue_packet(
            server_to_clients,
            report.server_to_clients,
            client,
            packet,
            static_cast<double>(report.ticks) / config.tick_rate,
            config.wire_diagnostics);
    };
    ReplicationServer server(server_registry, server_options);
    for (std::uint32_t client_index = 0; client_index < config.clients; ++client_index) {
        server.add_client(static_cast<ClientId>(client_index + 1U));
    }

    std::vector<ReplicationClient> clients;
    clients.reserve(config.clients);
    for (std::uint32_t client_index = 0; client_index < config.clients; ++client_index) {
        ReplicationClientOptions client_options;
        client_options.network.mtu_bytes = config.mtu;
        client_options.entities.default_mode = config.client_mode;
        client_options.buffered.buffered_frame_lag = config.buffered_frame_lag;
        client_options.buffered.auto_buffered_frame_lag = true;
        client_options.buffered.auto_buffered_frame_lag_min = 1;
        client_options.buffered.auto_buffered_frame_lag_jitter_multiplier = 2.0f;
        client_options.buffered.auto_buffered_frame_lag_smoothing = 0.1f;
        client_options.buffered.auto_buffered_time_dilation_min = static_cast<float>(config.buffered_time_dilation_min);
        client_options.buffered.auto_buffered_time_dilation_max = static_cast<float>(config.buffered_time_dilation_max);
        client_options.buffered.auto_buffered_time_dilation_gain = static_cast<float>(config.buffered_time_dilation_gain);
        client_options.clock.fixed_dt_seconds = 1.0 / config.tick_rate;
        client_options.session.ping_interval_seconds = config.ping_interval_seconds;
        clients.emplace_back(client_registries[client_index], std::move(client_options));
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
            deliver_ready(server_to_clients, report.server_to_clients, now, [&](ClientId client_id, const ashiato::BitBuffer& packet) {
                const std::size_t index = static_cast<std::size_t>(client_id - 1U);
                if (index < clients.size()) {
                    clients[index].receive(client_registries[index], packet);
                }
            });
            if (config.client_mode == ReplicationClientMode::BufferedInterpolation) {
                for (std::size_t index = 0; index < clients.size(); ++index) {
                    client_accumulators[index] += dt * clients[index].timing_stats().buffered_time_dilation;
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
                for (const ashiato::BitBuffer& ack : clients[index].drain_packets()) {
                    enqueue_packet(
                        clients_to_server,
                        report.clients_to_server,
                        static_cast<ClientId>(index + 1U),
                        ack,
                        now,
                        config.wire_diagnostics);
                }
            }
            deliver_ready(clients_to_server, report.clients_to_server, now, [&](ClientId client_id, const ashiato::BitBuffer& packet) {
                server.process_packet(server_registry, client_id, packet);
            });
        }

        {
            ScopedTimer timer(report.timing.server_simulation_seconds);
            update_server_world(server_registry, balls, server_schema, config, dt, spawn_accumulator, simulation_rng, report);
        }

        {
            ScopedTimer timer(report.timing.server_replication_seconds);
            server.tick(server_registry, server.options().fixed_dt_seconds);
        }

        if ((tick % 16U) == 0U) {
            report.memory.rss_peak_bytes = std::max(report.memory.rss_peak_bytes, current_rss_bytes());
        }
    }

    const double end_time = static_cast<double>(total_ticks) * dt + 60.0;
    deliver_ready(server_to_clients, report.server_to_clients, end_time, [&](ClientId client_id, const ashiato::BitBuffer& packet) {
        const std::size_t index = static_cast<std::size_t>(client_id - 1U);
        if (index < clients.size()) {
            clients[index].receive(client_registries[index], packet);
            if (config.client_mode == ReplicationClientMode::BufferedInterpolation) {
                clients[index].apply_frame(client_registries[index], client_frames[index]);
            }
        }
    });
    for (std::size_t index = 0; index < clients.size(); ++index) {
        for (const ashiato::BitBuffer& ack : clients[index].drain_packets()) {
            enqueue_packet(
                clients_to_server,
                report.clients_to_server,
                static_cast<ClientId>(index + 1U),
                ack,
                end_time,
                config.wire_diagnostics);
        }
    }
    deliver_ready(clients_to_server, report.clients_to_server, end_time + 60.0, [&](ClientId client_id, const ashiato::BitBuffer& packet) {
        server.process_packet(server_registry, client_id, packet);
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
        report.client_timing.average_measured_buffered_frame_lag +=
            timing.measured_buffered_frame_lag;
        report.client_timing.average_buffered_time_dilation += timing.buffered_time_dilation;
        report.client_timing.max_desired_buffered_frame_lag = std::max(
            report.client_timing.max_desired_buffered_frame_lag,
            timing.desired_buffered_frame_lag);
        report.client_timing.max_target_buffered_frame_lag = std::max(
            report.client_timing.max_target_buffered_frame_lag,
            timing.target_buffered_frame_lag);
        report.client_timing.max_current_buffered_frame_lag = std::max(
            report.client_timing.max_current_buffered_frame_lag,
            timing.current_buffered_frame_lag);
        report.client_timing.server_update_packets_received += timing.server_update_packets_received;
        report.client_timing.server_update_packets_missing += timing.server_update_packets_missing;
        report.client_timing.server_update_packets_reordered_or_duplicate +=
            timing.server_update_packets_reordered_or_duplicate;
    }
    if (!clients.empty()) {
        report.client_timing.average_latency_frames /= static_cast<double>(clients.size());
        report.client_timing.average_jitter_frames /= static_cast<double>(clients.size());
        report.client_timing.average_measured_buffered_frame_lag /= static_cast<double>(clients.size());
        report.client_timing.average_buffered_time_dilation /= static_cast<double>(clients.size());
    }
    const std::uint64_t server_update_packet_total =
        report.client_timing.server_update_packets_received +
        report.client_timing.server_update_packets_missing;
    report.client_timing.server_update_packet_loss = server_update_packet_total == 0U
        ? 0.0
        : static_cast<double>(report.client_timing.server_update_packets_missing) /
            static_cast<double>(server_update_packet_total);
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
    out << "  client_connect_request packets=" << stats.client_connect_request_packets
        << " bytes=" << stats.client_connect_request_bytes << '\n';
    out << "  server_connect_response packets=" << stats.server_connect_response_packets
        << " bytes=" << stats.server_connect_response_bytes << '\n';
    out << "  client_connect_ack packets=" << stats.client_connect_ack_packets
        << " bytes=" << stats.client_connect_ack_bytes << '\n';
    out << "  client_ping packets=" << stats.client_ping_packets << " bytes=" << stats.client_ping_bytes << '\n';
    out << "  server_pong packets=" << stats.server_pong_packets << " bytes=" << stats.server_pong_bytes << '\n';
    out << "  unknown packets=" << stats.unknown_packets << " bytes=" << stats.unknown_bytes << '\n';
}

inline void write_wire_text(
    std::ostream& out,
    const char* name,
    const DirectionStats& stats,
    std::size_t mtu_bytes) {
    const WireFormatStats& wire = stats.wire;
    const double average_mtu_fill = stats.packets == 0 || mtu_bytes == 0
        ? 0.0
        : static_cast<double>(stats.bytes) / static_cast<double>(stats.packets * mtu_bytes);
    out << "wire_format " << name
        << " packet_bits=" << wire.packet_bits
        << " padding_bits=" << wire.padding_bits
        << " max_packet_bytes=" << wire.max_packet_bytes
        << " average_mtu_fill=" << average_mtu_fill
        << " server_update_header_bits=" << wire.server_update_header_bits
        << " server_update_entities=" << wire.server_update_entities
        << " max_server_update_entities_per_packet=" << wire.max_server_update_entities_per_packet
        << " destroy_record_bits=" << wire.destroy_record_bits
        << " full_upsert_bits=" << wire.full_upsert_bits
        << " full_upsert_payload_bits=" << wire.full_upsert_payload_bits
        << " full_upsert_slot_list_records=" << wire.full_upsert_slot_list_records
        << " full_upsert_presence_mask_records=" << wire.full_upsert_presence_mask_records
        << " full_upsert_presence_mask_bits=" << wire.full_upsert_presence_mask_bits
        << " delta_upsert_bits=" << wire.delta_upsert_bits
        << " delta_upsert_payload_bits=" << wire.delta_upsert_payload_bits
        << " delta_baseline_bits=" << wire.delta_baseline_bits
        << " delta_baseline_relative=" << wire.delta_baseline_relative
        << " delta_baseline_absolute=" << wire.delta_baseline_absolute
        << " delta_change_mask_bits=" << wire.delta_change_mask_bits
        << " cue_records=" << wire.cue_records
        << " cue_record_bits=" << wire.cue_record_bits
        << " cue_payload_bits=" << wire.cue_payload_bits
        << " ack_header_bits=" << wire.ack_header_bits
        << " ack_records=" << wire.ack_records
        << " ack_record_bits=" << wire.ack_record_bits << '\n';
    for (std::size_t slot = 0; slot < WireFormatStats::slot_count; ++slot) {
        const WireSlotStats& slot_stats = wire.slots[slot];
        out << "  slot name=" << wire_slot_name(slot)
            << " records=" << slot_stats.records
            << " index_bits=" << slot_stats.index_bits
            << " payload_bits=" << slot_stats.payload_bits << '\n';
    }
}

inline void write_report_text(std::ostream& out, const StressReport& report) {
    out << "ashiato-sync ball stress\n";
    out << "ticks=" << report.ticks << " clients=" << report.config.clients
        << " live_balls=" << report.live_balls << " spawned=" << report.spawned
        << " despawned=" << report.despawned << '\n';
    out << "client_mode=" << client_mode_name(report.config.client_mode)
        << " buffered_frame_lag=" << report.config.buffered_frame_lag
        << " ping_interval_seconds=" << report.config.ping_interval_seconds
        << " wire_diagnostics=" << (report.config.wire_diagnostics ? "true" : "false") << '\n';
    out << "network latency_ms=" << report.config.latency_ms
        << " jitter_ms=" << report.config.jitter_ms
        << " server_to_client_latency_ms=" << report.config.server_to_client_latency_ms
        << " server_to_client_jitter_ms=" << report.config.server_to_client_jitter_ms
        << " client_to_server_latency_ms=" << report.config.client_to_server_latency_ms
        << " client_to_server_jitter_ms=" << report.config.client_to_server_jitter_ms << '\n';
    out << "client_timing samples=" << report.client_timing.sample_count
        << " average_latency_frames=" << report.client_timing.average_latency_frames
        << " average_jitter_frames=" << report.client_timing.average_jitter_frames
        << " average_measured_buffered_frame_lag="
        << report.client_timing.average_measured_buffered_frame_lag
        << " average_buffered_time_dilation=" << report.client_timing.average_buffered_time_dilation
        << " desired_buffered_frame_lag="
        << report.client_timing.max_desired_buffered_frame_lag
        << " target_buffered_frame_lag=" << report.client_timing.max_target_buffered_frame_lag
        << " current_buffered_frame_lag=" << report.client_timing.max_current_buffered_frame_lag
        << " server_update_packets_received=" << report.client_timing.server_update_packets_received
        << " server_update_packets_missing=" << report.client_timing.server_update_packets_missing
        << " server_update_packets_reordered_or_duplicate="
        << report.client_timing.server_update_packets_reordered_or_duplicate
        << " server_update_packet_loss=" << report.client_timing.server_update_packet_loss << '\n';
    out << "poison added_components=" << report.poison_components_added
        << " removed_components=" << report.poison_components_removed
        << " ticks=" << report.poison_ticks << '\n';
    out << "tags spawn_added=" << report.spawn_tags_added
        << " bounce_added=" << report.bounce_tags_added << '\n';
    out << "cues bounce_emitted=" << report.bounce_cues_emitted << '\n';
    out << std::fixed << std::setprecision(6);
    out << "timing wall=" << report.timing.wall_seconds
        << " server_sim=" << report.timing.server_simulation_seconds
        << " server_replication=" << report.timing.server_replication_seconds
        << " client_receive=" << report.timing.client_receive_seconds
        << " ack_processing=" << report.timing.ack_processing_seconds << '\n';
    write_direction_text(out, "server_to_clients", report.server_to_clients);
    write_direction_text(out, "clients_to_server", report.clients_to_server);
    if (report.config.wire_diagnostics) {
        write_wire_text(out, "server_to_clients", report.server_to_clients, report.config.mtu);
        write_wire_text(out, "clients_to_server", report.clients_to_server, report.config.mtu);
    }
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
        << "\"client_connect_request\":{\"packets\":" << stats.client_connect_request_packets
        << ",\"bytes\":" << stats.client_connect_request_bytes << "},"
        << "\"server_connect_response\":{\"packets\":" << stats.server_connect_response_packets
        << ",\"bytes\":" << stats.server_connect_response_bytes << "},"
        << "\"client_connect_ack\":{\"packets\":" << stats.client_connect_ack_packets
        << ",\"bytes\":" << stats.client_connect_ack_bytes << "},"
        << "\"client_ping\":{\"packets\":" << stats.client_ping_packets
        << ",\"bytes\":" << stats.client_ping_bytes << "},"
        << "\"server_pong\":{\"packets\":" << stats.server_pong_packets
        << ",\"bytes\":" << stats.server_pong_bytes << "},"
        << "\"unknown\":{\"packets\":" << stats.unknown_packets << ",\"bytes\":" << stats.unknown_bytes << "}"
        << "}";
}

inline void write_wire_json(std::ostream& out, const DirectionStats& stats, std::size_t mtu_bytes) {
    const WireFormatStats& wire = stats.wire;
    const double average_mtu_fill = stats.packets == 0 || mtu_bytes == 0
        ? 0.0
        : static_cast<double>(stats.bytes) / static_cast<double>(stats.packets * mtu_bytes);
    out << "{"
        << "\"packet_bits\":" << wire.packet_bits << ","
        << "\"padding_bits\":" << wire.padding_bits << ","
        << "\"max_packet_bytes\":" << wire.max_packet_bytes << ","
        << "\"average_mtu_fill\":" << average_mtu_fill << ","
        << "\"server_update_header_bits\":" << wire.server_update_header_bits << ","
        << "\"server_update_entities\":" << wire.server_update_entities << ","
        << "\"max_server_update_entities_per_packet\":" << wire.max_server_update_entities_per_packet << ","
        << "\"destroy_record_bits\":" << wire.destroy_record_bits << ","
        << "\"full_upsert_bits\":" << wire.full_upsert_bits << ","
        << "\"full_upsert_payload_bits\":" << wire.full_upsert_payload_bits << ","
        << "\"full_upsert_slot_list_records\":" << wire.full_upsert_slot_list_records << ","
        << "\"full_upsert_presence_mask_records\":" << wire.full_upsert_presence_mask_records << ","
        << "\"full_upsert_presence_mask_bits\":" << wire.full_upsert_presence_mask_bits << ","
        << "\"delta_upsert_bits\":" << wire.delta_upsert_bits << ","
        << "\"delta_upsert_payload_bits\":" << wire.delta_upsert_payload_bits << ","
        << "\"delta_baseline_bits\":" << wire.delta_baseline_bits << ","
        << "\"delta_baseline_relative\":" << wire.delta_baseline_relative << ","
        << "\"delta_baseline_absolute\":" << wire.delta_baseline_absolute << ","
        << "\"delta_change_mask_bits\":" << wire.delta_change_mask_bits << ","
        << "\"cue_records\":" << wire.cue_records << ","
        << "\"cue_record_bits\":" << wire.cue_record_bits << ","
        << "\"cue_payload_bits\":" << wire.cue_payload_bits << ","
        << "\"ack_header_bits\":" << wire.ack_header_bits << ","
        << "\"ack_records\":" << wire.ack_records << ","
        << "\"ack_record_bits\":" << wire.ack_record_bits << ","
        << "\"slots\":{";
    for (std::size_t slot = 0; slot < WireFormatStats::slot_count; ++slot) {
        if (slot != 0U) {
            out << ",";
        }
        const WireSlotStats& slot_stats = wire.slots[slot];
        out << "\"" << wire_slot_name(slot) << "\":{"
            << "\"records\":" << slot_stats.records << ","
            << "\"index_bits\":" << slot_stats.index_bits << ","
            << "\"payload_bits\":" << slot_stats.payload_bits << "}";
    }
    out << "}}";
}

inline void write_report_json(std::ostream& out, const StressReport& report) {
    out << std::fixed << std::setprecision(9);
    out << "{";
    out << "\"ticks\":" << report.ticks << ",";
    out << "\"clients\":" << report.config.clients << ",";
    out << "\"client_mode\":\"" << client_mode_name(report.config.client_mode) << "\",";
    out << "\"buffered_frame_lag\":" << report.config.buffered_frame_lag << ",";
    out << "\"ping_interval_seconds\":" << report.config.ping_interval_seconds << ",";
    out << "\"wire_diagnostics\":" << (report.config.wire_diagnostics ? "true" : "false") << ",";
    out << "\"network\":{\"latency_ms\":" << report.config.latency_ms
        << ",\"jitter_ms\":" << report.config.jitter_ms
        << ",\"server_to_client_latency_ms\":" << report.config.server_to_client_latency_ms
        << ",\"server_to_client_jitter_ms\":" << report.config.server_to_client_jitter_ms
        << ",\"client_to_server_latency_ms\":" << report.config.client_to_server_latency_ms
        << ",\"client_to_server_jitter_ms\":" << report.config.client_to_server_jitter_ms << "},";
    out << "\"client_timing\":{\"sample_count\":" << report.client_timing.sample_count
        << ",\"average_latency_frames\":" << report.client_timing.average_latency_frames
        << ",\"average_jitter_frames\":" << report.client_timing.average_jitter_frames
        << ",\"average_measured_buffered_frame_lag\":"
        << report.client_timing.average_measured_buffered_frame_lag
        << ",\"average_buffered_time_dilation\":" << report.client_timing.average_buffered_time_dilation
        << ",\"desired_buffered_frame_lag\":"
        << report.client_timing.max_desired_buffered_frame_lag
        << ",\"target_buffered_frame_lag\":"
        << report.client_timing.max_target_buffered_frame_lag
        << ",\"current_buffered_frame_lag\":"
        << report.client_timing.max_current_buffered_frame_lag
        << ",\"server_update_packets_received\":"
        << report.client_timing.server_update_packets_received
        << ",\"server_update_packets_missing\":"
        << report.client_timing.server_update_packets_missing
        << ",\"server_update_packets_reordered_or_duplicate\":"
        << report.client_timing.server_update_packets_reordered_or_duplicate
        << ",\"server_update_packet_loss\":"
        << report.client_timing.server_update_packet_loss << "},";
    out << "\"live_balls\":" << report.live_balls << ",";
    out << "\"spawned\":" << report.spawned << ",";
    out << "\"despawned\":" << report.despawned << ",";
    out << "\"poison\":{\"added_components\":" << report.poison_components_added
        << ",\"removed_components\":" << report.poison_components_removed
        << ",\"ticks\":" << report.poison_ticks << "},";
    out << "\"tags\":{\"spawn_added\":" << report.spawn_tags_added
        << ",\"bounce_added\":" << report.bounce_tags_added << "},";
    out << "\"cues\":{\"bounce_emitted\":" << report.bounce_cues_emitted << "},";
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
    if (report.config.wire_diagnostics) {
        out << "\"wire_format\":{\"server_to_clients\":";
        write_wire_json(out, report.server_to_clients, report.config.mtu);
        out << ",\"clients_to_server\":";
        write_wire_json(out, report.clients_to_server, report.config.mtu);
        out << "},";
    }
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
        } else if (arg == "--buffered-frame-lag") {
            config.buffered_frame_lag = static_cast<SyncFrame>(parse_u64(arg, require_value()));
        } else if (arg == "--buffered-time-dilation-min") {
            config.buffered_time_dilation_min = parse_double(arg, require_value());
        } else if (arg == "--buffered-time-dilation-max") {
            config.buffered_time_dilation_max = parse_double(arg, require_value());
        } else if (arg == "--buffered-time-dilation-gain") {
            config.buffered_time_dilation_gain = parse_double(arg, require_value());
        } else if (arg == "--ping-interval-seconds") {
            config.ping_interval_seconds = parse_double(arg, require_value());
        } else if (arg == "--wire-diagnostics") {
            config.wire_diagnostics = true;
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
    out << "Usage: ashiato_sync_ball_stress [options]\n"
        << "  --duration-seconds N\n"
        << "  --clients N\n"
        << "  --max-balls N\n"
        << "  --spawn-interval-ms N\n"
        << "  --poison-min N --poison-max N\n"
        << "  --health-min N --health-max N\n"
        << "  --tick-rate N\n"
        << "  --mtu N\n"
        << "  --bandwidth-limit N\n"
        << "  --seed N\n"
        << "  --latency-ms N --jitter-ms N --loss-percent N\n"
        << "  --server-to-client-latency-ms N --client-to-server-latency-ms N\n"
        << "  --server-to-client-jitter-ms N --client-to-server-jitter-ms N\n"
        << "  --server-to-client-loss-percent N --client-to-server-loss-percent N\n"
        << "  --client-mode snap|buffered-interpolation\n"
        << "  --buffered-frame-lag N\n"
        << "  --buffered-frame-lag-capacity N\n"
        << "  --buffered-time-dilation-min N --buffered-time-dilation-max N --buffered-time-dilation-gain N\n"
        << "  --ping-interval-seconds N\n"
        << "  --wire-diagnostics\n"
        << "  --report text|json\n";
}

}  // namespace ashiato::sync::stress
