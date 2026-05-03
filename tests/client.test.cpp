#include "test_components.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using kage_sync_tests::NetworkedPosition;
using kage_sync_tests::Health;
using kage_sync_tests::Position;
using kage_sync_tests::PredictedPosition;
using kage_sync_tests::Secret;
using kage_sync_tests::SmoothPosition;
using kage_sync_tests::CuePlayback;
using kage_sync_tests::ReferenceCue;
using kage_sync_tests::TestCue;
using kage_sync_tests::TargetReference;
using kage_sync_tests::Visible;

namespace {

bool start_sync(ecs::Registry& registry, ecs::Entity entity, kage::sync::SyncArchetypeId archetype) {
    return registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{archetype}) != nullptr;
}

struct AckRecord {
    std::uint32_t packet_id = 0;
};

struct UpdateRecord {
    bool destroy = false;
    bool full = false;
    kage::sync::SyncFrame baseline_frame = 0;
    std::uint32_t network_id = 0;
    ecs::Entity entity;
};

struct UpdatePacket {
    kage::sync::SyncFrame frame = 0;
    kage::sync::SyncFrame input_ack_frame = 0;
    std::vector<UpdateRecord> records;
};

struct ClientInputPacket {
    std::uint16_t ack_count = 0;
    kage::sync::SyncFrame baseline_frame = 0;
    kage::sync::SyncFrame first_input_frame = 0;
    std::uint16_t input_count = 0;
    bool first_input_full = false;
};

std::vector<AckRecord> read_acks(kage::sync::BitBuffer packet) {
    REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_ack_message);
    const auto count = static_cast<std::uint16_t>(packet.read_bits(16U));
    std::vector<AckRecord> records;
    records.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        AckRecord record;
        record.packet_id = static_cast<std::uint32_t>(packet.read_bits(kage::sync::protocol::server_packet_id_bits));
        records.push_back(record);
    }
    return records;
}

ClientInputPacket read_client_input_header(kage::sync::BitBuffer packet) {
    REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message);
    ClientInputPacket input;
    input.ack_count = static_cast<std::uint16_t>(packet.read_bits(16U));
    for (std::uint16_t index = 0; index < input.ack_count; ++index) {
        packet.read_bits(kage::sync::protocol::server_packet_id_bits);
    }
    input.baseline_frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
    input.input_count = static_cast<std::uint16_t>(packet.read_bits(16U));
    input.first_input_full = packet.read_bool();
    input.first_input_frame = input.input_count == 0U ? 0U : input.baseline_frame + 1U;
    if (input.first_input_full) {
        const auto explicit_first_input_frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
        if (input.input_count != 0U) {
            input.first_input_frame = explicit_first_input_frame;
        }
    }
    return input;
}

bool record_ping_sample(
    kage::sync::ReplicationClient& client,
    ecs::Registry& registry,
    kage::sync::SyncFrame receive_frame) {
    std::vector<kage::sync::BitBuffer> packets = client.drain_packets();
    if (packets.empty()) {
        REQUIRE(client.tick(registry, client.options().ping_interval_seconds));
        packets = client.drain_packets();
    }
    for (kage::sync::BitBuffer packet : packets) {
        if (packet.remaining_bits() < 8U) {
            continue;
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message != kage::sync::protocol::client_ping_message) {
            continue;
        }
        const auto sequence = static_cast<std::uint32_t>(packet.read_bits(32U));
        const auto send_frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
        kage::sync::BitBuffer pong;
        pong.push_bits(kage::sync::protocol::server_pong_message, 8U);
        pong.push_bits(sequence, 32U);
        pong.push_bits(send_frame, 32U);
        return client.receive(registry, pong, receive_frame);
    }
    return false;
}

struct PingPacket {
    std::uint32_t sequence = 0;
    kage::sync::SyncFrame send_frame = 0;
};

bool read_ping_packet(kage::sync::BitBuffer packet, PingPacket& out) {
    if (packet.remaining_bits() < 8U) {
        return false;
    }
    const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
    if (message != kage::sync::protocol::client_ping_message || packet.remaining_bits() < 64U) {
        return false;
    }
    out.sequence = static_cast<std::uint32_t>(packet.read_bits(32U));
    out.send_frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
    return true;
}

bool drain_ping(
    kage::sync::ReplicationClient& client,
    ecs::Registry& registry,
    double dt_seconds,
    PingPacket& out) {
    if (dt_seconds > 0.0) {
        REQUIRE(client.tick(registry, dt_seconds));
    }
    for (kage::sync::BitBuffer packet : client.drain_packets()) {
        if (read_ping_packet(packet, out)) {
            return true;
        }
    }
    return false;
}

bool receive_pong(
    kage::sync::ReplicationClient& client,
    ecs::Registry& registry,
    const PingPacket& ping,
    kage::sync::SyncFrame receive_frame) {
    kage::sync::BitBuffer pong;
    pong.push_bits(kage::sync::protocol::server_pong_message, 8U);
    pong.push_bits(ping.sequence, 32U);
    pong.push_bits(ping.send_frame, 32U);
    return client.receive(registry, pong, receive_frame);
}

UpdatePacket read_update(kage::sync::BitBuffer packet, std::size_t sync_slot_count = 2U) {
    REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::server_update_message);
    UpdatePacket update;
    update.frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
    packet.read_bits(kage::sync::protocol::server_packet_id_bits);
    update.input_ack_frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
    const auto count = static_cast<std::uint16_t>(packet.read_bits(16U));
    update.records.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        UpdateRecord record;
        record.destroy = packet.read_bool();
        REQUIRE(kage::sync::protocol::read_network_entity_id(packet, record.network_id));
        if (!record.destroy) {
            record.full = packet.read_bool();
            if (record.full) {
                packet.read_bits(32U);
                const bool uses_presence_mask = packet.read_bool();
                const std::size_t sync_slot_bits = kage::sync::protocol::bits_for_range(sync_slot_count);
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
                REQUIRE(kage::sync::protocol::read_baseline_frame(packet, update.frame, record.baseline_frame));
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

const kage::sync::BitBuffer& packet_for(
    const std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>>& packets,
    kage::sync::ClientId client) {
    const auto found = std::find_if(packets.begin(), packets.end(), [&](const auto& sent) {
        return sent.first == client;
    });
    REQUIRE(found != packets.end());
    return found->second;
}

std::uint32_t test_network_id(ecs::Entity entity) {
    return ecs::Registry::entity_index(entity) + 1U;
}

kage::sync::ClientEntityNetworkId test_client_entity_network_id(
    kage::sync::ClientId client,
    std::uint32_t wire_network_id,
    std::uint32_t version = 1U) {
    return kage::sync::make_client_entity_network_id(client, wire_network_id, version);
}

kage::sync::BitBuffer make_position_packet(
    kage::sync::SyncFrame frame,
    const std::vector<std::pair<ecs::Entity, Position>>& records,
    std::size_t sync_slot_count = 2U,
    std::uint32_t packet_id = 0U) {
    kage::sync::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(packet_id == 0U ? frame : packet_id, kage::sync::protocol::server_packet_id_bits);
    packet.push_bits(0, 32U);
    packet.push_bits(static_cast<std::uint16_t>(records.size()), 16U);
    for (const auto& record : records) {
        packet.push_bool(false);
        kage::sync::protocol::write_network_entity_id(packet, test_network_id(record.first));
        packet.push_bool(true);
        packet.push_bits(0, 32U);
        packet.push_bool(false);
        packet.push_bits(1, 16U);
        packet.push_bits(1, kage::sync::protocol::bits_for_range(sync_slot_count));

        packet.push_bytes(reinterpret_cast<const char*>(&record.second), sizeof(Position));
        packet.push_bool(false);
    }
    return packet;
}

kage::sync::BitBuffer make_predicted_position_packet(
    kage::sync::SyncFrame frame,
    ecs::Entity server_entity,
    PredictedPosition position,
    std::uint32_t packet_id = 0U) {
    kage::sync::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(packet_id == 0U ? frame : packet_id, kage::sync::protocol::server_packet_id_bits);
    packet.push_bits(0, 32U);
    packet.push_bits(1, 16U);
    packet.push_bool(false);
    kage::sync::protocol::write_network_entity_id(packet, test_network_id(server_entity));
    packet.push_bool(true);
    packet.push_bits(0, 32U);
    packet.push_bool(false);
    packet.push_bits(1, 16U);
    packet.push_bits(1, kage::sync::protocol::bits_for_range(2U));
    packet.push_bits(static_cast<std::int32_t>(position.x * 10.0f), 16U);
    packet.push_bits(static_cast<std::int32_t>(position.y * 10.0f), 16U);
    packet.push_bool(false);
    return packet;
}

kage::sync::BitBuffer make_destroy_packet(kage::sync::SyncFrame frame, ecs::Entity server_entity) {
    kage::sync::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(frame, kage::sync::protocol::server_packet_id_bits);
    packet.push_bits(0, 32U);
    packet.push_bits(1, 16U);
    packet.push_bool(true);
    kage::sync::protocol::write_network_entity_id(packet, test_network_id(server_entity));
    return packet;
}

}  // namespace

TEST_CASE("replication client applies full updates and queues ACKs") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    REQUIRE(client.receive(client_registry, packets[0]));

    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client_registry.contains<Position>(local));
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(client_registry.get<Position>(local).y == 2.0f);

    std::vector<kage::sync::BitBuffer> ack_packets = client.drain_ack_packets();
    REQUIRE(ack_packets.size() == 1);
    std::vector<AckRecord> acks = read_acks(ack_packets[0]);
    REQUIRE(acks.size() == 1);
    REQUIRE(acks[0].packet_id != 0);
    REQUIRE(server.process_packet(1, ack_packets[0]));
}

TEST_CASE("replication client input applies to locally owned entities and drains with ACKs") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    kage::sync::configure_client(registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(registry));

    const ecs::Entity owned = registry.create();
    const ecs::Entity other = registry.create();
    REQUIRE(kage::sync::set_owner(registry, owned, 1));
    REQUIRE(kage::sync::set_owner(registry, other, 2));

    kage::sync::ReplicationClient client;
    REQUIRE(client.set_input(registry, NetworkedPosition{3.0f, 4.0f}));
    REQUIRE(registry.contains<NetworkedPosition>(owned));
    REQUIRE(registry.get<NetworkedPosition>(owned).x == 3.0f);
    REQUIRE(registry.get<NetworkedPosition>(owned).y == 4.0f);
    REQUIRE_FALSE(registry.contains<NetworkedPosition>(other));

    const ecs::Entity server_entity = ecs::Entity{1};
    REQUIRE(client.receive(registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));

    const std::vector<kage::sync::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());
    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.ack_count == 1);
    REQUIRE(input.baseline_frame == 0);
    REQUIRE(input.input_count == 1);
}

TEST_CASE("replication client sends full first input frame after unacked input buffer overwrite") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    kage::sync::configure_client(registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(registry));

    kage::sync::ReplicationClientOptions options;
    options.input_buffer_capacity_frames = 4;
    kage::sync::ReplicationClient client(options);
    REQUIRE(client.set_input(registry, NetworkedPosition{1.0f, 2.0f}));
    for (int tick = 0; tick < 6; ++tick) {
        REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    }

    const std::vector<kage::sync::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());
    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.baseline_frame == 0);
    REQUIRE(input.first_input_frame == 3);
    REQUIRE(input.input_count == 4);
    REQUIRE(input.first_input_full);
}

TEST_CASE("replicated snap cues play once with late time and ACK-driven resend") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    kage::sync::register_sync_cue<TestCue>(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    REQUIRE(kage::sync::emit_cue(server_registry, server_entity, 1, TestCue{7}, 1.0f));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    client_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(client_registry);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClient client;

    REQUIRE(client.receive(client_registry, packets[0], 10));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(client_registry.contains<CuePlayback>(local));
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);
    REQUIRE(client_registry.get<CuePlayback>(local).last_id == 7);
    REQUIRE(client_registry.get<CuePlayback>(local).last_late_seconds ==
            Catch::Approx(9.0 / 60.0f));

    packets.clear();
    server.tick(server_registry);
    REQUIRE_FALSE(packets.empty());
    REQUIRE(client.receive(client_registry, packets.back(), 11));
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);

    std::vector<kage::sync::BitBuffer> acks = client.drain_ack_packets();
    REQUIRE_FALSE(acks.empty());
    for (const kage::sync::BitBuffer& ack : acks) {
        REQUIRE(server.process_packet(1, ack));
    }
    packets.clear();
    server.tick(server_registry);
    REQUIRE_FALSE(packets.empty());
    REQUIRE(client.receive(client_registry, packets.back(), 12));
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);
}

TEST_CASE("owner-only cues replicate only to the cue owner") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    kage::sync::register_sync_cue<TestCue>(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 1));

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& packet) {
        packets.push_back({client, packet});
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    REQUIRE(kage::sync::emit_cue(server_registry, server_entity, 1, TestCue{11}, 1.0f, true));
    server.tick(server_registry);

    ecs::Registry owner_registry;
    REQUIRE(kage_sync_tests::define_position_archetype(owner_registry) == server_archetype);
    owner_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(owner_registry);
    kage::sync::configure_client(owner_registry, 1);

    ecs::Registry other_registry;
    REQUIRE(kage_sync_tests::define_position_archetype(other_registry) == server_archetype);
    other_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(other_registry);
    kage::sync::configure_client(other_registry, 2);

    kage::sync::ReplicationClient owner_client;
    kage::sync::ReplicationClient other_client;
    REQUIRE(owner_client.receive(owner_registry, packet_for(packets, 1)));
    REQUIRE(other_client.receive(other_registry, packet_for(packets, 2)));

    const ecs::Entity owner_local = owner_client.local_entity(server_entity);
    const ecs::Entity other_local = other_client.local_entity(server_entity);
    REQUIRE(owner_registry.contains<CuePlayback>(owner_local));
    REQUIRE(owner_registry.get<CuePlayback>(owner_local).last_id == 11);
    REQUIRE_FALSE(other_registry.contains<CuePlayback>(other_local));
}

