#pragma once

#include "test_components.hpp"
#include "test_setup.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ashiato_sync_tests {

inline bool start_sync(ashiato::Registry& registry, ashiato::Entity entity, ashiato::sync::SyncArchetypeId archetype) {
    return registry.add<ashiato::sync::Replicated>(entity, ashiato::sync::Replicated{archetype}) != nullptr;
}

inline ashiato::sync::SyncArchetypeId define_predicted_archetype(ashiato::Registry& registry) {
    const ashiato::Entity position =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    return ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position, ashiato::sync::ReplicationAudience::All}});
}

struct AckRecord {
    std::uint32_t packet_id = 0;
};

struct ClientUpdateRecord {
    bool destroy = false;
    bool full = false;
    ashiato::sync::SyncFrame baseline_frame = 0;
    std::uint32_t network_id = 0;
    ashiato::Entity entity;
};

using UpdateRecord = ClientUpdateRecord;

struct ClientUpdatePacket {
    ashiato::sync::SyncFrame frame = 0;
    ashiato::sync::SyncFrame input_ack_frame = 0;
    std::vector<ClientUpdateRecord> records;
};

using UpdatePacket = ClientUpdatePacket;

struct ClientInputPacket {
    std::uint16_t ack_count = 0;
    ashiato::sync::SyncFrame baseline_frame = 0;
    ashiato::sync::SyncFrame first_input_frame = 0;
    std::uint16_t input_count = 0;
    bool first_input_full = false;
};

inline std::vector<AckRecord> read_acks(ashiato::BitBuffer packet) {
    REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_ack_message);
    const auto count = static_cast<std::uint16_t>(packet.read_bits(16U));
    std::vector<AckRecord> records;
    records.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        AckRecord record;
        record.packet_id = static_cast<std::uint32_t>(packet.read_bits(ashiato::sync::protocol::server_packet_id_bits));
        records.push_back(record);
    }
    return records;
}

inline ClientInputPacket read_client_input_header(ashiato::BitBuffer packet) {
    REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message);
    ClientInputPacket input;
    input.ack_count = static_cast<std::uint16_t>(packet.read_bits(16U));
    for (std::uint16_t index = 0; index < input.ack_count; ++index) {
        packet.read_bits(ashiato::sync::protocol::server_packet_id_bits);
    }
    input.baseline_frame = static_cast<ashiato::sync::SyncFrame>(packet.read_bits(32U));
    input.input_count = static_cast<std::uint16_t>(packet.read_bits(16U));
    input.first_input_full = packet.read_bool();
    input.first_input_frame = input.input_count == 0U ? 0U : input.baseline_frame + 1U;
    if (input.first_input_full) {
        const auto explicit_first_input_frame = static_cast<ashiato::sync::SyncFrame>(packet.read_bits(32U));
        if (input.input_count != 0U) {
            input.first_input_frame = explicit_first_input_frame;
        }
    }
    return input;
}

template <typename Client>
bool receive_at_local_frame(
    Client& client,
    ashiato::Registry& registry,
    ashiato::BitBuffer packet,
    ashiato::sync::SyncFrame local_frame) {
    const double target_time = static_cast<double>(local_frame) * client.fixed_dt_seconds();
    while (client.local_time_seconds() < target_time) {
        REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    }
    return client.receive(registry, std::move(packet));
}

template <typename Client>
void tick_client_fixed_frames(
    Client& client,
    ashiato::Registry& registry,
    ashiato::sync::SyncFrame frame_count) {
    for (ashiato::sync::SyncFrame frame = 0; frame < frame_count; ++frame) {
        REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    }
}

template <typename Client>
bool apply_estimated_server_frame(
    Client& client,
    ashiato::Registry& registry,
    ashiato::sync::SyncFrame estimated_server_frame) {
    const ashiato::sync::SyncFrame lag = client.current_buffered_frame_lag();
    return client.apply_frame(registry, estimated_server_frame > lag ? estimated_server_frame - lag : 0U);
}

