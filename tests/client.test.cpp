#include "test_components.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

using kage_sync_tests::NetworkedPosition;
using kage_sync_tests::Health;
using kage_sync_tests::Position;

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
                packet.read_bits(16U);
                const auto payload_bits = static_cast<std::uint32_t>(packet.read_bits(32U));
                for (std::uint32_t bit = 0; bit < payload_bits; ++bit) {
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