TEST_CASE("default cues still replicate to all clients") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    kage::sync::register_sync_cue<TestCue>(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& packet) {
        packets.push_back({client, packet});
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    REQUIRE(kage::sync::emit_cue(server_registry, server_entity, 1, TestCue{12}, 1.0f));
    server.tick(server_registry);

    ecs::Registry first_registry;
    REQUIRE(kage_sync_tests::define_position_archetype(first_registry) == server_archetype);
    first_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(first_registry);
    kage::sync::configure_client(first_registry, 1);

    ecs::Registry second_registry;
    REQUIRE(kage_sync_tests::define_position_archetype(second_registry) == server_archetype);
    second_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(second_registry);
    kage::sync::configure_client(second_registry, 2);

    kage::sync::ReplicationClient first_client;
    kage::sync::ReplicationClient second_client;
    REQUIRE(first_client.receive(first_registry, packet_for(packets, 1)));
    REQUIRE(second_client.receive(second_registry, packet_for(packets, 2)));

    REQUIRE(first_registry.get<CuePlayback>(first_client.local_entity(server_entity)).last_id == 12);
    REQUIRE(second_registry.get<CuePlayback>(second_client.local_entity(server_entity)).last_id == 12);
}

TEST_CASE("cue entity references resolve to client-local entities") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    kage::sync::register_sync_cue<ReferenceCue>(server_registry);
    const ecs::Entity target = server_registry.create();
    const ecs::Entity source = server_registry.create();
    REQUIRE(server_registry.add<Position>(target, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add<Position>(source, Position{3.0f, 4.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, source, 1));

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, target, server_archetype));
    REQUIRE(start_sync(server_registry, source, server_archetype));
    REQUIRE(kage::sync::emit_cue(
        server_registry,
        source,
        1,
        ReferenceCue{kage::sync::EntityReference{target}},
        1.0f,
        true));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    REQUIRE(kage_sync_tests::define_position_archetype(client_registry) == server_archetype);
    client_registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<ReferenceCue>(client_registry);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ClientEntityNetworkId target_network_id = kage::sync::invalid_client_entity_network_id;
    kage::sync::ClientEntityNetworkId source_network_id = kage::sync::invalid_client_entity_network_id;
    kage::sync::ReplicationClientOptions client_options;
    client_options.entity_mode_selector = [&](const kage::sync::ReplicatedEntityUpdateView& update) {
        Position position{};
        if (update.try_get(client_registry, position) && position.x == 1.0f) {
            target_network_id = update.client_entity_network_id;
        } else {
            source_network_id = update.client_entity_network_id;
        }
        return kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(client_options);
    REQUIRE(client.receive(client_registry, packets[0]));

    const ecs::Entity local_source = client.local_entity(source_network_id);
    const ecs::Entity local_target = client.local_entity(target_network_id);
    REQUIRE(local_source);
    REQUIRE(local_target);
    const CuePlayback& playback = client_registry.get<CuePlayback>(local_source);
    REQUIRE(playback.plays == 1);
    REQUIRE(playback.last_target == local_target);
    REQUIRE(playback.last_target_network_id != kage::sync::invalid_client_entity_network_id);
}

TEST_CASE("entity references serialize as client-local network ids") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<Position>(server_registry, "Position");
    const ecs::Entity server_reference =
        kage::sync::register_sync_component<TargetReference>(server_registry, "TargetReference");
    const kage::sync::SyncArchetypeId position_archetype = kage::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const kage::sync::SyncArchetypeId reference_archetype = kage::sync::define_archetype(
        server_registry,
        "ReferenceActor",
        {{server_reference, kage::sync::ReplicationAudience::All}});

    const ecs::Entity target = server_registry.create();
    const ecs::Entity source = server_registry.create();
    REQUIRE(server_registry.add<Position>(target, Position{4.0f, 5.0f}) != nullptr);
    REQUIRE(server_registry.add<TargetReference>(
                source,
                TargetReference{kage::sync::EntityReference{target}}) != nullptr);
    REQUIRE(start_sync(server_registry, target, position_archetype));
    REQUIRE(start_sync(server_registry, source, reference_archetype));

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<Position>(client_registry, "Position");
    const ecs::Entity client_reference =
        kage::sync::register_sync_component<TargetReference>(client_registry, "TargetReference");
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "PositionActor",
                {{client_position, kage::sync::ReplicationAudience::All}}) == position_archetype);
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "ReferenceActor",
                {{client_reference, kage::sync::ReplicationAudience::All}}) == reference_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ClientEntityNetworkId target_network_id =
        kage::sync::invalid_client_entity_network_id;
    kage::sync::ClientEntityNetworkId source_network_id =
        kage::sync::invalid_client_entity_network_id;
    kage::sync::ReplicationClientOptions options;
    options.entity_mode_selector = [&](const kage::sync::ReplicatedEntityUpdateView& update) {
        if (update.archetype == position_archetype) {
            target_network_id = update.client_entity_network_id;
        } else if (update.archetype == reference_archetype) {
            source_network_id = update.client_entity_network_id;
        }
        return kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(options);
    REQUIRE(client.receive(client_registry, packets[0]));

    const ecs::Entity local_target = client.local_entity(target_network_id);
    const ecs::Entity local_source = client.local_entity(source_network_id);
    REQUIRE(local_target);
    REQUIRE(local_source);
    REQUIRE(client_registry.alive(local_target));
    REQUIRE(client_registry.alive(local_source));

    const TargetReference& reference = client_registry.get<TargetReference>(local_source);
    REQUIRE(reference.target.client_entity_network_id != kage::sync::invalid_client_entity_network_id);
    REQUIRE(reference.target.client_entity_network_id == target_network_id);
    REQUIRE(reference.target.entity == local_target);
    REQUIRE(client.local_entity(reference.target.client_entity_network_id) == local_target);
}

TEST_CASE("entity references remain resolvable when the referenced entity arrives later in the packet") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<Position>(server_registry, "Position");
    const ecs::Entity server_reference =
        kage::sync::register_sync_component<TargetReference>(server_registry, "TargetReference");
    const kage::sync::SyncArchetypeId position_archetype = kage::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const kage::sync::SyncArchetypeId reference_archetype = kage::sync::define_archetype(
        server_registry,
        "ReferenceActor",
        {{server_reference, kage::sync::ReplicationAudience::All}});

    const ecs::Entity source = server_registry.create();
    const ecs::Entity target = server_registry.create();
    REQUIRE(server_registry.add<TargetReference>(
                source,
                TargetReference{kage::sync::EntityReference{target}}) != nullptr);
    REQUIRE(server_registry.add<Position>(target, Position{4.0f, 5.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, source, reference_archetype));
    REQUIRE(start_sync(server_registry, target, position_archetype));

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<Position>(client_registry, "Position");
    const ecs::Entity client_reference =
        kage::sync::register_sync_component<TargetReference>(client_registry, "TargetReference");
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "PositionActor",
                {{client_position, kage::sync::ReplicationAudience::All}}) == position_archetype);
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "ReferenceActor",
                {{client_reference, kage::sync::ReplicationAudience::All}}) == reference_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ClientEntityNetworkId target_network_id =
        kage::sync::invalid_client_entity_network_id;
    kage::sync::ClientEntityNetworkId source_network_id =
        kage::sync::invalid_client_entity_network_id;
    kage::sync::ReplicationClientOptions options;
    options.entity_mode_selector = [&](const kage::sync::ReplicatedEntityUpdateView& update) {
        if (update.archetype == position_archetype) {
            target_network_id = update.client_entity_network_id;
        } else if (update.archetype == reference_archetype) {
            source_network_id = update.client_entity_network_id;
        }
        return kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(options);
    REQUIRE(client.receive(client_registry, packets[0]));

    const ecs::Entity local_target = client.local_entity(target_network_id);
    const ecs::Entity local_source = client.local_entity(source_network_id);
    REQUIRE(local_target);
    REQUIRE(local_source);

    const TargetReference& reference = client_registry.get<TargetReference>(local_source);
    REQUIRE(reference.target.client_entity_network_id != kage::sync::invalid_client_entity_network_id);
    REQUIRE(reference.target.client_entity_network_id == target_network_id);
    REQUIRE(reference.target.entity == ecs::Entity{});
    REQUIRE(client.local_entity(reference.target.client_entity_network_id) == local_target);

    kage::sync::EntityReference resolved = reference.target;
    REQUIRE(client.resolve_entity_reference(resolved) == kage::sync::EntityReferenceStatus::Alive);
    REQUIRE(resolved.client_entity_network_id == target_network_id);
    REQUIRE(resolved.entity == local_target);
}

TEST_CASE("resolve entity reference preserves pending references") {
    kage::sync::ReplicationClient client;

    kage::sync::EntityReference invalid;
    invalid.entity = ecs::Entity{77};
    REQUIRE(client.resolve_entity_reference(invalid) == kage::sync::EntityReferenceStatus::Invalid);
    REQUIRE_FALSE(invalid.entity);
    REQUIRE(invalid.client_entity_network_id == kage::sync::invalid_client_entity_network_id);

    const kage::sync::ClientEntityNetworkId pending_id = test_client_entity_network_id(1, 42U);
    kage::sync::EntityReference pending;
    pending.entity = ecs::Entity{77};
    pending.client_entity_network_id = pending_id;
    REQUIRE(client.resolve_entity_reference(pending) == kage::sync::EntityReferenceStatus::Pending);
    REQUIRE_FALSE(pending.entity);
    REQUIRE(pending.client_entity_network_id == pending_id);
}

TEST_CASE("resolve entity reference clears destroyed and reused network ids") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    const ecs::Entity server_entity{42};
    const std::uint32_t wire_id = test_network_id(server_entity);
    const kage::sync::ClientEntityNetworkId first_id = test_client_entity_network_id(1, wire_id, 1U);
    const kage::sync::ClientEntityNetworkId second_id = test_client_entity_network_id(1, wire_id, 2U);
    kage::sync::ReplicationClient client;

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    const ecs::Entity first_local = client.local_entity(first_id);
    REQUIRE(first_local);

    kage::sync::EntityReference alive{first_local, first_id};
    REQUIRE(client.resolve_entity_reference(alive) == kage::sync::EntityReferenceStatus::Alive);
    REQUIRE(alive.entity == first_local);
    REQUIRE(alive.client_entity_network_id == first_id);

    kage::sync::EntityReference destroyed{first_local, first_id};
    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client.resolve_entity_reference(destroyed) == kage::sync::EntityReferenceStatus::Destroyed);
    REQUIRE_FALSE(destroyed.entity);
    REQUIRE(destroyed.client_entity_network_id == kage::sync::invalid_client_entity_network_id);

    REQUIRE(client.receive(client_registry, make_position_packet(3, {{server_entity, Position{5.0f, 6.0f}}})));
    const ecs::Entity second_local = client.local_entity(second_id);
    REQUIRE(second_local);
    REQUIRE(second_local != first_local);

    kage::sync::EntityReference stale{first_local, first_id};
    REQUIRE(client.resolve_entity_reference(stale) == kage::sync::EntityReferenceStatus::Destroyed);
    REQUIRE_FALSE(stale.entity);
    REQUIRE(stale.client_entity_network_id == kage::sync::invalid_client_entity_network_id);

    kage::sync::EntityReference reused{ecs::Entity{}, second_id};
    REQUIRE(client.resolve_entity_reference(reused) == kage::sync::EntityReferenceStatus::Alive);
    REQUIRE(reused.entity == second_local);
    REQUIRE(reused.client_entity_network_id == second_id);
}

TEST_CASE("predicted client mode requires ShouldRollBack traits") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);

    REQUIRE_THROWS_AS(
        client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})),
        std::logic_error);
}

TEST_CASE("predicted client snaps first frame predicts locally and skips matching rollback") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);

    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{1.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 2.0f);
}

TEST_CASE("predicted client rolls back and resimulates mismatched frames") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    options.rollback_policy = kage::sync::ReplicationRollbackPolicy::All;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 3.0f);
}

#ifdef KAGE_SYNC_ENABLE_TRACING
TEST_CASE("predicted client traces rollback reason separately from rollback conflict") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    std::vector<kage::sync::SyncTraceEvent> events;
    kage::sync::SyncTracer tracer;
    tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { events.push_back(event); }});
    client.set_tracer(&tracer);

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));

    const auto conflict = std::find_if(events.begin(), events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PredictionRollbackConflict;
    });
    REQUIRE(conflict != events.end());
    REQUIRE(conflict->component == position_component);
    REQUIRE(conflict->component_name == "PredictedPosition");
    REQUIRE(conflict->data.empty());

    const auto reason = std::find_if(events.begin(), events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::RollbackReason;
    });
    REQUIRE(reason != events.end());
    REQUIRE(std::count_if(events.begin(), events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::RollbackReason;
    }) == 1);
    REQUIRE(reason->component == position_component);
    REQUIRE(reason->component_name == "PredictedPosition");
    REQUIRE(reason->data == "PredictedPosition.x mismatch");
}

TEST_CASE("predicted cue rollback tracing records server mismatch reason") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(registry);
    kage::sync::configure_client(registry, 1);

    bool emit_prediction_cue = true;
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each(
        [&](ecs::Entity entity, PredictedPosition& position) {
            position.x += 1.0f;
            if (emit_prediction_cue) {
                REQUIRE(kage::sync::emit_cue(registry, entity, TestCue{7}, 1.0f));
                emit_prediction_cue = false;
            }
        });

    std::vector<kage::sync::SyncTraceEvent> events;
    kage::sync::SyncTracer tracer;
    tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { events.push_back(event); }});
    client.set_tracer(&tracer);

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));

    REQUIRE(std::any_of(events.begin(), events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::CueRolledBack &&
            event.data.find("rollback_reason=server_mismatch") != std::string::npos;
    }));
}