template <typename Client>
bool sample_estimated_server_frame(
    Client& client,
    const ashiato::Registry& registry,
    double estimated_server_frame,
    ashiato::sync::FractionalTickSampleBuffer& out) {
    return client.sample_fractional_tick_frame(
        registry,
        std::max(0.0, estimated_server_frame - static_cast<double>(client.current_buffered_frame_lag())),
        out);
}

template <typename Client>
bool record_ping_sample(
    Client& client,
    ashiato::Registry& registry,
    ashiato::sync::SyncFrame local_frame) {
    std::vector<ashiato::BitBuffer> packets = client.drain_packets();
    if (packets.empty()) {
        REQUIRE(client.tick(registry, client.options().session.ping_interval_seconds));
        packets = client.drain_packets();
    }
    for (ashiato::BitBuffer packet : packets) {
        if (packet.remaining_bits() < 8U) {
            continue;
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message != ashiato::sync::protocol::client_ping_message) {
            continue;
        }
        const auto sequence = static_cast<std::uint32_t>(packet.read_bits(32U));
        const auto send_frame = static_cast<ashiato::sync::SyncFrame>(
            std::floor(client.local_time_seconds() / client.fixed_dt_seconds()));
        ashiato::BitBuffer pong;
        pong.push_bits(ashiato::sync::protocol::server_pong_message, 8U);
        pong.push_bits(sequence, 32U);
        const auto server_frame = static_cast<ashiato::sync::SyncFrame>((send_frame + local_frame) / 2U);
        pong.push_bits(server_frame, 32U);
        pong.push_bits(0U, ashiato::sync::protocol::frame_subframe_bits);
        pong.push_bits(server_frame, 32U);
        pong.push_bits(0U, ashiato::sync::protocol::frame_subframe_bits);
        return receive_at_local_frame(client, registry, std::move(pong), local_frame);
    }
    return false;
}

struct PingPacket {
    std::uint32_t sequence = 0;
    ashiato::sync::SyncFrame send_frame = 0;
};

inline bool read_ping_packet(ashiato::BitBuffer packet, PingPacket& out) {
    if (packet.remaining_bits() < 8U) {
        return false;
    }
    const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
    if (message != ashiato::sync::protocol::client_ping_message || packet.remaining_bits() < 32U) {
        return false;
    }
    out.sequence = static_cast<std::uint32_t>(packet.read_bits(32U));
    return true;
}

template <typename Client>
bool drain_ping(
    Client& client,
    ashiato::Registry& registry,
    double dt_seconds,
    PingPacket& out) {
    if (dt_seconds > 0.0) {
        REQUIRE(client.tick(registry, dt_seconds));
    }
    for (ashiato::BitBuffer packet : client.drain_packets()) {
        if (read_ping_packet(packet, out)) {
            out.send_frame = static_cast<ashiato::sync::SyncFrame>(
                std::floor(client.local_time_seconds() / client.fixed_dt_seconds()));
            return true;
        }
    }
    return false;
}

template <typename Client>
bool receive_pong(
    Client& client,
    ashiato::Registry& registry,
    const PingPacket& ping,
    ashiato::sync::SyncFrame local_frame) {
    ashiato::BitBuffer pong;
    pong.push_bits(ashiato::sync::protocol::server_pong_message, 8U);
    pong.push_bits(ping.sequence, 32U);
    const auto server_frame = static_cast<ashiato::sync::SyncFrame>((ping.send_frame + local_frame) / 2U);
    pong.push_bits(server_frame, 32U);
    pong.push_bits(0U, ashiato::sync::protocol::frame_subframe_bits);
    pong.push_bits(server_frame, 32U);
    pong.push_bits(0U, ashiato::sync::protocol::frame_subframe_bits);
    return receive_at_local_frame(client, registry, std::move(pong), local_frame);
}

