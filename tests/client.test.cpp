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
using kage_sync_tests::SmoothPosition;

namespace {

bool start_sync(ecs::Registry& registry, ecs::Entity entity, kage::sync::SyncArchetypeId archetype) {
    return registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{archetype}) != nullptr;
}

struct AckRecord {
    bool destroy = false;
    kage::sync::SyncFrame frame = 0;
    ecs::Entity entity;
};

struct UpdateRecord {
    bool destroy = false;
    bool full = false;
    kage::sync::SyncFrame baseline_frame = 0;
    ecs::Entity entity;
};

struct UpdatePacket {
    kage::sync::SyncFrame frame = 0;
    std::vector<UpdateRecord> records;
};

std::vector<AckRecord> read_acks(kage::sync::BitBuffer packet) {
    REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_ack_message);
    const auto count = static_cast<std::uint16_t>(packet.read_bits(16U));
    std::vector<AckRecord> records;
    records.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        AckRecord record;
        record.destroy = packet.read_bool();
        record.frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
        record.entity = ecs::Entity{packet.read_unsigned_bits(64U)};
        records.push_back(record);
    }
    return records;
}

UpdatePacket read_update(kage::sync::BitBuffer packet) {
    REQUIRE(static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::server_update_message);
    UpdatePacket update;
    update.frame = static_cast<kage::sync::SyncFrame>(packet.read_bits(32U));
    const auto count = static_cast<std::uint16_t>(packet.read_bits(16U));
    update.records.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        UpdateRecord record;
        record.destroy = packet.read_bool();
        record.entity = ecs::Entity{packet.read_unsigned_bits(64U)};
        if (!record.destroy) {
            record.full = packet.read_bool();
            if (record.full) {
                packet.read_bits(32U);
            } else {
                REQUIRE(kage::sync::protocol::read_baseline_frame(packet, update.frame, record.baseline_frame));
            }

            const auto component_count = static_cast<std::uint16_t>(packet.read_bits(16U));
            for (std::uint16_t component = 0; component < component_count; ++component) {
                const auto component_index = static_cast<std::uint16_t>(packet.read_bits(16U));
                const std::size_t payload_bits = component_index == 0 ? 17U : sizeof(Health) * 8U;
                for (std::size_t bit = 0; bit < payload_bits; ++bit) {
                    packet.read_bool();
                }
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

kage::sync::BitBuffer make_position_packet(
    kage::sync::SyncFrame frame,
    const std::vector<std::pair<ecs::Entity, Position>>& records) {
    kage::sync::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(static_cast<std::uint16_t>(records.size()), 16U);
    for (const auto& record : records) {
        packet.push_bool(false);
        packet.push_unsigned_bits(record.first.value, 64U);
        packet.push_bool(true);
        packet.push_bits(0, 32U);
        packet.push_bits(1, 16U);
        packet.push_bits(0, 16U);

        packet.push_bytes(reinterpret_cast<const char*>(&record.second), sizeof(Position));
    }
    return packet;
}

kage::sync::BitBuffer make_destroy_packet(kage::sync::SyncFrame frame, ecs::Entity server_entity) {
    kage::sync::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(1, 16U);
    packet.push_bool(true);
    packet.push_unsigned_bits(server_entity.value, 64U);
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
    REQUIRE_FALSE(acks[0].destroy);
    REQUIRE(acks[0].entity == server_entity);
    REQUIRE(server.process_packet(1, ack_packets[0]));
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
        if (record.entity == first) {
            saw_first_from_83 = record.baseline_frame == frame83.frame;
        } else if (record.entity == second) {
            saw_second_from_full = record.baseline_frame == full_update.frame;
        }
    }
    REQUIRE(saw_first_from_83);
    REQUIRE(saw_second_from_full);

    REQUIRE(client.receive(client_registry, packets.back()));
    const ecs::Entity first_local = client.local_entity(first);
    const ecs::Entity second_local = client.local_entity(second);
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
    packet.push_bits(1, 16U);
    packet.push_bool(false);
    packet.push_unsigned_bits(ecs::Entity{42}.value, 64U);
    packet.push_bool(false);
    packet.push_bits(1, 16U);
    packet.push_bits(0, 16U);
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
    invalid_archetype.push_bits(1, 16U);
    invalid_archetype.push_bool(false);
    invalid_archetype.push_unsigned_bits(ecs::Entity{42}.value, 64U);
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
    REQUIRE(acks[0].destroy);
    REQUIRE(acks[0].entity == server_entity);

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
    REQUIRE(acks.size() == 3);
    for (const kage::sync::BitBuffer& ack : acks) {
        REQUIRE(ack.byte_size() <= 16);
        REQUIRE(read_acks(ack).size() == 1);
    }
}

TEST_CASE("replication client retains ACKs that cannot fit the configured MTU") {
    kage::sync::ReplicationClient client(kage::sync::ReplicationClientOptions{15});
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
    REQUIRE(read_acks(client_one_acks[0])[0].destroy);
    REQUIRE(read_acks(client_two_acks[0])[0].destroy);
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
    kage::sync::configure_client(client_registry, 1);

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
    REQUIRE(client.receive(client_registry, packets.back()));

    REQUIRE(client.apply_frame(client_registry, 2));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<SmoothPosition>(local).x == 0.0f);

    kage::sync::DisplaySampleBuffer display;
    REQUIRE(client.sample_display_frame(client_registry, 2.5, display));
    REQUIRE(display.entities.size() == 1);
    REQUIRE(display.entities[0].server_entity == server_entity);
    REQUIRE(display.entities[0].local_entity == local);
    REQUIRE(display.entities[0].frame == 1);
    REQUIRE(display.entities[0].alpha == Catch::Approx(0.5f));

    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get(client_registry, sampled));
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

    kage::sync::DisplaySampleBuffer display;
    REQUIRE(client.sample_display_frame(client_registry, 2.75, display));
    REQUIRE(display.entities.size() == 1);

    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get(client_registry, sampled));
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

    kage::sync::DisplaySampleBuffer display;
    REQUIRE(client.sample_display_frame(client_registry, 2.0, display));
    REQUIRE(display.entities.empty());
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

    kage::sync::DisplaySampleBuffer display;
    REQUIRE(client.sample_display_frame(client_registry, 2.5, display));
    REQUIRE(display.entities.size() == 1);

    REQUIRE(client.sample_display_frame(client_registry, 3.0, display));
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
    const kage::sync::DisplaySampleBuffer& before = client.display_frame(client_registry);
    REQUIRE(before.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(before.entities[0].try_get(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(20.0f));

    REQUIRE(client.set_interpolation_buffer_frames(3));
    REQUIRE(client.tick(client_registry, 1.0 / 120.0));
    const kage::sync::DisplaySampleBuffer& after = client.display_frame(client_registry);
    REQUIRE(after.entities.size() == 1);
    REQUIRE(after.entities[0].try_get(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(20.0f));
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
    const kage::sync::DisplaySampleBuffer& before = client.display_frame(client_registry);
    REQUIRE(before.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(before.entities[0].try_get(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));

    REQUIRE(client.tick(client_registry, 1.0));
    const kage::sync::DisplaySampleBuffer& after = client.display_frame(client_registry);
    REQUIRE(after.entities.size() == 1);
    REQUIRE(after.entities[0].try_get(client_registry, sampled));
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
        return update.server_entity.value == 42
            ? kage::sync::ReplicationClientMode::BufferedInterpolation
            : kage::sync::ReplicationClientMode::Snap;
    };
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{ecs::Entity{42}, Position{10.0f, 0.0f}}})));
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{ecs::Entity{43}, Position{20.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 2.0));

    const kage::sync::DisplaySampleBuffer& display = client.display_frame(client_registry);
    REQUIRE(display.entities.size() == 2);
    int found = 0;
    for (const kage::sync::DisplayEntitySample& entity : display.entities) {
        SmoothPosition sampled;
        REQUIRE(entity.try_get(client_registry, sampled));
        if (entity.server_entity.value == 42) {
            REQUIRE(sampled.x == Catch::Approx(10.0f));
            ++found;
        }
        if (entity.server_entity.value == 43) {
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
    REQUIRE(client.display_frame(client_registry).entities.size() == 1);

    REQUIRE(client.set_default_entity_mode(kage::sync::ReplicationClientMode::BufferedInterpolation));
    REQUIRE(client.set_entity_mode(client_registry, existing, kage::sync::ReplicationClientMode::BufferedInterpolation));
    REQUIRE(client.receive(client_registry, make_position_packet(2, {{incoming, Position{20.0f, 0.0f}}})));
    REQUIRE(client.tick(client_registry, 1.0));

    const kage::sync::DisplaySampleBuffer& display = client.display_frame(client_registry);
    REQUIRE(display.entities.size() == 2);

    int found = 0;
    for (const kage::sync::DisplayEntitySample& entity : display.entities) {
        SmoothPosition sampled;
        REQUIRE(entity.try_get(client_registry, sampled));
        if (entity.server_entity == existing) {
            REQUIRE(sampled.x == Catch::Approx(10.0f));
            ++found;
        }
        if (entity.server_entity == incoming) {
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
    const kage::sync::DisplaySampleBuffer& first = client.display_frame(client_registry);
    REQUIRE(first.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(first.entities[0].try_get(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(0.0f));

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{10.0f, 0.0f}}})));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<SmoothPosition>(local).x == Catch::Approx(10.0f));

    REQUIRE(client.tick(client_registry, 0.25));
    const kage::sync::DisplaySampleBuffer& blended = client.display_frame(client_registry);
    REQUIRE(blended.entities.size() == 1);
    REQUIRE(blended.entities[0].try_get(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(2.5f));
    REQUIRE(client_registry.get<SmoothPosition>(local).x == Catch::Approx(10.0f));

    const kage::sync::DisplaySampleBuffer& repeated = client.display_frame(client_registry);
    REQUIRE(repeated.entities.size() == 1);
    REQUIRE(repeated.entities[0].try_get(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(2.5f));
}

TEST_CASE("snap display error blending clears after the accumulated tick dt consumes the error") {
    ecs::Registry client_registry;
    const ecs::Entity smooth =
        kage::sync::register_sync_component<SmoothPosition>(client_registry, "SmoothPosition");
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

    const kage::sync::DisplaySampleBuffer& display = client.display_frame(client_registry);
    REQUIRE(display.entities.size() == 1);
    SmoothPosition sampled;
    REQUIRE(display.entities[0].try_get(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));
}

TEST_CASE("snap display components without error traits keep snapped values") {
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

    const kage::sync::DisplaySampleBuffer& display = client.display_frame(client_registry);
    REQUIRE(display.entities.size() == 1);
    Position sampled;
    REQUIRE(display.entities[0].try_get(client_registry, sampled));
    REQUIRE(sampled.x == Catch::Approx(10.0f));
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
    REQUIRE(client.options().interpolation_buffer_frames == 3);

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

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().sample_count == 1);
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(client.timing_stats().jitter_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames == Catch::Approx(0.0f));
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(0.90f));
    REQUIRE(client.options().interpolation_buffer_frames == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(2, {{server_entity, Position{3.0f, 4.0f}}}), 4));
    REQUIRE(client.timing_stats().sample_count == 2);
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(2.0f));
    REQUIRE(client.timing_stats().jitter_frames == Catch::Approx(2.0f));
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 6);
    REQUIRE(client.options().interpolation_buffer_frames == 1);
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

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.options().interpolation_buffer_frames == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(6, {{server_entity, Position{3.0f, 4.0f}}}), 6));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames > 1);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames >= 1.0f);
    REQUIRE(client.options().interpolation_buffer_frames == 2);
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

    REQUIRE(client.receive(client_registry, make_position_packet(10, {{server_entity, Position{1.0f, 2.0f}}}), 11));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 1);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames == Catch::Approx(3.0f));
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.10f));
    REQUIRE(client.options().interpolation_buffer_frames == 3);

    REQUIRE(client.receive(client_registry, make_position_packet(11, {{server_entity, Position{2.0f, 3.0f}}}), 12));
    REQUIRE(client.options().interpolation_buffer_frames == 2);

    REQUIRE(client.receive(client_registry, make_position_packet(12, {{server_entity, Position{3.0f, 4.0f}}}), 13));
    REQUIRE(client.options().interpolation_buffer_frames == 1);

    REQUIRE(client.receive(client_registry, make_position_packet(13, {{server_entity, Position{4.0f, 5.0f}}}), 13));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 1);
    REQUIRE(client.timing_stats().measured_interpolation_buffer_frames == Catch::Approx(1.0f));
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.options().interpolation_buffer_frames == 1);
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
    REQUIRE(clamped.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 20));
    REQUIRE(clamped.timing_stats().target_interpolation_buffer_frames == 3);
    REQUIRE(clamped.timing_stats().time_dilation == Catch::Approx(0.90f));
    REQUIRE(clamped.options().interpolation_buffer_frames == 1);

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
    REQUIRE(manual.receive(manual_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(manual.timing_stats().target_interpolation_buffer_frames == 4);
    REQUIRE(manual.timing_stats().current_interpolation_buffer_frames == 2);
    REQUIRE(manual.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(manual.options().interpolation_buffer_frames == 2);
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
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));

    REQUIRE(client.timing_stats().sample_count == 1);
    REQUIRE(client.timing_stats().latency_frames == Catch::Approx(4.0f));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.options().interpolation_buffer_frames == 2);

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
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.options().interpolation_buffer_frames == 1);
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
    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 4);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(0.90f));

    REQUIRE(client.set_interpolation_buffer_frames(3));
    REQUIRE(client.timing_stats().desired_interpolation_buffer_frames == 3);
    REQUIRE(client.timing_stats().target_interpolation_buffer_frames == 3);
    REQUIRE(client.timing_stats().current_interpolation_buffer_frames == 3);
    REQUIRE(client.timing_stats().time_dilation == Catch::Approx(1.0f));
    REQUIRE(client.options().interpolation_buffer_frames == 3);
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

    REQUIRE(client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}), 5));
    REQUIRE(client.options().interpolation_buffer_frames == 1);
    REQUIRE(client.receive(client_registry, make_position_packet(6, {{server_entity, Position{6.0f, 2.0f}}}), 6));
    REQUIRE(client.options().interpolation_buffer_frames == 2);

    REQUIRE(client.apply_frame(client_registry, 7));
    const ecs::Entity local = client.local_entity(server_entity);
    REQUIRE(local);
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(client.apply_frame(client_registry, 8));
    REQUIRE(client_registry.get<Position>(local).x == 6.0f);

    REQUIRE(client.receive(client_registry, make_destroy_packet(7, server_entity), 7));
    REQUIRE(client.options().interpolation_buffer_frames == 3);
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
    REQUIRE(client.timing_stats().sample_count == 1);
    REQUIRE(client.options().interpolation_buffer_frames == 2);
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
    REQUIRE(records[0].entity == server_entity);
    REQUIRE(records[0].frame == 2);
    REQUIRE(records[0].destroy);
}