TEST_CASE("predicted cue rollback tracing records resim omission reason") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(registry);
    kage::sync::configure_client(registry, 1);

    int prediction_jobs = 0;
    bool emit_during_resim = false;
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    options.rollback_policy = kage::sync::ReplicationRollbackPolicy::All;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each(
        [&](ecs::Entity entity, PredictedPosition& position) {
            position.x += 1.0f;
            ++prediction_jobs;
            if (prediction_jobs == 2 || emit_during_resim) {
                REQUIRE(kage::sync::emit_cue(registry, entity, TestCue{9}, 1.0f));
            }
        });

    std::vector<kage::sync::SyncTraceEvent> events;
    kage::sync::SyncTracer tracer;
    tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { events.push_back(event); }});
    client.set_tracer(&tracer);

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));

    REQUIRE(std::any_of(events.begin(), events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::CueRolledBack &&
            event.data.find("rollback_reason=resim_not_replayed") != std::string::npos;
    }));
}
#endif

TEST_CASE("predicted client error blends display-interpolated resim corrections") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    REQUIRE(kage::sync::set_display_interpolated<PredictedPosition>(registry));
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 3.0f);

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(registry);
    REQUIRE(display.entities.size() == 1);
    PredictedPosition shown;
    REQUIRE(display.entities[0].try_get_display_value(registry, shown));
    REQUIRE(shown.x == Catch::Approx(2.2f));
}

TEST_CASE("predicted client display samples prediction history between fixed ticks") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    REQUIRE(kage::sync::set_display_interpolated<PredictedPosition>(registry));
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds * 0.5));

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(registry);
    REQUIRE(display.entities.size() == 1);
    PredictedPosition shown;
    REQUIRE(display.entities[0].try_get_display_value(registry, shown));
    REQUIRE(shown.x == Catch::Approx(2.7f));
}

TEST_CASE("predicted client keeps local prediction phase when authoritative frames arrive early") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f}, 1)));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);

    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds * 0.5));
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{1.0f, 0.0f}, 2)));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds * 0.5));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
}

TEST_CASE("predicted client ONLY_AFFECTED resim runs jobs for affected entities") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    int calls = 0;

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    options.rollback_policy = kage::sync::ReplicationRollbackPolicy::OnlyAffected;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([&](ecs::Entity, PredictedPosition& position) {
        ++calls;
        position.x += 1.0f;
    });

    const ecs::Entity first_server = registry.create();
    const ecs::Entity second_server = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, first_server, PredictedPosition{0.0f, 0.0f}, 1)));
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, second_server, PredictedPosition{0.0f, 0.0f}, 2)));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(calls == 4);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, first_server, PredictedPosition{2.0f, 0.0f}, 3)));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, second_server, PredictedPosition{1.0f, 0.0f}, 4)));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));

    REQUIRE(calls == 7);
    REQUIRE(registry.get<PredictedPosition>(client.local_entity(first_server)).x == 4.0f);
    REQUIRE(registry.get<PredictedPosition>(client.local_entity(second_server)).x == 3.0f);
}

TEST_CASE("predicted client cosmetic jobs do not run during resimulation") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    int simulation_calls = 0;
    int cosmetic_calls = 0;

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    options.rollback_policy = kage::sync::ReplicationRollbackPolicy::All;
    kage::sync::ReplicationClient client(options);

    client.simulation_job<PredictedPosition>(registry, 0).each([&](ecs::Entity, PredictedPosition& position) {
        ++simulation_calls;
        position.x += 1.0f;
    });
    const ecs::Entity cosmetic_job =
        client.cosmetic_job<PredictedPosition>(registry, 1).each([&](ecs::Entity, PredictedPosition&) {
            ++cosmetic_calls;
        });
    REQUIRE(registry.has<kage::sync::NoResim>(cosmetic_job));

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(simulation_calls == 2);
    REQUIRE(cosmetic_calls == 2);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));

    REQUIRE(simulation_calls == 4);
    REQUIRE(cosmetic_calls == 3);
}

TEST_CASE("client simulation jobs skip NoSimulate entities") {
    ecs::Registry registry;
    registry.register_component<PredictedPosition>("PredictedPosition");
    kage::sync::configure_client(registry, 1);

    const ecs::Entity simulated = registry.create();
    const ecs::Entity skipped = registry.create();
    REQUIRE(registry.add<PredictedPosition>(simulated, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<PredictedPosition>(skipped, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<kage::sync::NoSimulate>(skipped));

    kage::sync::ReplicationClient client;
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    registry.run_jobs();

    REQUIRE(registry.get<PredictedPosition>(simulated).x == 1.0f);
    REQUIRE(registry.get<PredictedPosition>(skipped).x == 0.0f);
}

TEST_CASE("predicted client applies authoritative destroys immediately") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    const ecs::Entity server_entity = registry.create();
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(registry.alive(local));

    REQUIRE(client.receive(registry, make_destroy_packet(2, server_entity)));
    REQUIRE_FALSE(registry.alive(local));
    REQUIRE_FALSE(client.local_entity(server_entity));
}

TEST_CASE("replication client decodes ACKed delta updates") {
    ecs::Registry server_registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(server_entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "NetworkedActor",
        {{client_position, kage::sync::ReplicationAudience::All}});
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    server_registry.write<NetworkedPosition>(server_entity) = NetworkedPosition{2.0f, 3.0f};
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));

    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<NetworkedPosition>(local).x == 2.0f);
    REQUIRE(client_registry.get<NetworkedPosition>(local).y == 3.0f);
}

TEST_CASE("replication client decodes deltas against the encoded baseline frame") {
    ecs::Registry server_registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(server_entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "NetworkedActor",
        {{client_position, kage::sync::ReplicationAudience::All}});
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    server_registry.write<NetworkedPosition>(server_entity) = NetworkedPosition{2.0f, 3.0f};
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.drain_ack_packets().size() == 1);

    server_registry.write<NetworkedPosition>(server_entity) = NetworkedPosition{3.0f, 4.0f};
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));

    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<NetworkedPosition>(local).x == 3.0f);
    REQUIRE(client_registry.get<NetworkedPosition>(local).y == 4.0f);
}

TEST_CASE("replication client decodes mixed-baseline deltas in one packet") {
    ecs::Registry server_registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity first = server_registry.create();
    const ecs::Entity second = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(server_registry.add<NetworkedPosition>(second, NetworkedPosition{10.0f, 10.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.mtu_bytes = 1200;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, first, server_archetype));
    REQUIRE(start_sync(server_registry, second, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "NetworkedActor",
        {{client_position, kage::sync::ReplicationAudience::All}});
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);
    const UpdatePacket full_update = read_update(packets.back());
    REQUIRE(full_update.records.size() == 2);
    std::uint32_t first_network_id = 0;
    std::uint32_t second_network_id = 0;
    first_network_id = full_update.records[0].network_id;
    second_network_id = full_update.records[1].network_id;
    REQUIRE(first_network_id != 0);
    REQUIRE(second_network_id != 0);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    server_registry.write<NetworkedPosition>(first) = NetworkedPosition{2.0f, 2.0f};
    server_registry.write<NetworkedPosition>(second) = NetworkedPosition{11.0f, 11.0f};
    packets.clear();
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);
    const UpdatePacket frame83 = read_update(packets.back());
    REQUIRE(frame83.records.size() == 2);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(server.acknowledge_entity(1, first, frame83.frame));

    server_registry.write<NetworkedPosition>(first) = NetworkedPosition{3.0f, 3.0f};
    server_registry.write<NetworkedPosition>(second) = NetworkedPosition{12.0f, 12.0f};
    packets.clear();
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);
    const UpdatePacket frame84 = read_update(packets.back());
    REQUIRE(frame84.records.size() == 2);

    bool saw_first_from_83 = false;
    bool saw_second_from_full = false;
    for (const UpdateRecord& record : frame84.records) {
        REQUIRE_FALSE(record.destroy);
        REQUIRE_FALSE(record.full);
        if (record.network_id == first_network_id) {
            saw_first_from_83 = record.baseline_frame == frame83.frame;
        } else if (record.network_id == second_network_id) {
            saw_second_from_full = record.baseline_frame == full_update.frame;
        }
    }
    REQUIRE(saw_first_from_83);
    REQUIRE(saw_second_from_full);

    REQUIRE(client.receive(client_registry, packets.back()));
    const ecs::Entity first_local = client.local_entity(test_client_entity_network_id(1, first_network_id));
    const ecs::Entity second_local = client.local_entity(test_client_entity_network_id(1, second_network_id));
    REQUIRE(client_registry.get<NetworkedPosition>(first_local).x == 3.0f);
    REQUIRE(client_registry.get<NetworkedPosition>(first_local).y == 3.0f);
    REQUIRE(client_registry.get<NetworkedPosition>(second_local).x == 12.0f);
    REQUIRE(client_registry.get<NetworkedPosition>(second_local).y == 12.0f);
}

TEST_CASE("replication client rejects invalid deltas without ACKing") {
    ecs::Registry registry;
    const ecs::Entity position =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    kage::sync::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_update_message, 8U);
    packet.push_bits(1, 32U);
    packet.push_bits(1, kage::sync::protocol::server_packet_id_bits);
    packet.push_bits(0, 32U);
    packet.push_bits(1, 16U);
    packet.push_bool(false);
    kage::sync::protocol::write_network_entity_id(packet, 1);
    packet.push_bool(false);
    packet.push_bits(1, 16U);
    packet.push_bits(0, kage::sync::protocol::bits_for_range(2U));
    packet.push_bits(17, 32U);
    packet.push_bool(true);
    packet.push_bits(10, 8U);
    packet.push_bits(10, 8U);

    kage::sync::ReplicationClient client;
    REQUIRE_FALSE(client.receive(registry, packet));
    REQUIRE(client.pending_ack_count() == 0);
    REQUIRE(client.drain_ack_packets().empty());
}

TEST_CASE("replication client rejects malformed update packets without ACKing") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    kage::sync::ReplicationClient client;

    kage::sync::BitBuffer empty;
    REQUIRE_FALSE(client.receive(registry, empty));

    kage::sync::BitBuffer wrong_message;
    wrong_message.push_bits(kage::sync::protocol::client_ack_message, 8U);
    REQUIRE_FALSE(client.receive(registry, wrong_message));

    kage::sync::BitBuffer truncated_header;
    truncated_header.push_bits(kage::sync::protocol::server_update_message, 8U);
    truncated_header.push_bits(1, 32U);
    REQUIRE_FALSE(client.receive(registry, truncated_header));

    kage::sync::BitBuffer invalid_archetype;
    invalid_archetype.push_bits(kage::sync::protocol::server_update_message, 8U);
    invalid_archetype.push_bits(1, 32U);
    invalid_archetype.push_bits(1, kage::sync::protocol::server_packet_id_bits);
    invalid_archetype.push_bits(0, 32U);
    invalid_archetype.push_bits(1, 16U);
    invalid_archetype.push_bool(false);
    kage::sync::protocol::write_network_entity_id(invalid_archetype, 1);
    invalid_archetype.push_bool(true);
    invalid_archetype.push_bits(99, 32U);
    REQUIRE_FALSE(client.receive(registry, invalid_archetype));

    REQUIRE(client.pending_ack_count() == 0);
    REQUIRE(client.drain_ack_packets().empty());
}

TEST_CASE("replication client applies destroy records and ACKs tombstones") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClient client;

    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    REQUIRE(server_registry.destroy(server_entity));
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE_FALSE(client_registry.alive(local));

    std::vector<kage::sync::BitBuffer> destroy_acks = client.drain_ack_packets();
    REQUIRE(destroy_acks.size() == 1);
    std::vector<AckRecord> acks = read_acks(destroy_acks[0]);
    REQUIRE(acks.size() == 1);
    REQUIRE(acks[0].packet_id != 0);

    server.tick(server_registry);
    REQUIRE(server.process_packet(1, destroy_acks[0]));
    const std::size_t packets_after_ack = packets.size();
    server.tick(server_registry);
    REQUIRE(packets.size() == packets_after_ack);
}

TEST_CASE("replication client packs ACKs within the configured MTU") {
    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{16});
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);

    std::vector<ecs::Entity> server_entities;
    for (int i = 0; i < 3; ++i) {
        const ecs::Entity entity = server_registry.create();
        REQUIRE(server_registry.add<Position>(entity, Position{static_cast<float>(i), 0.0f}) != nullptr);
        REQUIRE(start_sync(server_registry, entity, server_archetype));
        server_entities.push_back(entity);
    }

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.mtu_bytes = 29;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    for (const kage::sync::BitBuffer& packet : packets) {
        REQUIRE(client.receive(client_registry, packet));
    }

    std::vector<kage::sync::BitBuffer> acks = client.drain_ack_packets();
    REQUIRE(acks.size() == 1);
    for (const kage::sync::BitBuffer& ack : acks) {
        REQUIRE(ack.byte_size() <= 16);
        REQUIRE(read_acks(ack).size() == 3);
    }
}

TEST_CASE("replication client retains ACKs that cannot fit the configured MTU") {
    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{3});
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.pending_ack_count() == 1);
    REQUIRE(client.drain_ack_packets().empty());
    REQUIRE(client.pending_ack_count() == 1);
}

TEST_CASE("replication client rejects duplicate full updates without ACKing again") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.receive(client_registry, packets.back()));
    REQUIRE(client.pending_ack_count() == 0);
}