inline ClientUpdatePacket read_update(ashiato::BitBuffer packet, std::size_t sync_slot_count = 2U) {
    REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::server_update_message);
    ClientUpdatePacket update;
    update.frame = static_cast<ashiato::sync::SyncFrame>(packet.read_bits(32U));
    packet.read_bits(ashiato::sync::protocol::server_packet_id_bits);
    update.input_ack_frame = static_cast<ashiato::sync::SyncFrame>(packet.read_bits(32U));
    const auto count = static_cast<std::uint16_t>(packet.read_bits(16U));
    update.records.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        ClientUpdateRecord record;
        record.destroy = packet.read_bool();
        REQUIRE(ashiato::sync::protocol::read_network_entity_id(packet, record.network_id));
        if (!record.destroy) {
            record.full = packet.read_bool();
            if (record.full) {
                packet.read_bits(32U);
                const bool uses_presence_mask = packet.read_bool();
                const std::size_t sync_slot_bits = ashiato::sync::protocol::bits_for_range(sync_slot_count);
                std::uint16_t component_count = 0;
                std::uint64_t presence_mask = 0;
                if (uses_presence_mask) {
                    presence_mask = packet.read_unsigned_bits(sync_slot_count);
                    for (std::size_t slot = 0; slot < sync_slot_count; ++slot) {
                        if ((presence_mask & (std::uint64_t{1} << slot)) != 0U) {
                            ++component_count;
                        }
                    }
                } else {
                    component_count = static_cast<std::uint16_t>(packet.read_bits(16U));
                }
                for (std::uint16_t component = 0; component < component_count; ++component) {
                    std::uint16_t component_index = 0;
                    if (uses_presence_mask) {
                        while ((presence_mask & (std::uint64_t{1} << component_index)) == 0U) {
                            ++component_index;
                        }
                        presence_mask &= ~(std::uint64_t{1} << component_index);
                    } else {
                        component_index = static_cast<std::uint16_t>(packet.read_bits(sync_slot_bits));
                    }
                    const std::size_t payload_bits = component_index == 1 ? 17U : sizeof(Health) * 8U;
                    for (std::size_t bit = 0; bit < payload_bits; ++bit) {
                        packet.read_bool();
                    }
                }
                const bool has_cues = packet.read_bool();
                REQUIRE_FALSE(has_cues);
            } else {
                REQUIRE(ashiato::sync::protocol::read_baseline_frame(packet, update.frame, record.baseline_frame));
                const bool tag_changed = packet.read_bool();
                REQUIRE_FALSE(tag_changed);
                const bool position_changed = packet.read_bool();
                if (position_changed) {
                    for (std::size_t bit = 0; bit < 17U; ++bit) {
                        packet.read_bool();
                    }
                }
                const bool has_cues = packet.read_bool();
                REQUIRE_FALSE(has_cues);
            }
        }
        update.records.push_back(record);
    }
    return update;
}

inline const ashiato::BitBuffer& packet_for(
    const std::vector<std::pair<ashiato::sync::ClientId, ashiato::BitBuffer>>& packets,
    ashiato::sync::ClientId client) {
    const auto found = std::find_if(packets.begin(), packets.end(), [&](const auto& sent) {
        return sent.first == client;
    });
    REQUIRE(found != packets.end());
    return found->second;
}

inline std::uint32_t test_network_id(ashiato::Entity entity) {
    return ashiato::Registry::entity_index(entity) + 1U;
}

inline ashiato::sync::ClientEntityNetworkId test_client_entity_network_id(
    ashiato::sync::ClientId client,
    std::uint32_t wire_network_id,
    std::uint32_t version = 1U) {
    return ashiato::sync::make_client_entity_network_id(client, wire_network_id, version);
}

inline ashiato::sync::ClientEntityNetworkId test_client_entity_network_id(
    ashiato::sync::ClientId client,
    ashiato::Entity entity,
    std::uint32_t version = 1U) {
    return test_client_entity_network_id(client, test_network_id(entity), version);
}

inline ashiato::sync::ClientEntityNetworkId first_allocated_client_entity_network_id(
    ashiato::sync::ClientId client,
    std::uint32_t version = 1U) {
    return test_client_entity_network_id(client, 1U, version);
}