TEST_CASE("replication clients recover independently when one misses ACK processing") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "NetworkedActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(server_entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& packet) {
        packets.push_back({client, packet});
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_one_registry;
    const ecs::Entity client_one_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_one_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId client_one_archetype = kage::sync::define_archetype(
        client_one_registry,
        "NetworkedActor",
        {{client_one_position, kage::sync::ReplicationAudience::All}});
    REQUIRE(client_one_archetype == server_archetype);
    kage::sync::configure_client(client_one_registry, 1);

    ecs::Registry client_two_registry;
    const ecs::Entity client_two_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_two_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId client_two_archetype = kage::sync::define_archetype(
        client_two_registry,
        "NetworkedActor",
        {{client_two_position, kage::sync::ReplicationAudience::All}});
    REQUIRE(client_two_archetype == server_archetype);
    kage::sync::configure_client(client_two_registry, 2);

    kage::sync::ReplicationClient client_one;
    kage::sync::ReplicationClient client_two;

    server.tick(server_registry);
    REQUIRE(packets.size() == 2);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    for (const kage::sync::BitBuffer& ack : client_one.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }
    REQUIRE(client_two.drain_ack_packets().size() == 1);

    server_registry.write<NetworkedPosition>(server_entity) = NetworkedPosition{2.0f, 3.0f};
    packets.clear();
    server.tick(server_registry);
    REQUIRE(packets.size() == 2);

    const UpdatePacket client_one_update = read_update(packet_for(packets, 1));
    const UpdatePacket client_two_update = read_update(packet_for(packets, 2));
    REQUIRE(client_one_update.records.size() == 1);
    REQUIRE(client_two_update.records.size() == 1);
    REQUIRE_FALSE(client_one_update.records[0].full);
    REQUIRE(client_two_update.records[0].full);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));

    for (const kage::sync::BitBuffer& ack : client_one.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }
    for (const kage::sync::BitBuffer& ack : client_two.drain_ack_packets()) {
        REQUIRE(server.process_packet(2, ack));
    }

    const ecs::Entity client_one_local = client_one.local_entity(server_entity);
    const ecs::Entity client_two_local = client_two.local_entity(server_entity);
    REQUIRE(client_one_registry.get<NetworkedPosition>(client_one_local).x == 2.0f);
    REQUIRE(client_one_registry.get<NetworkedPosition>(client_one_local).y == 3.0f);
    REQUIRE(client_two_registry.get<NetworkedPosition>(client_two_local).x == 2.0f);
    REQUIRE(client_two_registry.get<NetworkedPosition>(client_two_local).y == 3.0f);

    server_registry.write<NetworkedPosition>(server_entity) = NetworkedPosition{4.0f, 5.0f};
    packets.clear();
    server.tick(server_registry);
    REQUIRE(packets.size() == 2);
    REQUIRE_FALSE(read_update(packet_for(packets, 1)).records[0].full);
    REQUIRE_FALSE(read_update(packet_for(packets, 2)).records[0].full);
}

TEST_CASE("replication clients ACK destroy records independently") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& packet) {
        packets.push_back({client, packet});
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_one_registry;
    const kage::sync::SyncArchetypeId client_one_archetype =
        kage_sync_tests::define_position_archetype(client_one_registry);
    REQUIRE(client_one_archetype == server_archetype);
    kage::sync::configure_client(client_one_registry, 1);

    ecs::Registry client_two_registry;
    const kage::sync::SyncArchetypeId client_two_archetype =
        kage_sync_tests::define_position_archetype(client_two_registry);
    REQUIRE(client_two_archetype == server_archetype);
    kage::sync::configure_client(client_two_registry, 2);

    kage::sync::ReplicationClient client_one;
    kage::sync::ReplicationClient client_two;

    server.tick(server_registry);
    REQUIRE(packets.size() == 2);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    const ecs::Entity client_one_local = client_one.local_entity(server_entity);
    const ecs::Entity client_two_local = client_two.local_entity(server_entity);
    REQUIRE(client_one_local);
    REQUIRE(client_two_local);
    for (const kage::sync::BitBuffer& ack : client_one.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }
    for (const kage::sync::BitBuffer& ack : client_two.drain_ack_packets()) {
        REQUIRE(server.process_packet(2, ack));
    }

    REQUIRE(server_registry.destroy(server_entity));
    packets.clear();
    server.tick(server_registry);
    REQUIRE(packets.size() == 2);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    REQUIRE_FALSE(client_one_registry.alive(client_one_local));
    REQUIRE_FALSE(client_two_registry.alive(client_two_local));

    std::vector<kage::sync::BitBuffer> client_one_acks = client_one.drain_ack_packets();
    std::vector<kage::sync::BitBuffer> client_two_acks = client_two.drain_ack_packets();
    REQUIRE(client_one_acks.size() == 1);
    REQUIRE(client_two_acks.size() == 1);
    REQUIRE(read_acks(client_one_acks[0]).size() == 1);
    REQUIRE(read_acks(client_two_acks[0]).size() == 1);
    REQUIRE(server.process_packet(1, client_one_acks[0]));

    packets.clear();
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);
    REQUIRE(packets[0].first == 2);
    REQUIRE(server.process_packet(2, client_two_acks[0]));

    packets.clear();
    server.tick(server_registry);
    REQUIRE(packets.empty());
}

TEST_CASE("replication client reconciles components when owner visibility changes") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const ecs::Entity server_health = kage::sync::register_sync_component<Health>(server_registry, "Health");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "OwnedActor",
        {
            {server_position, kage::sync::ReplicationAudience::All},
            {server_health, kage::sync::ReplicationAudience::Owner},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(server_entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{42}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 1));

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& packet) {
        packets.push_back({client, packet});
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_one_registry;
    const ecs::Entity client_one_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_one_registry, "NetworkedPosition");
    const ecs::Entity client_one_health = kage::sync::register_sync_component<Health>(client_one_registry, "Health");
    const kage::sync::SyncArchetypeId client_one_archetype = kage::sync::define_archetype(
        client_one_registry,
        "OwnedActor",
        {
            {client_one_position, kage::sync::ReplicationAudience::All},
            {client_one_health, kage::sync::ReplicationAudience::Owner},
        });
    REQUIRE(client_one_archetype == server_archetype);
    kage::sync::configure_client(client_one_registry, 1);

    ecs::Registry client_two_registry;
    const ecs::Entity client_two_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_two_registry, "NetworkedPosition");
    const ecs::Entity client_two_health = kage::sync::register_sync_component<Health>(client_two_registry, "Health");
    const kage::sync::SyncArchetypeId client_two_archetype = kage::sync::define_archetype(
        client_two_registry,
        "OwnedActor",
        {
            {client_two_position, kage::sync::ReplicationAudience::All},
            {client_two_health, kage::sync::ReplicationAudience::Owner},
        });
    REQUIRE(client_two_archetype == server_archetype);
    kage::sync::configure_client(client_two_registry, 2);

    kage::sync::ReplicationClient client_one;
    kage::sync::ReplicationClient client_two;

    server.tick(server_registry);
    REQUIRE(packets.size() == 2);
    for (const auto& sent : packets) {
        if (sent.first == 1) {
            REQUIRE(client_one.receive(client_one_registry, sent.second));
        } else {
            REQUIRE(client_two.receive(client_two_registry, sent.second));
        }
    }
    for (const kage::sync::BitBuffer& ack : client_one.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }
    for (const kage::sync::BitBuffer& ack : client_two.drain_ack_packets()) {
        REQUIRE(server.process_packet(2, ack));
    }

    const ecs::Entity client_one_local = client_one.local_entity(server_entity);
    const ecs::Entity client_two_local = client_two.local_entity(server_entity);
    REQUIRE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE_FALSE(client_two_registry.contains<Health>(client_two_local));

    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 2));
    packets.clear();
    server.tick(server_registry);
    REQUIRE(packets.size() == 2);
    for (const auto& sent : packets) {
        if (sent.first == 1) {
            REQUIRE(client_one.receive(client_one_registry, sent.second));
        } else {
            REQUIRE(client_two.receive(client_two_registry, sent.second));
        }
    }

    REQUIRE_FALSE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE(client_two_registry.contains<Health>(client_two_local));
    REQUIRE(client_two_registry.get<Health>(client_two_local).value == 42);
}

TEST_CASE("replication client applies synced tags and owner-filtered tag visibility") {
    ecs::Registry server_registry;
    const ecs::Entity server_visible = server_registry.register_component<Visible>("Visible");
    const ecs::Entity server_secret = server_registry.register_component<Secret>("Secret");
    const ecs::Entity server_position =
        kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        kage::sync::SyncArchetypeDesc{
            "TaggedActor",
            {{server_visible, kage::sync::ReplicationAudience::All},
             {server_secret, kage::sync::ReplicationAudience::Owner}},
            {{server_position, kage::sync::ReplicationAudience::All}},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(server_entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add_tag(server_entity, server_visible));
    REQUIRE(server_registry.add_tag(server_entity, server_secret));
    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 1));

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& packet) {
        packets.push_back({client, packet});
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry owner_registry;
    const ecs::Entity owner_visible = owner_registry.register_component<Visible>("Visible");
    const ecs::Entity owner_secret = owner_registry.register_component<Secret>("Secret");
    const ecs::Entity owner_position =
        kage::sync::register_sync_component<NetworkedPosition>(owner_registry, "NetworkedPosition");
    REQUIRE(kage::sync::define_archetype(
                owner_registry,
                kage::sync::SyncArchetypeDesc{
                    "TaggedActor",
                    {{owner_visible, kage::sync::ReplicationAudience::All},
                     {owner_secret, kage::sync::ReplicationAudience::Owner}},
                    {{owner_position, kage::sync::ReplicationAudience::All}},
                }) == server_archetype);
    kage::sync::configure_client(owner_registry, 1);

    ecs::Registry non_owner_registry;
    const ecs::Entity non_owner_visible = non_owner_registry.register_component<Visible>("Visible");
    const ecs::Entity non_owner_secret = non_owner_registry.register_component<Secret>("Secret");
    const ecs::Entity non_owner_position =
        kage::sync::register_sync_component<NetworkedPosition>(non_owner_registry, "NetworkedPosition");
    REQUIRE(kage::sync::define_archetype(
                non_owner_registry,
                kage::sync::SyncArchetypeDesc{
                    "TaggedActor",
                    {{non_owner_visible, kage::sync::ReplicationAudience::All},
                     {non_owner_secret, kage::sync::ReplicationAudience::Owner}},
                    {{non_owner_position, kage::sync::ReplicationAudience::All}},
                }) == server_archetype);
    kage::sync::configure_client(non_owner_registry, 2);

    kage::sync::ReplicationClient owner_client;
    kage::sync::ReplicationClient non_owner_client;
    server.tick(server_registry);
    REQUIRE(owner_client.receive(owner_registry, packet_for(packets, 1)));
    REQUIRE(non_owner_client.receive(non_owner_registry, packet_for(packets, 2)));

    const ecs::Entity owner_local = owner_client.local_entity(server_entity);
    const ecs::Entity non_owner_local = non_owner_client.local_entity(server_entity);
    REQUIRE(owner_registry.has(owner_local, owner_visible));
    REQUIRE(owner_registry.has(owner_local, owner_secret));
    REQUIRE(non_owner_registry.has(non_owner_local, non_owner_visible));
    REQUIRE_FALSE(non_owner_registry.has(non_owner_local, non_owner_secret));

    for (const kage::sync::BitBuffer& ack : owner_client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }
    for (const kage::sync::BitBuffer& ack : non_owner_client.drain_ack_packets()) {
        REQUIRE(server.process_packet(2, ack));
    }

    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 2));
    packets.clear();
    server.tick(server_registry);
    REQUIRE(owner_client.receive(owner_registry, packet_for(packets, 1)));
    REQUIRE(non_owner_client.receive(non_owner_registry, packet_for(packets, 2)));
    REQUIRE(owner_registry.has(owner_local, owner_visible));
    REQUIRE_FALSE(owner_registry.has(owner_local, owner_secret));
    REQUIRE(non_owner_registry.has(non_owner_local, non_owner_visible));
    REQUIRE(non_owner_registry.has(non_owner_local, non_owner_secret));
}

TEST_CASE("buffered interpolation delays entity creation until the target frame") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.local_entity(server_entity));

    REQUIRE_FALSE(client.apply_frame(client_registry, 2));
    REQUIRE_FALSE(client.local_entity(server_entity));
    REQUIRE(client.apply_frame(client_registry, 3));

    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(client_registry.get<Position>(local).y == 2.0f);
}

TEST_CASE("entity mode selector chooses snap or buffered from decoded component data") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    const ecs::Entity snap_entity{41};
    const ecs::Entity buffered_entity{42};
    int selector_calls = 0;

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.interpolation_buffer_frames = 2;
    options.interpolation_buffer_capacity_frames = 8;
    options.entity_mode_selector = [&](const kage::sync::ReplicatedEntityUpdateView& update) {
        Position position;
        REQUIRE(update.try_get(client_registry, position));
        REQUIRE(update.archetype == client_archetype);
        ++selector_calls;
        return position.x > 10.0f
            ? kage::sync::ReplicationClientMode::BufferedInterpolation
            : kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(
            1,
            {
                {snap_entity, Position{1.0f, 2.0f}},
                {buffered_entity, Position{20.0f, 3.0f}},
            })));

    REQUIRE(selector_calls == 2);
    REQUIRE(client.entity_mode(snap_entity) == kage::sync::ReplicationClientMode::Snap);
    REQUIRE(client.entity_mode(buffered_entity) == kage::sync::ReplicationClientMode::BufferedInterpolation);

    const ecs::Entity snap_local = client.local_entity(snap_entity);
    REQUIRE(snap_local);
    REQUIRE(client_registry.get<Position>(snap_local).x == 1.0f);
    REQUIRE_FALSE(client.local_entity(buffered_entity));

    REQUIRE(client.apply_frame(client_registry, 3));
    const ecs::Entity buffered_local = client.local_entity(buffered_entity);
    REQUIRE(buffered_local);
    REQUIRE(client_registry.get<Position>(buffered_local).x == 20.0f);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, buffered_entity)));
    REQUIRE(client.apply_frame(client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(buffered_local));

    REQUIRE(client.receive(client_registry, make_position_packet(4, {{buffered_entity, Position{2.0f, 3.0f}}})));
    REQUIRE(selector_calls == 3);
    REQUIRE(client.entity_mode(buffered_entity) == kage::sync::ReplicationClientMode::Snap);
    const ecs::Entity recreated_local = client.local_entity(buffered_entity);
    REQUIRE(recreated_local);
    REQUIRE(client_registry.get<Position>(recreated_local).x == 2.0f);
}