inline ashiato::BitBuffer make_position_packet(
    ashiato::sync::SyncFrame frame,
    const std::vector<std::pair<ashiato::Entity, Position>>& records,
    std::size_t sync_slot_count = 2U,
    std::uint32_t packet_id = 0U,
    ashiato::sync::SyncFrame input_ack_frame = 0U) {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(packet_id == 0U ? frame : packet_id, ashiato::sync::protocol::server_packet_id_bits);
    packet.push_bits(input_ack_frame, 32U);
    packet.push_bits(static_cast<std::uint16_t>(records.size()), 16U);
    for (const auto& record : records) {
        packet.push_bool(false);
        ashiato::sync::protocol::write_network_entity_id(packet, test_network_id(record.first));
        packet.push_bool(true);
        packet.push_bits(0, 32U);
        packet.push_bool(false);
        packet.push_bits(1, 16U);
        packet.push_bits(1, ashiato::sync::protocol::bits_for_range(sync_slot_count));

        packet.push_bytes(reinterpret_cast<const char*>(&record.second), sizeof(Position));
        packet.push_bool(false);
    }
    return packet;
}

inline ashiato::BitBuffer make_predicted_position_packet(
    ashiato::sync::SyncFrame frame,
    ashiato::Entity server_entity,
    PredictedPosition position,
    std::uint32_t packet_id = 0U) {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(packet_id == 0U ? frame : packet_id, ashiato::sync::protocol::server_packet_id_bits);
    packet.push_bits(0, 32U);
    packet.push_bits(1, 16U);
    packet.push_bool(false);
    ashiato::sync::protocol::write_network_entity_id(packet, test_network_id(server_entity));
    packet.push_bool(true);
    packet.push_bits(0, 32U);
    packet.push_bool(false);
    packet.push_bits(1, 16U);
    packet.push_bits(1, ashiato::sync::protocol::bits_for_range(2U));
    packet.push_bits(static_cast<std::int32_t>(position.x * 10.0f), 16U);
    packet.push_bits(static_cast<std::int32_t>(position.y * 10.0f), 16U);
    packet.push_bool(false);
    return packet;
}

inline ashiato::BitBuffer make_destroy_packet(ashiato::sync::SyncFrame frame, ashiato::Entity server_entity) {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(frame, ashiato::sync::protocol::server_packet_id_bits);
    packet.push_bits(0, 32U);
    packet.push_bits(1, 16U);
    packet.push_bool(true);
    ashiato::sync::protocol::write_network_entity_id(packet, test_network_id(server_entity));
    return packet;
}

struct ComponentRecord {
    std::uint16_t component_index = 0;
    ashiato::BitBuffer payload;
};

struct EntityRecord {
    ashiato::Entity entity;
    std::uint32_t network_id = 0;
    bool destroy = false;
    bool full = false;
    ashiato::sync::SyncFrame baseline_frame = 0;
    ashiato::sync::SyncArchetypeId archetype;
    std::vector<ComponentRecord> components;
};

struct ServerUpdatePacket {
    std::uint8_t message = 0;
    ashiato::sync::SyncFrame frame = 0;
    std::uint32_t packet_id = 0;
    ashiato::sync::SyncFrame input_ack_frame = 0;
    std::vector<EntityRecord> entities;
};

inline ashiato::BitBuffer make_connect_request(const std::string& token) {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::client_connect_request_message, 8U);
    ashiato::sync::protocol::write_string(packet, token);
    return packet;
}