TEST_CASE("set entity mode rejects unknown entities") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    REQUIRE_FALSE(client.set_entity_mode(
        client_registry,
        ecs::Entity{42},
        kage::sync::ReplicationClientMode::BufferedInterpolation));
}

TEST_CASE("set entity mode switches snap entities to buffered for future updates") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);

    REQUIRE(client.set_entity_mode(
        client_registry,
        server_entity,
        kage::sync::ReplicationClientMode::BufferedInterpolation));
    REQUIRE(client.entity_mode(server_entity) == kage::sync::ReplicationClientMode::BufferedInterpolation);

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{5.0f, 2.0f}}})));
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(client.apply_frame(client_registry, 3));
    REQUIRE(client_registry.get<Position>(local).x == 5.0f);
}

TEST_CASE("set entity mode switches buffered entities to snap immediately") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{7.0f, 2.0f}}})));
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(client.set_entity_mode(client_registry, server_entity, kage::sync::ReplicationClientMode::Snap));
    REQUIRE(client_registry.get<Position>(local).x == 7.0f);
}

TEST_CASE("set entity mode switches buffered entities to predict and seeds prediction") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    registry.job<PredictedPosition>(0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.apply_frame(registry, 2));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{1.0f, 0.0f})));
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);
    REQUIRE(client.set_entity_mode(registry, server_entity, kage::sync::ReplicationClientMode::Predict));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);

    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 2.0f);
}

TEST_CASE("set entity mode switches an entity by client network id") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClient client;
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f}, 1)));

    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client.entity_mode(server_entity) == kage::sync::ReplicationClientMode::Snap);

    const kage::sync::ClientEntityNetworkId network_id =
        test_client_entity_network_id(1, test_network_id(server_entity));
    REQUIRE(client.set_entity_mode(registry, network_id, kage::sync::ReplicationClientMode::Predict));
    REQUIRE(client.entity_mode(server_entity) == kage::sync::ReplicationClientMode::Predict);

    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
}

TEST_CASE("set entity mode by client network id switches buffered delayed destroys to predict immediately") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    const ecs::Entity server_entity = registry.create();
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{1.0f, 0.0f})));
    REQUIRE(client.apply_frame(registry, 2));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);

    REQUIRE(client.receive(registry, make_destroy_packet(2, server_entity)));
    REQUIRE(registry.alive(local));
    const kage::sync::ClientEntityNetworkId network_id =
        test_client_entity_network_id(1, test_network_id(server_entity));
    REQUIRE(client.set_default_entity_mode(kage::sync::ReplicationClientMode::Predict));
    REQUIRE(client.set_entity_mode(registry, network_id, kage::sync::ReplicationClientMode::Predict));
    REQUIRE_FALSE(registry.alive(local));
    REQUIRE(client.entity_mode(server_entity) == kage::sync::ReplicationClientMode::Predict);
}

TEST_CASE("set entity mode switches buffered delayed destroys to snap immediately") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.set_entity_mode(client_registry, server_entity, kage::sync::ReplicationClientMode::Snap));
    REQUIRE_FALSE(client_registry.alive(local));
    REQUIRE_FALSE(client.local_entity(server_entity));
    REQUIRE_FALSE(client.set_entity_mode(
        client_registry,
        server_entity,
        kage::sync::ReplicationClientMode::BufferedInterpolation));
}

TEST_CASE("snap-selected entities do not require interpolation trait hooks") {
    ecs::Registry client_registry;
    const ecs::Entity position = kage::sync::register_sync_component<Position>(client_registry, "Position");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "PositionActor",
        {{position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
}

TEST_CASE("buffered interpolation fills skipped frames with component trait interpolation") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<SmoothPosition>(server_registry, "SmoothPosition");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "SmoothActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<SmoothPosition>(server_entity, SmoothPosition{0.0f, 0.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server.tick(server_registry);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{20.0f, 0.0f};
    server.tick(server_registry);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{30.0f, 0.0f};
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(client.apply_frame(client_registry, 4));
    ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 10.0f);
    REQUIRE(client.apply_frame(client_registry, 5));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 20.0f);
    REQUIRE(client.apply_frame(client_registry, 6));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 30.0f);
}

TEST_CASE("buffered interpolation floors synced tags") {
    ecs::Registry server_registry;
    const ecs::Entity server_visible = server_registry.register_component<Visible>("Visible");
    const ecs::Entity server_secret = server_registry.register_component<Secret>("Secret");
    const ecs::Entity server_position =
        kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        kage::sync::SyncArchetypeDesc{
            "TaggedActor",
            {{server_visible, kage::sync::ReplicationAudience::All},
             {server_secret, kage::sync::ReplicationAudience::All}},
            {{server_position, kage::sync::ReplicationAudience::All}},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(server_entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add_tag(server_entity, server_visible));

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_visible = client_registry.register_component<Visible>("Visible");
    const ecs::Entity client_secret = client_registry.register_component<Secret>("Secret");
    const ecs::Entity client_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                kage::sync::SyncArchetypeDesc{
                    "TaggedActor",
                    {{client_visible, kage::sync::ReplicationAudience::All},
                     {client_secret, kage::sync::ReplicationAudience::All}},
                    {{client_position, kage::sync::ReplicationAudience::All}},
                }) == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    REQUIRE(server_registry.add_tag(server_entity, server_secret));
    packets.clear();
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(client_registry.has(local, client_visible));
    REQUIRE_FALSE(client_registry.has(local, client_secret));

    REQUIRE(client.apply_frame(client_registry, 3));
    REQUIRE(client_registry.has(local, client_visible));
    REQUIRE(client_registry.has(local, client_secret));
}

TEST_CASE("buffered interpolation delays component removal and entity destroy") {
    ecs::Registry server_registry;
    const ecs::Entity server_position = kage::sync::register_sync_component<Position>(server_registry, "Position");
    const ecs::Entity server_health = kage::sync::register_sync_component<Health>(server_registry, "Health");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "Actor",
        {
            {server_position, kage::sync::ReplicationAudience::All},
            {server_health, kage::sync::ReplicationAudience::All},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{7}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position = kage::sync::register_sync_component<Position>(client_registry, "Position");
    const ecs::Entity client_health = kage::sync::register_sync_component<Health>(client_registry, "Health");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "Actor",
        {
            {client_position, kage::sync::ReplicationAudience::All},
            {client_health, kage::sync::ReplicationAudience::All},
        });
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(client_registry.contains<Health>(local));
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    packets.clear();
    REQUIRE(server_registry.remove<Health>(server_entity));
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.apply_frame(client_registry, 2));
    REQUIRE(client_registry.contains<Health>(local));
    REQUIRE(client.apply_frame(client_registry, 3));
    REQUIRE_FALSE(client_registry.contains<Health>(local));
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    packets.clear();
    REQUIRE(server_registry.destroy(server_entity));
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.apply_frame(client_registry, 3));
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.apply_frame(client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(local));
}

TEST_CASE("buffered interpolation rejects interpolated components without trait hooks") {
    ecs::Registry server_registry;
    const ecs::Entity server_position = kage::sync::register_sync_component<Position>(server_registry, "Position");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);

    ecs::Registry client_registry;
    const ecs::Entity client_position = kage::sync::register_sync_component<Position>(client_registry, "Position");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "PositionActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    REQUIRE_FALSE(client.receive(client_registry, packets.back()));
    REQUIRE(client.pending_ack_count() == 0);
    REQUIRE_FALSE(client.local_entity(server_entity));
}

TEST_CASE("buffered interpolation keeps step components held between interpolated samples") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<SmoothPosition>(server_registry, "SmoothPosition");
    const ecs::Entity server_health = kage::sync::register_sync_component<Health>(server_registry, "Health");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "MixedActor",
        {
            {server_position, kage::sync::ReplicationAudience::All},
            {server_health, kage::sync::ReplicationAudience::All},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<SmoothPosition>(server_entity, SmoothPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{10}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    const ecs::Entity client_health = kage::sync::register_sync_component<Health>(client_registry, "Health");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "MixedActor",
        {
            {client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate},
            {client_health, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Step},
        });
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }
    REQUIRE(client.apply_frame(client_registry, 3));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);

    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{20};
    server.tick(server_registry);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{20.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{30};
    server.tick(server_registry);
    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{30.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{40};
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(client.apply_frame(client_registry, 4));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 10.0f);
    REQUIRE(client_registry.get<Health>(local).value == 10);
    REQUIRE(client.apply_frame(client_registry, 5));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 20.0f);
    REQUIRE(client_registry.get<Health>(local).value == 10);
    REQUIRE(client.apply_frame(client_registry, 6));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 30.0f);
    REQUIRE(client_registry.get<Health>(local).value == 40);
}

TEST_CASE("display interpolation samples fractional frames without mutating ECS") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<SmoothPosition>(server_registry, "SmoothPosition");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "SmoothActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<SmoothPosition>(server_entity, SmoothPosition{0.0f, 0.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, client_position));
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    packets.clear();
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server.tick(server_registry);
    const UpdatePacket update = read_update(packets.back(), 1U);
    REQUIRE(update.records.size() == 1);
    const kage::sync::ClientEntityNetworkId client_entity_network_id =
        test_client_entity_network_id(1, update.records[0].network_id);
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 0.0f);

    kage::sync::DisplayInterpolationSampleBuffer display;
    REQUIRE(client.sample_display_interpolation_frame(client_registry, 2.5, display));
    REQUIRE(display.entities.size() == 1);
    REQUIRE(display.entities[0].client_entity_network_id == client_entity_network_id);
    REQUIRE(display.entities[0].local_entity == local);
    REQUIRE(display.entities[0].frame == 1);
    REQUIRE(display.entities[0].alpha == Catch::Approx(0.5f));

    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(5.0f));
    REQUIRE(sampled.y == Catch::Approx(0.0f));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 0.0f);
}

TEST_CASE("display interpolation returns floor samples when the next frame is unavailable") {
    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, client_position));
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    const ecs::Entity server_entity{42};
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{7.0f, 3.0f}}})));

    kage::sync::DisplayInterpolationSampleBuffer display;
    REQUIRE(client.sample_display_interpolation_frame(client_registry, 2.75, display));
    REQUIRE(display.entities.size() == 1);

    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(7.0f));
    REQUIRE(sampled.y == Catch::Approx(3.0f));
}

TEST_CASE("display interpolation omits untagged components from samples") {
    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    const ecs::Entity server_entity{42};
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{7.0f, 3.0f}}})));

    kage::sync::DisplayInterpolationSampleBuffer display;
    REQUIRE(client.sample_display_interpolation_frame(client_registry, 2.0, display));
    REQUIRE(display.entities.empty());
}

TEST_CASE("display samples throw for non-display components instead of falling through to ECS") {
    auto define_actor = [](ecs::Registry& registry, bool display_position) {
        const ecs::Entity position =
            kage::sync::register_sync_component<SmoothPosition>(registry, "SmoothPosition");
        const ecs::Entity health = kage::sync::register_sync_component<Health>(registry, "Health");
        if (display_position) {
            REQUIRE(kage::sync::set_display_interpolated(registry, position));
        }
        return kage::sync::define_archetype(
            registry,
            "Actor",
            {
                {position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate},
                {health, kage::sync::ReplicationAudience::All},
            });
    };

    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_actor(server_registry, false);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<SmoothPosition>(server_entity, SmoothPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{100}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);
    server_registry.write<SmoothPosition>(server_entity) = SmoothPosition{10.0f, 0.0f};
    server_registry.write<Health>(server_entity) = Health{0};
    server.tick(server_registry);
    REQUIRE(packets.size() == 2);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_actor(client_registry, true);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    REQUIRE(client.receive(client_registry, packets[0]));
    REQUIRE(client.receive(client_registry, packets[1]));

    kage::sync::DisplayInterpolationSampleBuffer display;
    REQUIRE(client.sample_display_interpolation_frame(client_registry, 2.5, display));
    REQUIRE(display.entities.size() == 1);

    SmoothPosition sampled_position;
    Health sampled_health;
    REQUIRE(display.entities[0].try_get_display_value(client_registry, sampled_position));
    REQUIRE(sampled_position.x == Catch::Approx(5.0f));
    REQUIRE_THROWS_AS(display.entities[0].try_get_display_value(client_registry, sampled_health), std::logic_error);

    REQUIRE(client.sample_display_interpolation_frame(client_registry, 3.0, display));
    REQUIRE(display.entities.size() == 1);
    REQUIRE_THROWS_AS(display.entities[0].try_get_display_value(client_registry, sampled_health), std::logic_error);
}

TEST_CASE("display interpolation steps entity destroy at the floor frame") {
    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, client_position));
    const kage::sync::SyncArchetypeId client_archetype = kage::sync::define_archetype(
        client_registry,
        "SmoothActor",
        {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    const ecs::Entity server_entity{42};
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{7.0f, 3.0f}}})));
    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));

    kage::sync::DisplayInterpolationSampleBuffer display;
    REQUIRE(client.sample_display_interpolation_frame(client_registry, 2.5, display));
    REQUIRE(display.entities.size() == 1);

    REQUIRE(client.sample_display_interpolation_frame(client_registry, 3.0, display));
    REQUIRE(display.entities.empty());
}

TEST_CASE("client-owned display frame holds instead of rewinding when buffer depth grows") {
    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, client_position));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}})
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{20.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(3, {{server_entity, Position{30.0f, 0.0f}}})));

    REQUIRE(client.tick(client_registry, 3.0));
    const kage::sync::DisplayInterpolationSampleBuffer& before = client.display_interpolation_frame(client_registry);
    REQUIRE(before.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(before.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(21.5f));

    REQUIRE(client.set_interpolation_buffer_frames(3));
    REQUIRE(client.tick(client_registry, 1.0 / 120.0));
    const kage::sync::DisplayInterpolationSampleBuffer& after = client.display_interpolation_frame(client_registry);
    REQUIRE(after.entities.size() == 1);
    REQUIRE(after.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(21.5f));
}