inline ServerUpdatePacket read_server_update(
    ashiato::BitBuffer packet,
    std::size_t sync_slot_count = 2U,
    std::size_t component_one_payload_bits = 17U,
    std::size_t network_entity_id_tier0_bits =
        ashiato::sync::protocol::default_network_entity_id_tier0_bits) {
    ServerUpdatePacket update;
    update.message = static_cast<std::uint8_t>(packet.read_bits(8U));
    update.frame = static_cast<ashiato::sync::SyncFrame>(packet.read_bits(32U));
    update.packet_id = static_cast<std::uint32_t>(packet.read_bits(ashiato::sync::protocol::server_packet_id_bits));
    update.input_ack_frame = static_cast<ashiato::sync::SyncFrame>(packet.read_bits(32U));
    const auto entity_count = static_cast<std::uint16_t>(packet.read_bits(16U));
    update.entities.reserve(entity_count);
    for (std::uint16_t entity_index = 0; entity_index < entity_count; ++entity_index) {
        EntityRecord entity;
        entity.destroy = packet.read_bool();
        REQUIRE(ashiato::sync::protocol::read_network_entity_id(
            packet,
            entity.network_id,
            network_entity_id_tier0_bits));
        if (entity.destroy) {
            update.entities.push_back(std::move(entity));
            continue;
        }
        entity.full = packet.read_bool();
        if (entity.full) {
            entity.archetype =
                ashiato::sync::SyncArchetypeId{static_cast<std::uint32_t>(packet.read_bits(32U))};
            const bool uses_presence_mask = packet.read_bool();
            const std::size_t sync_slot_bits = ashiato::sync::protocol::bits_for_range(sync_slot_count);
            std::uint16_t component_count = 0;
            std::uint64_t presence_mask = 0;
            if (uses_presence_mask) {
                presence_mask = packet.read_unsigned_bits(sync_slot_count);
                for (std::size_t slot = 0; slot < sync_slot_count; ++slot) {
                    if ((presence_mask & (std::uint64_t{1} << slot)) != 0U) {
                        ++component_count;
                    }
                }
            } else {
                component_count = static_cast<std::uint16_t>(packet.read_bits(16U));
            }
            entity.components.reserve(component_count);
            for (std::uint16_t component = 0; component < component_count; ++component) {
                ComponentRecord record;
                if (uses_presence_mask) {
                    while ((presence_mask & (std::uint64_t{1} << record.component_index)) == 0U) {
                        ++record.component_index;
                    }
                    presence_mask &= ~(std::uint64_t{1} << record.component_index);
                } else {
                    record.component_index = static_cast<std::uint16_t>(packet.read_bits(sync_slot_bits));
                }
                const std::size_t payload_bits =
                    record.component_index == 1 ? component_one_payload_bits : sizeof(Health) * 8U;
                for (std::size_t bit = 0; bit < payload_bits; ++bit) {
                    record.payload.push_bool(packet.read_bool());
                }
                entity.components.push_back(std::move(record));
            }
            const bool has_cues = packet.read_bool();
            REQUIRE_FALSE(has_cues);
        } else {
            REQUIRE(ashiato::sync::protocol::read_baseline_frame(packet, update.frame, entity.baseline_frame));
            const bool tag_changed = packet.read_bool();
            REQUIRE_FALSE(tag_changed);
            std::vector<std::uint16_t> changed_components;
            for (std::uint16_t component_index = 1; component_index < sync_slot_count; ++component_index) {
                if (packet.read_bool()) {
                    changed_components.push_back(component_index);
                }
            }
            for (std::size_t changed_index = 0; changed_index < changed_components.size(); ++changed_index) {
                ComponentRecord record;
                record.component_index = changed_components[changed_index];
                const std::size_t payload_bits =
                    record.component_index == 1 ? component_one_payload_bits : sizeof(Health) * 8U;
                for (std::size_t bit = 0; bit < payload_bits; ++bit) {
                    record.payload.push_bool(packet.read_bool());
                }
                entity.components.push_back(std::move(record));
            }
            const bool has_cues = packet.read_bool();
            REQUIRE_FALSE(has_cues);
        }
        update.entities.push_back(std::move(entity));
    }
    return update;
}

inline NetworkedPayload read_first_networked_payload(const ashiato::BitBuffer& packet) {
    const ServerUpdatePacket update = read_server_update(packet);
    REQUIRE(update.message == 1);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].components.size() == 1);
    return read_networked_payload(update.entities[0].components[0].payload);
}

inline ashiato::BitBuffer write_ack_packet(std::uint32_t packet_id) {
    ashiato::BitBuffer packet;
    packet.push_bits(ashiato::sync::protocol::client_ack_message, 8U);
    packet.push_bits(1, 16U);
    packet.push_bits(packet_id, ashiato::sync::protocol::server_packet_id_bits);
    return packet;
}

}  // namespace ashiato_sync_tests