TEST_CASE("client-owned display frame returns the previous valid sample when target data is missing") {
    ecs::Registry client_registry;
    const ecs::Entity client_position =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, client_position));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{client_position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}})
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 2.0));
    const kage::sync::DisplayInterpolationSampleBuffer& before = client.display_interpolation_frame(client_registry);
    REQUIRE(before.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(before.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));

    REQUIRE(client.tick(client_registry, 1.0));
    const kage::sync::DisplayInterpolationSampleBuffer& after = client.display_interpolation_frame(client_registry);
    REQUIRE(after.entities.size() == 1);
    REQUIRE(after.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));
}

TEST_CASE("client-owned display frame exposes snap and buffered entities in one loop") {
    ecs::Registry client_registry;
    const ecs::Entity smooth =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    const ecs::Entity health = kage::sync::register_sync_component<Health>(client_registry, "Health");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, smooth));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "Actor",
                {
                    {smooth, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate},
                    {health, kage::sync::ReplicationAudience::All},
                })
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    options.fixed_dt_seconds = 1.0;
    options.entity_mode_selector = [](const kage::sync::ReplicatedEntityUpdateView& update) {
        return kage::sync::client_entity_network_id_wire_id(update.client_entity_network_id) ==
                test_network_id(ecs::Entity{42})
            ? kage::sync::ReplicationClientMode::BufferedInterpolation
            : kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{ecs::Entity{42}, Position{10.0f, 0.0f}}}, 3U)));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{ecs::Entity{43}, Position{20.0f, 0.0f}}}, 3U)));
    REQUIRE(client.tick(client_registry, 2.0));

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(client_registry);
    REQUIRE(display.entities.size() == 2);
    int found = 0;
    for (const kage::sync::DisplayInterpolationSample& entity : display.entities) {
        SmoothPosition sampled;
        REQUIRE(entity.try_get_display_value(client_registry, sampled));
        if (entity.client_entity_network_id == test_client_entity_network_id(1, test_network_id(ecs::Entity{42}))) {
            REQUIRE(sampled.x == Catch::Approx(10.0f));
            ++found;
        }
        if (entity.client_entity_network_id == test_client_entity_network_id(1, test_network_id(ecs::Entity{43}))) {
            REQUIRE(sampled.x == Catch::Approx(20.0f));
            ++found;
        }
    }
    REQUIRE(found == 2);
}

TEST_CASE("client-owned display frame keeps previous entities while committing newly valid buffered entities") {
    ecs::Registry client_registry;
    const ecs::Entity smooth =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, smooth));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{smooth, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}})
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.interpolation_buffer_frames = 1;
    options.interpolation_buffer_capacity_frames = 8;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);

    const ecs::Entity existing{42};
    const ecs::Entity incoming{43};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{existing, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 2.0));
    REQUIRE(client.display_interpolation_frame(client_registry).entities.size() == 1);

    REQUIRE(client.set_default_entity_mode(kage::sync::ReplicationClientMode::BufferedInterpolation));
    REQUIRE(client.set_entity_mode(client_registry, existing, kage::sync::ReplicationClientMode::BufferedInterpolation));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{incoming, Position{20.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 1.0));

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(client_registry);
    REQUIRE(display.entities.size() == 2);

    int found = 0;
    for (const kage::sync::DisplayInterpolationSample& entity : display.entities) {
        SmoothPosition sampled;
        REQUIRE(entity.try_get_display_value(client_registry, sampled));
        if (entity.client_entity_network_id == test_client_entity_network_id(1, test_network_id(existing))) {
            REQUIRE(sampled.x == Catch::Approx(10.0f));
            ++found;
        }
        if (entity.client_entity_network_id == test_client_entity_network_id(1, test_network_id(incoming))) {
            REQUIRE(sampled.x == Catch::Approx(20.0f));
            ++found;
        }
    }
    REQUIRE(found == 2);
}

TEST_CASE("snap display error blending uses tick dt without mutating ECS") {
    ecs::Registry client_registry;
    const ecs::Entity smooth =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, smooth));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{smooth, kage::sync::ReplicationAudience::All}})
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{0.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 0.5));
    const kage::sync::DisplayInterpolationSampleBuffer& first = client.display_interpolation_frame(client_registry);
    REQUIRE(first.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(first.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(0.0f));

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{10.0f, 0.0f}}})));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<SmoothPosition>(local).x == Catch::Approx(10.0f));

    REQUIRE(client.tick(client_registry, 0.25));
    const kage::sync::DisplayInterpolationSampleBuffer& blended = client.display_interpolation_frame(client_registry);
    REQUIRE(blended.entities.size() == 1);
    REQUIRE(blended.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(2.5f));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == Catch::Approx(10.0f));

    const kage::sync::DisplayInterpolationSampleBuffer& repeated = client.display_interpolation_frame(client_registry);
    REQUIRE(repeated.entities.size() == 1);
    REQUIRE(repeated.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(2.5f));
}

TEST_CASE("snap display error blending clears after the accumulated tick dt consumes the error") {
    ecs::Registry client_registry;
    const ecs::Entity smooth =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
    REQUIRE(kage::sync::set_display_interpolated(client_registry, smooth));
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "SmoothActor",
                {{smooth, kage::sync::ReplicationAudience::All}})
        .value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{0.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 1.0));

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(client_registry);
    REQUIRE(display.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get_display_value(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));
}

TEST_CASE("display samples throw for unmarked snap components") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{0.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{10.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 0.25));

    const kage::sync::DisplayInterpolationSampleBuffer& display = client.display_interpolation_frame(client_registry);
    REQUIRE(display.entities.size() == 1);
    Position sampled;
    REQUIRE_THROWS_AS(display.entities[0].try_get_display_value(client_registry, sampled), std::logic_error);
}

TEST_CASE("buffered interpolation validates wrapped buffer samples by frame") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 0.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        4});

    for (int frame = 1; frame <= 6; ++frame) {
        packets.clear();
        server_registry.write<Position>(server_entity) = Position{static_cast<float>(frame), 0.0f};
        server.tick(server_registry);
        REQUIRE(client.receive(client_registry, packets.back()));
        for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
            REQUIRE(server.process_packet(1, ack));
        }
    }

    REQUIRE_FALSE(client.apply_frame(client_registry, 3));
    REQUIRE_FALSE(client.local_entity(server_entity));
    REQUIRE(client.apply_frame(client_registry, 7));

    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 6.0f);
}

TEST_CASE("buffered interpolation rejects stale duplicate packets without ACKing") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});

    server.tick(server_registry);
    const kage::sync::BitBuffer full_packet = packets.back();
    REQUIRE(client.receive(client_registry, full_packet));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.receive(client_registry, full_packet));
    REQUIRE(client.pending_ack_count() == 0);

    server_registry.write<Position>(server_entity) = Position{3.0f, 4.0f};
    packets.clear();
    server.tick(server_registry);
    const kage::sync::BitBuffer stale_full_packet = full_packet;
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.receive(client_registry, stale_full_packet));
    REQUIRE(client.pending_ack_count() == 0);

    REQUIRE(server_registry.destroy(server_entity));
    packets.clear();
    server.tick(server_registry);
    const kage::sync::BitBuffer destroy_packet = packets.back();
    REQUIRE(client.receive(client_registry, destroy_packet));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.receive(client_registry, destroy_packet));
    REQUIRE(client.pending_ack_count() == 0);
}

TEST_CASE("replication client rejects stale full after destroy and accepts reused network id") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClient client;

    auto process_client_acks = [&]() {
        for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
            REQUIRE(server.process_packet(1, ack));
        }
    };

    const ecs::Entity first = server_registry.create();
    REQUIRE(server_registry.add<Position>(first, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, first, server_archetype));
    server.tick(server_registry);
    const kage::sync::BitBuffer first_full = packets.back();
    const UpdatePacket first_update = read_update(first_full);
    REQUIRE(first_update.records.size() == 1);
    const std::uint32_t reused_network_id = first_update.records[0].network_id;
    REQUIRE(client.receive(client_registry, first_full));
    process_client_acks();

    packets.clear();
    REQUIRE(server_registry.destroy(first));
    server.tick(server_registry);
    const kage::sync::BitBuffer destroy = packets.back();
    REQUIRE(client.receive(client_registry, destroy));
    process_client_acks();
    REQUIRE_FALSE(client.receive(client_registry, first_full));
    REQUIRE(client.pending_ack_count() == 0);

    packets.clear();
    const ecs::Entity second = server_registry.create();
    REQUIRE(server_registry.add<Position>(second, Position{3.0f, 4.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, second, server_archetype));
    server.tick(server_registry);
    const UpdatePacket second_update = read_update(packets.back());
    REQUIRE(second_update.records.size() == 1);
    REQUIRE(second_update.records[0].network_id == reused_network_id);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.local_entity(test_client_entity_network_id(1, reused_network_id, 2U)));
}

TEST_CASE("buffered interpolation delays component additions from full updates") {
    ecs::Registry server_registry;
    const ecs::Entity server_position = kage::sync::register_sync_component<Position>(server_registry, "Position");
    const ecs::Entity server_health = kage::sync::register_sync_component<Health>(server_registry, "Health");
    const kage::sync::SyncArchetypeId position_archetype = kage::sync::define_archetype(
        server_registry,
        "PositionActor",
        {{server_position, kage::sync::ReplicationAudience::All}});
    const kage::sync::SyncArchetypeId health_archetype = kage::sync::define_archetype(
        server_registry,
        "HealthActor",
        {
            {server_position, kage::sync::ReplicationAudience::All},
            {server_health, kage::sync::ReplicationAudience::All},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, position_archetype));

    ecs::Registry client_registry;
    const ecs::Entity client_position = kage::sync::register_sync_component<Position>(client_registry, "Position");
    const ecs::Entity client_health = kage::sync::register_sync_component<Health>(client_registry, "Health");
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "PositionActor",
                {{client_position, kage::sync::ReplicationAudience::All}}) == position_archetype);
    REQUIRE(kage::sync::define_archetype(
                client_registry,
                "HealthActor",
                {
                    {client_position, kage::sync::ReplicationAudience::All},
                    {client_health, kage::sync::ReplicationAudience::All},
                }) == health_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE_FALSE(client_registry.contains<Health>(local));
    for (const kage::sync::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }

    REQUIRE(server_registry.add<Health>(server_entity, Health{7}) != nullptr);
    REQUIRE(start_sync(server_registry, server_entity, health_archetype));
    packets.clear();
    server.tick(server_registry);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.apply_frame(client_registry, 2));
    REQUIRE_FALSE(client_registry.contains<Health>(local));
    REQUIRE(client.apply_frame(client_registry, 3));
    REQUIRE(client_registry.get<Health>(local).value == 7);
}

TEST_CASE("buffered interpolation delays owner visibility reconciliation") {
    ecs::Registry server_registry;
    const ecs::Entity server_position =
        kage::sync::register_sync_component<NetworkedPosition>(server_registry, "NetworkedPosition");
    const ecs::Entity server_health = kage::sync::register_sync_component<Health>(server_registry, "Health");
    const kage::sync::SyncArchetypeId server_archetype = kage::sync::define_archetype(
        server_registry,
        "OwnedActor",
        {
            {server_position, kage::sync::ReplicationAudience::All},
            {server_health, kage::sync::ReplicationAudience::Owner},
        });
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<NetworkedPosition>(server_entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(server_registry.add<Health>(server_entity, Health{42}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 1));

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId client_id, const kage::sync::BitBuffer& packet) {
        packets.push_back({client_id, packet});
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    ecs::Registry client_one_registry;
    const ecs::Entity client_one_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_one_registry, "NetworkedPosition");
    const ecs::Entity client_one_health = kage::sync::register_sync_component<Health>(client_one_registry, "Health");
    REQUIRE(kage::sync::define_archetype(
                client_one_registry,
                "OwnedActor",
                {
                    {client_one_position, kage::sync::ReplicationAudience::All},
                    {client_one_health, kage::sync::ReplicationAudience::Owner},
                }) == server_archetype);
    kage::sync::configure_client(client_one_registry, 1);

    ecs::Registry client_two_registry;
    const ecs::Entity client_two_position =
        kage::sync::register_sync_component<NetworkedPosition>(client_two_registry, "NetworkedPosition");
    const ecs::Entity client_two_health = kage::sync::register_sync_component<Health>(client_two_registry, "Health");
    REQUIRE(kage::sync::define_archetype(
                client_two_registry,
                "OwnedActor",
                {
                    {client_two_position, kage::sync::ReplicationAudience::All},
                    {client_two_health, kage::sync::ReplicationAudience::Owner},
                }) == server_archetype);
    kage::sync::configure_client(client_two_registry, 2);

    kage::sync::ReplicationClient client_one(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});
    kage::sync::ReplicationClient client_two(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});

    server.tick(server_registry);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    REQUIRE(client_one.apply_frame(client_one_registry, 2));
    REQUIRE(client_two.apply_frame(client_two_registry, 2));
    const ecs::Entity client_one_local = client_one.local_entity(server_entity);
    const ecs::Entity client_two_local = client_two.local_entity(server_entity);
    REQUIRE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE_FALSE(client_two_registry.contains<Health>(client_two_local));
    for (const kage::sync::BitBuffer& ack : client_one.drain_ack_packets()) {
        REQUIRE(server.process_packet(1, ack));
    }
    for (const kage::sync::BitBuffer& ack : client_two.drain_ack_packets()) {
        REQUIRE(server.process_packet(2, ack));
    }

    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 2));
    packets.clear();
    server.tick(server_registry);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    REQUIRE(client_one.apply_frame(client_one_registry, 2));
    REQUIRE(client_two.apply_frame(client_two_registry, 2));
    REQUIRE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE_FALSE(client_two_registry.contains<Health>(client_two_local));

    REQUIRE(client_one.apply_frame(client_one_registry, 3));
    REQUIRE(client_two.apply_frame(client_two_registry, 3));
    REQUIRE_FALSE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE(client_two_registry.get<Health>(client_two_local).value == 42);
}

TEST_CASE("buffered interpolation applies valid entities when another target sample is missing") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});

    const ecs::Entity first{101};
    const ecs::Entity second{102};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{first, Position{1.0f, 0.0f}}, {second, Position{2.0f, 0.0f}}})));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{first, Position{3.0f, 0.0f}}})));

    REQUIRE_FALSE(client.apply_frame(client_registry, 3));
    REQUIRE(client.local_entity(first));
    REQUIRE_FALSE(client.local_entity(second));
    REQUIRE(client_registry.get<Position>(client.local_entity(first)).x == 3.0f);
}

#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
TEST_CASE("buffered interpolation diagnostics count missing target samples") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8});

    const ecs::Entity first{101};
    const ecs::Entity second{102};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{first, Position{1.0f, 0.0f}}, {second, Position{2.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{first, Position{3.0f, 0.0f}}})));

    REQUIRE_FALSE(client.apply_frame(client_registry, 3));
    const kage::sync::ReplicationClientInterpolationDiagnostics& diagnostics = client.interpolation_diagnostics();
    REQUIRE(diagnostics.total_interpolated_entity_frame_checks == 2);
    REQUIRE(diagnostics.total_interpolated_entity_frame_starvations == 1);
    REQUIRE(diagnostics.window_interpolated_entity_frame_checks == 2);
    REQUIRE(diagnostics.window_interpolated_entity_frame_starvations == 1);
    REQUIRE(diagnostics.interpolated_entity_starvation_rate() == Catch::Approx(0.5f));
    REQUIRE(diagnostics.interpolated_entity_starvation_percent() == Catch::Approx(50.0f));
    REQUIRE(diagnostics.lifetime_interpolated_entity_starvation_rate() == Catch::Approx(0.5f));

    for (std::size_t offset = 0; offset < kage::sync::interpolation_diagnostics_window_frames; ++offset) {
        const kage::sync::SyncFrame frame = static_cast<kage::sync::SyncFrame>(3U + offset);
        REQUIRE(client.receive(client_registry, make_position_packet(
            frame,
            {{first, Position{static_cast<float>(frame), 0.0f}}, {second, Position{static_cast<float>(frame), 0.0f}}})));
        REQUIRE(client.apply_frame(client_registry, static_cast<kage::sync::SyncFrame>(frame + 1U)));
    }

    REQUIRE(diagnostics.total_interpolated_entity_frame_starvations == 1);
    REQUIRE(diagnostics.window_interpolated_entity_frame_checks == kage::sync::interpolation_diagnostics_window_frames * 2U);
    REQUIRE(diagnostics.window_interpolated_entity_frame_starvations == 0);
    REQUIRE(diagnostics.interpolated_entity_starvation_percent() == Catch::Approx(0.0f));
    REQUIRE(diagnostics.lifetime_interpolated_entity_starvation_percent() > 0.0f);

    client.reset_interpolation_diagnostics();
    REQUIRE(client.interpolation_diagnostics().total_interpolated_entity_frame_checks == 0);
    REQUIRE(client.interpolation_diagnostics().total_interpolated_entity_frame_starvations == 0);
    REQUIRE(client.interpolation_diagnostics().window_interpolated_entity_frame_checks == 0);
    REQUIRE(client.interpolation_diagnostics().window_interpolated_entity_frame_starvations == 0);
}
#endif

TEST_CASE("buffered interpolation validates client buffer options") {
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            0}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            3}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            4,
            4}),
        std::invalid_argument);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        4});
    REQUIRE(client.set_interpolation_buffer_frames(3));
    REQUIRE_FALSE(client.set_interpolation_buffer_frames(4));
    REQUIRE(client.current_interpolation_buffer_frames() == 3);

    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            4}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            1,
            -1.0f}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            1,
            2.0f,
            0.0f}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            1,
            2.0f,
            0.1f,
            0.0f}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            1,
            2.0f,
            0.1f,
            0.95f,
            0.5f}),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        kage::sync::ReplicationClient(kage::sync::ReplicationClientOptions{
            1200,
            kage::sync::ReplicationClientMode::BufferedInterpolation,
            1,
            4,
            true,
            1,
            2.0f,
            0.1f,
            0.95f,
            1.05f,
            -0.1f}),
        std::invalid_argument);
}

TEST_CASE("frame-aware receive records latency and emits dilation without jumping the buffer") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    const ecs::Entity server_entity{42};

    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().sample_count == 1);
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(client.timing_stats().jitter_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(0.90f));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{3.0f, 4.0f}}}), 4));
    REQUIRE(client.timing_stats().sample_count == 1);
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(client.timing_stats().jitter_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 4);
    REQUIRE(client.current_interpolation_buffer_frames() == 1);
}

TEST_CASE("ping samples compute conservative prediction lead and prediction dilation") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.interpolation_buffer_capacity_frames = 8;
    options.input_buffer_capacity_frames = 16;
    options.auto_interpolation_smoothing = 1.0f;
    options.auto_prediction_time_dilation_min = 0.90f;
    options.auto_prediction_time_dilation_max = 1.10f;
    options.auto_prediction_time_dilation_gain = 0.10f;
    options.auto_timing_fast_recovery = false;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};
    REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 0.0f}));

    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(client.timing_stats().desired_prediction_lead_frames == 9);
    REQUIRE(client.timing_stats().target_prediction_lead_frames == 9);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().measured_prediction_lead_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().current_prediction_lead_frames == 0);
    REQUIRE(client.timing_stats().prediction_time_dilation == Catch::Approx(1.10f));

    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{3.0f, 4.0f}}}), 6));
    REQUIRE(client.timing_stats().measured_prediction_lead_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().prediction_time_dilation == Catch::Approx(1.10f));
}

TEST_CASE("server update lag fast recovery pre-fills future input frames") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions options;
    options.input_buffer_capacity_frames = 32;
    options.auto_timing_warmup_samples = 3;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};
    REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 0.0f}));

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 8));

    REQUIRE(client.timing_stats().target_prediction_lead_frames == 15);
    REQUIRE(client.timing_stats().current_prediction_lead_frames == 15);
    REQUIRE(client.timing_stats().measured_prediction_lead_frames == Catch::Approx(15.0f));
    std::vector<kage::sync::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());
    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.first_input_frame == 1);
    REQUIRE(input.input_count >= 16);
}

TEST_CASE("ping-derived prediction target jump pre-fills future input frames") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions options;
    options.input_buffer_capacity_frames = 32;
    options.auto_interpolation_smoothing = 1.0f;
    options.auto_timing_warmup_samples = 1;
    kage::sync::ReplicationClient client(options);
    const ecs::Entity server_entity{42};
    REQUIRE(client.set_input(client_registry, NetworkedPosition{1.0f, 0.0f}));

    PingPacket ping;
    REQUIRE(drain_ping(client, client_registry, 0.0, ping));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(99, {{server_entity, Position{1.0f, 2.0f}}}),
        72));
    REQUIRE(client.timing_stats().target_prediction_lead_frames == 2);

    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 14));

    REQUIRE(client.timing_stats().target_prediction_lead_frames == 15);
    REQUIRE(client.timing_stats().current_prediction_lead_frames == 15);
    std::vector<kage::sync::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());
    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.first_input_frame <= 114);
    REQUIRE(input.first_input_frame + input.input_count - 1U >= 114);
}

TEST_CASE("replication client tick emits packets through configured sender") {
    ecs::Registry registry;
    kage::sync::configure_client(registry, 1);

    std::vector<kage::sync::BitBuffer> sent;
    kage::sync::ReplicationClient client;
    client.set_packet_sender([&](const kage::sync::BitBuffer& packet) {
        sent.push_back(packet);
    });

    REQUIRE(client.tick(registry, 0.0));
    REQUIRE_FALSE(sent.empty());
}

TEST_CASE("replication client queued receive packets are processed during tick") {
    ecs::Registry registry;
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClient client;
    kage::sync::BitBuffer response;
    response.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    response.push_bits(1U, 1U);
    response.push_unsigned_bits(1U, 64U);
    client.receive_packet(response);

    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds * 0.5));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Accepted);
    REQUIRE(client.continuous_receive_frame() == Catch::Approx(0.5));
}

TEST_CASE("replication client queued stale receive packets do not fail tick") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity server_entity{42};
    const kage::sync::BitBuffer packet =
        make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}});
    REQUIRE(client.receive(registry, packet, 1));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.receive(registry, packet, 1));

    client.receive_packet(packet);
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.pending_ack_count() == 0);
}

TEST_CASE("client input frames are immutable samples from the input clock") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    kage::sync::configure_client(registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(registry));

    kage::sync::ReplicationClient client;
    REQUIRE(client.set_input(registry, NetworkedPosition{3.0f, 4.0f}));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));

    std::vector<kage::sync::BitBuffer> packets = client.drain_packets();
    auto input_packet = std::find_if(packets.begin(), packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != packets.end());

    const ClientInputPacket input = read_client_input_header(*input_packet);
    REQUIRE(input.baseline_frame == 0);
    REQUIRE(input.input_count == 1);
    REQUIRE(client.timing_stats().current_prediction_lead_frames == 2);
}

TEST_CASE("replication client delays server update packet loss until gaps leave the receive window") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(2, {{server_entity, Position{3.0f, 4.0f}}}, 2U, 3U),
        2));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 2);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 0);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client fills packet loss gaps from reordered server update packets") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity first_entity{42};
    const ecs::Entity second_entity{43};
    const ecs::Entity reordered_entity{44};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(3, {{second_entity, Position{3.0f, 4.0f}}}, 2U, 3U),
        3));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(4, {{reordered_entity, Position{2.0f, 3.0f}}}, 2U, 2U),
        4));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 3);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client counts packet loss when gaps leave the receive window") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(3, {{server_entity, Position{3.0f, 4.0f}}}, 2U, 3U),
        3));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(66, {{server_entity, Position{6.0f, 6.0f}}}, 2U, 66U),
        66));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 3);
    REQUIRE(stats.server_update_packets_missing == 1);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 0);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(1.0f / 4.0f));
}

TEST_CASE("replication client treats duplicate server update packet ids as duplicate without loss") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity first_entity{42};
    const ecs::Entity duplicate_packet_id_entity{43};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(2, {{duplicate_packet_id_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        2));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 2);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client fills packet loss gaps from reordered wrapped packet ids") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity first_entity{42};
    const ecs::Entity second_entity{43};
    const ecs::Entity reordered_entity{44};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 254U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(2, {{second_entity, Position{3.0f, 4.0f}}}, 2U, 1U),
        2));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(3, {{reordered_entity, Position{2.0f, 3.0f}}}, 2U, 255U),
        3));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 3);
    REQUIRE(stats.server_update_packets_missing == 0);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(0.0f));
}

TEST_CASE("replication client does not undo confirmed loss for packets older than the receive window") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    const ecs::Entity first_entity{42};
    const ecs::Entity second_entity{43};
    const ecs::Entity far_entity{44};
    const ecs::Entity late_entity{45};

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{first_entity, Position{1.0f, 2.0f}}}, 2U, 1U),
        1));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(3, {{second_entity, Position{3.0f, 4.0f}}}, 2U, 3U),
        3));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(66, {{far_entity, Position{6.0f, 6.0f}}}, 2U, 66U),
        66));
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(67, {{late_entity, Position{2.0f, 3.0f}}}, 2U, 2U),
        67));

    const kage::sync::ReplicationClientTimingStats stats = client.timing_stats();
    REQUIRE(stats.server_update_packets_received == 4);
    REQUIRE(stats.server_update_packets_missing == 1);
    REQUIRE(stats.server_update_packets_reordered_or_duplicate == 1);
    REQUIRE(stats.server_update_packet_loss == Catch::Approx(1.0f / 5.0f));
}

TEST_CASE("auto interpolation buffer moves one frame when dilation creates headroom") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8,
        true,
        1,
        4.0f,
        0.5f,
        0.90f,
        1.10f,
        0.10f});
    const ecs::Entity server_entity{42};

    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(6, {{server_entity, Position{3.0f, 4.0f}}}), 6));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames > 1);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames >= 1.0f);
    REQUIRE(client.current_interpolation_buffer_frames() == 2);
}

TEST_CASE("auto interpolation target uses downstream update lag as a floor") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    const ecs::Entity server_entity{42};

    REQUIRE(record_ping_sample(client, client_registry, 4));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 2);
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 2);

    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}),
        4));

    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 2);
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 3);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(0.90f));
    REQUIRE(client.current_interpolation_buffer_frames() == 2);
}

TEST_CASE("auto interpolation emits speedup when buffered data exceeds the desired depth") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(10, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 1);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames == Catch::Approx(6.0f));
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.10f));
}

TEST_CASE("auto interpolation buffer shrinks one frame at a time and returns to neutral dilation") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        4,
        8,
        true,
        1,
        0.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    const ecs::Entity server_entity{42};

    REQUIRE(record_ping_sample(client, client_registry, 0));
    REQUIRE(client.receive(client_registry, make_position_packet(10, {{server_entity, Position{1.0f, 2.0f}}}), 11));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 1);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames == Catch::Approx(3.0f));
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.10f));
    REQUIRE(client.current_interpolation_buffer_frames() == 3);

    REQUIRE(client.receive(client_registry, make_position_packet(11, {{server_entity, Position{2.0f, 3.0f}}}), 12));
    REQUIRE(client.current_interpolation_buffer_frames() == 2);

    REQUIRE(client.receive(client_registry, make_position_packet(12, {{server_entity, Position{3.0f, 4.0f}}}), 13));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(13, {{server_entity, Position{4.0f, 5.0f}}}), 13));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 1);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames == Catch::Approx(1.0f));
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);
}

TEST_CASE("auto interpolation buffer clamps and can be disabled") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient clamped(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        4,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    REQUIRE(record_ping_sample(clamped, client_registry, 20));
    REQUIRE(clamped.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 20));
    REQUIRE(clamped.timing_stats().target_interpolation_buffer_frames == 3);
    REQUIRE(clamped.timing_stats().time_dilation == Catch::Approx(0.90f));
    REQUIRE(clamped.current_interpolation_buffer_frames() == 1);

    ecs::Registry manual_registry;
    kage_sync_tests::define_position_archetype(manual_registry);
    kage::sync::configure_client(manual_registry, 1);
    kage::sync::ReplicationClient manual(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8,
        false,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    REQUIRE(record_ping_sample(manual, manual_registry, 8));
    REQUIRE(manual.receive(manual_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(manual.timing_stats().target_interpolation_buffer_frames == 4);
    REQUIRE(manual.timing_stats().current_interpolation_buffer_frames == 2);
    REQUIRE(manual.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(manual.current_interpolation_buffer_frames() == 2);
}

TEST_CASE("frame-aware receive failures do not update timing stats") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    const kage::sync::BitBuffer valid = make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}});
    REQUIRE(client.receive(client_registry, valid, 3));
    const kage::sync::ReplicationClientTimingStats baseline = client.timing_stats();

    kage::sync::BitBuffer truncated_header;
    truncated_header.push_bits(kage::sync::protocol::server_update_message, 8U);
    truncated_header.push_bits(2, 32U);
    REQUIRE_FALSE(client.receive(client_registry, truncated_header, 4));
    REQUIRE(client.timing_stats().sample_count == baseline.sample_count);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(baseline.time_dilation));

    REQUIRE_FALSE(client.receive(client_registry, valid, 3));
    REQUIRE(client.timing_stats().sample_count == baseline.sample_count);

    ecs::Registry invalid_interpolation_registry;
    const ecs::Entity position =
        kage::sync::register_sync_component<Position>(invalid_interpolation_registry, "Position");
    REQUIRE(kage::sync::define_archetype(
                invalid_interpolation_registry,
                "PositionActor",
                {{position, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}})
        == client_archetype);
    kage::sync::configure_client(invalid_interpolation_registry, 1);
    kage::sync::ReplicationClient invalid_interpolation_client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    REQUIRE_FALSE(invalid_interpolation_client.receive(
        invalid_interpolation_registry,
        make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}),
        3));
    REQUIRE(invalid_interpolation_client.timing_stats().sample_count == 0);
    REQUIRE(invalid_interpolation_client.timing_stats().time_dilation == Catch::Approx(1.0f));
}

TEST_CASE("snap mode records timing without emitting playback dilation") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::Snap,
        2,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));

    REQUIRE(client.timing_stats().sample_count == 1);
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.current_interpolation_buffer_frames() == 2);

    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
}

TEST_CASE("zero dilation gain keeps playback speed neutral while tracking desired buffer") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.0f});
    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);
}

TEST_CASE("manual buffer override resets auto timing target and dilation") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8,
        true,
        1,
        2.0f,
        1.0f,
        0.90f,
        1.10f,
        0.10f});
    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(0.90f));

    REQUIRE(client.set_interpolation_buffer_frames(3));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 3);
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 3);
    REQUIRE(client.timing_stats().current_interpolation_buffer_frames == 3);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.current_interpolation_buffer_frames() == 3);
}

TEST_CASE("buffered interpolation applies correct target frame after auto buffer depth changes") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        1,
        8,
        true,
        1,
        4.0f,
        0.5f,
        0.90f,
        1.10f,
        0.10f});

    REQUIRE(record_ping_sample(client, client_registry, 8));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.current_interpolation_buffer_frames() == 1);
    REQUIRE(client.receive(client_registry, make_position_packet(6, {{server_entity, Position{6.0f, 2.0f}}}), 6));
    REQUIRE(client.current_interpolation_buffer_frames() == 2);

    REQUIRE(client.apply_frame(client_registry, 7));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(client.apply_frame(client_registry, 8));
    REQUIRE(client_registry.get<Position>(local).x == 6.0f);

    REQUIRE(client.receive(client_registry, make_destroy_packet(7, server_entity), 7));
    REQUIRE(client.current_interpolation_buffer_frames() == 3);
    REQUIRE(client.apply_frame(client_registry, 9));
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.apply_frame(client_registry, 10));
    REQUIRE_FALSE(client_registry.alive(local));
}

TEST_CASE("normal receive records timing from the client-owned clock without moving an idle clock buffer") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);
    const ecs::Entity server_entity{42};

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.timing_stats().sample_count == 0);
    REQUIRE(client.current_interpolation_buffer_frames() == 2);
}

TEST_CASE("client connect handshake ACKs accepted id until first update") {
    ecs::Registry client_registry;
    kage_sync_tests::define_position_archetype(client_registry);

    kage::sync::ReplicationClientOptions options;
    options.connect_token = "token";
    kage::sync::ReplicationClient client(options);

    std::vector<kage::sync::BitBuffer> packets = client.drain_packets();
    REQUIRE(packets.size() == 1);
    REQUIRE(static_cast<std::uint8_t>(packets[0].read_bits(8U)) ==
            kage::sync::protocol::client_connect_request_message);
    std::string token;
    REQUIRE(kage::sync::protocol::read_string(packets[0], token));
    REQUIRE(token == "token");

    kage::sync::BitBuffer accepted;
    accepted.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    accepted.push_bool(true);
    accepted.push_unsigned_bits(7, 64U);
    REQUIRE(client.receive(client_registry, accepted));
    REQUIRE(client.client_id() == 7);
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Accepted);
    REQUIRE(client_registry.get<kage::sync::SyncSettings>().local_client == 7);

    packets = client.drain_packets();
    REQUIRE_FALSE(packets.empty());
    bool saw_connect_ack = false;
    for (kage::sync::BitBuffer packet : packets) {
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message == kage::sync::protocol::client_connect_ack_message) {
            saw_connect_ack = true;
            REQUIRE(packet.read_unsigned_bits(64U) == 7);
        }
    }
    REQUIRE(saw_connect_ack);

    const ecs::Entity server_entity{42};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Ready);

    REQUIRE(client.tick(client_registry, client.options().connect_resend_interval_seconds));
    packets = client.drain_packets();
    for (kage::sync::BitBuffer packet : packets) {
        REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) !=
                kage::sync::protocol::client_connect_ack_message);
    }
}

TEST_CASE("adaptive ping interval samples frequently until latency stabilizes") {
    ecs::Registry client_registry;
    kage_sync_tests::define_position_archetype(client_registry);

    kage::sync::ReplicationClientOptions options;
    options.ping_interval_seconds = 3.0;
    options.adaptive_ping_interval_seconds = 0.25;
    options.adaptive_ping_stable_samples = 3;
    options.adaptive_ping_stable_threshold_frames = 0.5f;
    kage::sync::ReplicationClient client(options);

    PingPacket ping;
    REQUIRE(drain_ping(client, client_registry, 0.0, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 8));

    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 8));

    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 8));

    REQUIRE_FALSE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE_FALSE(drain_ping(client, client_registry, 2.50, ping));
    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
}

TEST_CASE("adaptive ping interval re-enters fast mode after latency jumps") {
    ecs::Registry client_registry;
    kage_sync_tests::define_position_archetype(client_registry);

    kage::sync::ReplicationClientOptions options;
    options.ping_interval_seconds = 3.0;
    options.adaptive_ping_interval_seconds = 0.25;
    options.adaptive_ping_stable_samples = 2;
    options.adaptive_ping_stable_threshold_frames = 0.5f;
    options.adaptive_ping_jump_threshold_frames = 3.0f;
    kage::sync::ReplicationClient client(options);

    PingPacket ping;
    REQUIRE(drain_ping(client, client_registry, 0.0, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 8));
    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 8));

    REQUIRE_FALSE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(drain_ping(client, client_registry, 2.75, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 20));

    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(receive_pong(client, client_registry, ping, ping.send_frame + 20));
    REQUIRE(drain_ping(client, client_registry, 0.25, ping));
}

TEST_CASE("adaptive ping interval can be disabled") {
    ecs::Registry client_registry;
    kage_sync_tests::define_position_archetype(client_registry);

    kage::sync::ReplicationClientOptions options;
    options.ping_interval_seconds = 3.0;
    options.adaptive_ping_interval = false;
    options.adaptive_ping_interval_seconds = 0.25;
    kage::sync::ReplicationClient client(options);

    PingPacket ping;
    REQUIRE(drain_ping(client, client_registry, 0.0, ping));
    REQUIRE_FALSE(drain_ping(client, client_registry, 0.25, ping));
    REQUIRE(drain_ping(client, client_registry, 2.75, ping));
}

TEST_CASE("client does not send input packets before first server update") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::register_sync_component<NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions options;
    options.connect_token = "token";
    kage::sync::ReplicationClient client(options);

    kage::sync::BitBuffer accepted;
    accepted.push_bits(kage::sync::protocol::server_connect_response_message, 8U);
    accepted.push_bool(true);
    accepted.push_unsigned_bits(1, 64U);
    REQUIRE(client.receive(client_registry, accepted));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Accepted);

    REQUIRE(client.set_input(client_registry, NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    std::vector<kage::sync::BitBuffer> packets = client.drain_packets();
    REQUIRE(std::none_of(packets.begin(), packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    }));

    const ecs::Entity server_entity{42};
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.connection_state() == kage::sync::ReplicationClientConnectionState::Ready);

    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    packets = client.drain_packets();
    REQUIRE(std::any_of(packets.begin(), packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    }));
}

TEST_CASE("buffered interpolation does not recreate destroyed entities before the new target frame") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.apply_frame(client_registry, 3));
    const ecs::Entity first_local = client.local_entity(server_entity);
    REQUIRE(first_local);
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client.apply_frame(client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(first_local));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(4, {{server_entity, Position{5.0f, 6.0f}}})));

    REQUIRE(client.apply_frame(client_registry, 5));
    REQUIRE_FALSE(client.local_entity(server_entity));
    REQUIRE(client.apply_frame(client_registry, 6));
    const ecs::Entity second_local = client.local_entity(server_entity);
    REQUIRE(second_local);
    REQUIRE(second_local != first_local);
    REQUIRE(client_registry.get<Position>(second_local).x == 5.0f);
    REQUIRE(client_registry.get<Position>(second_local).y == 6.0f);
}

TEST_CASE("buffered interpolation applies late destroys for already sampled target frames") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    const ecs::Entity server_entity{42};

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.apply_frame(client_registry, 3));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE_FALSE(client.apply_frame(client_registry, 4));

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE_FALSE(client_registry.alive(local));
    REQUIRE_FALSE(client.local_entity(server_entity));

    const std::vector<kage::sync::BitBuffer> acks = client.drain_ack_packets();
    REQUIRE(acks.size() == 1);
    const std::vector<AckRecord> records = read_acks(acks[0]);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].packet_id == 2);
}

TEST_CASE("buffered interpolation keeps client entity network ids alive until destroy frame applies") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{
        1200,
        kage::sync::ReplicationClientMode::BufferedInterpolation,
        2,
        8});
    const ecs::Entity server_entity{42};
    const kage::sync::ClientEntityNetworkId client_entity_network_id =
        test_client_entity_network_id(1, test_network_id(server_entity), 1U);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.apply_frame(client_registry, 3));
    const ecs::Entity local = client.local_entity(client_entity_network_id);
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.is_alive_network_id(client_entity_network_id));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client_registry.alive(local));
    REQUIRE(client.local_entity(client_entity_network_id) == local);
    REQUIRE(client.is_alive_network_id(client_entity_network_id));

    REQUIRE(client.apply_frame(client_registry, 4));
    REQUIRE_FALSE(client_registry.alive(local));
    REQUIRE_FALSE(client.is_alive_network_id(client_entity_network_id));
    REQUIRE(client.drain_ack_packets().size() == 1);
}

TEST_CASE("replication client rejects stale client entity network ids after wire id reuse") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    const ecs::Entity server_entity{42};
    const std::uint32_t wire_id = test_network_id(server_entity);
    const kage::sync::ClientEntityNetworkId first_id = test_client_entity_network_id(1, wire_id, 1U);
    const kage::sync::ClientEntityNetworkId second_id = test_client_entity_network_id(1, wire_id, 2U);
    kage::sync::ReplicationClient client;

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{5.0f, 6.0f}}})));
    const ecs::Entity first_local = client.local_entity(first_id);
    REQUIRE(first_local);
    REQUIRE(client_registry.alive(first_local));
    REQUIRE(client.is_alive_network_id(first_id));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE_FALSE(client_registry.alive(first_local));
    REQUIRE_FALSE(client.is_alive_network_id(first_id));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(3, {{server_entity, Position{1.0f, 2.0f}}})));
    const ecs::Entity second_local = client.local_entity(second_id);
    REQUIRE(second_local);
    REQUIRE(second_local != first_local);
    REQUIRE(client.is_alive_network_id(second_id));
    REQUIRE_FALSE(client.local_entity(first_id));
}

TEST_CASE("replication client does not advance implicit versions twice for destroy resends") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    const ecs::Entity server_entity{42};
    const std::uint32_t wire_id = test_network_id(server_entity);
    const kage::sync::ClientEntityNetworkId second_id = test_client_entity_network_id(1, wire_id, 2U);
    const kage::sync::ClientEntityNetworkId third_id = test_client_entity_network_id(1, wire_id, 3U);
    kage::sync::ReplicationClient client;

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}})));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_destroy_packet(2, server_entity)));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE(client.receive(client_registry, make_destroy_packet(3, server_entity)));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(4, {{server_entity, Position{5.0f, 6.0f}}})));
    const ecs::Entity local = client.local_entity(second_id);
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client_registry.get<Position>(local).x == 5.0f);
    REQUIRE_FALSE(client.local_entity(third_id));

    const std::vector<kage::sync::BitBuffer> acks = client.drain_ack_packets();
    REQUIRE(acks.size() == 1);
    const std::vector<AckRecord> records = read_acks(acks[0]);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].packet_id == 4);
}
