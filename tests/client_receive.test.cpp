#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

namespace {

constexpr std::size_t client_destroy_tombstone_capacity = 65536;

ecs::BitBuffer make_destroy_packet_for_wire_ids(
    kage::sync::SyncFrame frame,
    std::uint32_t packet_id,
    std::uint32_t first_wire_id,
    std::uint16_t count) {
    ecs::BitBuffer packet;
    packet.push_bits(kage::sync::protocol::server_update_message, 8U);
    packet.push_bits(frame, 32U);
    packet.push_bits(packet_id, kage::sync::protocol::server_packet_id_bits);
    packet.push_bits(0, 32U);
    packet.push_bits(count, 16U);
    for (std::uint16_t offset = 0; offset < count; ++offset) {
        packet.push_bool(true);
        kage::sync::protocol::write_network_entity_id(packet, first_wire_id + offset);
    }
    return packet;
}

}  // namespace

TEST_CASE("replication client applies full updates and queues ACKs") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    REQUIRE(client.receive(client_registry, packets[0]));

    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.alive(local));
    REQUIRE(client_registry.contains<Position>(local));
    REQUIRE(client_registry.get<Position>(local).x == 1.0f);
    REQUIRE(client_registry.get<Position>(local).y == 2.0f);

    std::vector<ecs::BitBuffer> ack_packets = client.drain_ack_packets();
    REQUIRE(ack_packets.size() == 1);
    std::vector<AckRecord> acks = read_acks(ack_packets[0]);
    REQUIRE(acks.size() == 1);
    REQUIRE(acks[0].packet_id != 0);
    REQUIRE(server.process_packet(server_registry, 1, ack_packets[0]));
}

TEST_CASE("replication client exposes server timing estimates after updates") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    REQUIRE(client.estimated_server_frame() == 0.0);
    REQUIRE(client.continuous_prediction_frames_ahead() == 0.0);
    REQUIRE(client.continuous_interpolation_frames_behind() == 0.0);

    REQUIRE(client.receive(client_registry, packets[0], 10, 7));
    REQUIRE(client.estimated_server_frame() == Catch::Approx(1.0));
    REQUIRE(std::isfinite(client.continuous_prediction_frames_ahead()));
    REQUIRE(std::isfinite(client.continuous_interpolation_frames_behind()));
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

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
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
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }

    server_registry.write<NetworkedPosition>(server_entity) = NetworkedPosition{2.0f, 3.0f};
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));

    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
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

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
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
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }

    server_registry.write<NetworkedPosition>(server_entity) = NetworkedPosition{2.0f, 3.0f};
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.drain_ack_packets().size() == 1);

    server_registry.write<NetworkedPosition>(server_entity) = NetworkedPosition{3.0f, 4.0f};
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));

    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    REQUIRE(client_registry.get<NetworkedPosition>(local).x == 3.0f);
    REQUIRE(client_registry.get<NetworkedPosition>(local).y == 4.0f);
}

TEST_CASE("replication client reports missing prediction rollback traits for default predict mode") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);

    const ecs::Entity server_entity{42};
    try {
        (void)client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}));
        FAIL("expected missing prediction rollback trait error");
    } catch (const kage::sync::ClientError& error) {
        REQUIRE(error.status() == kage::sync::ClientStatus::MissingPredictionRollbackTrait);
    }
}

TEST_CASE("replication client reports missing prediction rollback traits for selector predict mode") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Snap;
    options.entity_mode_selector = [](const kage::sync::ReplicatedEntityUpdateView&) {
        return kage::sync::ReplicationClientMode::Predict;
    };
    kage::sync::ReplicationClient client(options);

    const ecs::Entity server_entity{42};
    try {
        (void)client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}));
        FAIL("expected missing prediction rollback trait error");
    } catch (const kage::sync::ClientError& error) {
        REQUIRE(error.status() == kage::sync::ClientStatus::MissingPredictionRollbackTrait);
    }
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

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.mtu_bytes = 1200;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
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
    server.tick(server_registry, server.options().fixed_dt_seconds);
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
    for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }

    server_registry.write<NetworkedPosition>(first) = NetworkedPosition{2.0f, 2.0f};
    server_registry.write<NetworkedPosition>(second) = NetworkedPosition{11.0f, 11.0f};
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);
    const UpdatePacket frame83 = read_update(packets.back());
    REQUIRE(frame83.records.size() == 2);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.drain_ack_packets().size() == 1);

    REQUIRE(server.acknowledge_entity(1, first, frame83.frame));

    server_registry.write<NetworkedPosition>(first) = NetworkedPosition{3.0f, 3.0f};
    server_registry.write<NetworkedPosition>(second) = NetworkedPosition{12.0f, 12.0f};
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
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

    ecs::BitBuffer packet;
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

    ecs::BitBuffer empty;
    REQUIRE_FALSE(client.receive(registry, empty));

    ecs::BitBuffer wrong_message;
    wrong_message.push_bits(kage::sync::protocol::client_ack_message, 8U);
    REQUIRE_FALSE(client.receive(registry, wrong_message));

    ecs::BitBuffer truncated_header;
    truncated_header.push_bits(kage::sync::protocol::server_update_message, 8U);
    truncated_header.push_bits(1, 32U);
    REQUIRE_FALSE(client.receive(registry, truncated_header));

    ecs::BitBuffer invalid_archetype;
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

TEST_CASE("replication client rejects malformed entity records without ACKing") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = kage_sync_tests::define_position_archetype(registry);
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    auto make_update_prefix = [](std::uint16_t record_count = 1) {
        ecs::BitBuffer packet;
        packet.push_bits(kage::sync::protocol::server_update_message, 8U);
        packet.push_bits(1, 32U);
        packet.push_bits(1, kage::sync::protocol::server_packet_id_bits);
        packet.push_bits(0, 32U);
        packet.push_bits(record_count, 16U);
        return packet;
    };

    {
        ecs::BitBuffer packet = make_update_prefix();
        packet.push_bool(false);
        kage::sync::protocol::write_network_entity_id(packet, 0);
        packet.push_bool(true);
        packet.push_bits(0, 32U);

        kage::sync::ReplicationClient client;
        REQUIRE_FALSE(client.receive(registry, packet));
        REQUIRE(client.pending_ack_count() == 0);
    }

    {
        ecs::BitBuffer packet = make_update_prefix();
        packet.push_bool(false);
        kage::sync::protocol::write_network_entity_id(packet, 1);
        packet.push_bool(true);
        packet.push_bits(0, 32U);
        packet.push_bool(false);
        packet.push_bits(1, 16U);
        packet.push_bits(2, kage::sync::protocol::bits_for_range(2U));

        kage::sync::ReplicationClient client;
        REQUIRE_FALSE(client.receive(registry, packet));
        REQUIRE(client.pending_ack_count() == 0);
    }

    {
        ecs::BitBuffer packet = make_update_prefix();
        packet.push_bool(false);
        kage::sync::protocol::write_network_entity_id(packet, 1);
        packet.push_bool(true);
        packet.push_bits(0, 32U);
        packet.push_bool(false);
        packet.push_bits(1, 16U);
        packet.push_bits(1, kage::sync::protocol::bits_for_range(2U));
        packet.push_bits(1, 8U);

        kage::sync::ReplicationClient client;
        REQUIRE_FALSE(client.receive(registry, packet));
        REQUIRE(client.pending_ack_count() == 0);
    }
}

TEST_CASE("replication client applies destroy records and ACKs tombstones") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
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

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    const ecs::Entity local = client.local_entity(first_allocated_client_entity_network_id(1));
    REQUIRE(local);
    for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }

    REQUIRE(server_registry.destroy(server_entity));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE_FALSE(client_registry.alive(local));

    std::vector<ecs::BitBuffer> destroy_acks = client.drain_ack_packets();
    REQUIRE(destroy_acks.size() == 1);
    std::vector<AckRecord> acks = read_acks(destroy_acks[0]);
    REQUIRE(acks.size() == 1);
    REQUIRE(acks[0].packet_id != 0);

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(server.process_packet(server_registry, 1, destroy_acks[0]));
    const std::size_t packets_after_ack = packets.size();
    server.tick(server_registry, server.options().fixed_dt_seconds);
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

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.mtu_bytes = 29;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    server.tick(server_registry, server.options().fixed_dt_seconds);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    for (const ecs::BitBuffer& packet : packets) {
        REQUIRE(client.receive(client_registry, packet));
    }

    std::vector<ecs::BitBuffer> acks = client.drain_ack_packets();
    REQUIRE(acks.size() == 1);
    for (const ecs::BitBuffer& ack : acks) {
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

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);

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

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);

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

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& packet) {
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

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 2);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    for (const ecs::BitBuffer& ack : client_one.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }
    REQUIRE(client_two.drain_ack_packets().size() == 1);

    server_registry.write<NetworkedPosition>(server_entity) = NetworkedPosition{2.0f, 3.0f};
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 2);

    const UpdatePacket client_one_update = read_update(packet_for(packets, 1));
    const UpdatePacket client_two_update = read_update(packet_for(packets, 2));
    REQUIRE(client_one_update.records.size() == 1);
    REQUIRE(client_two_update.records.size() == 1);
    REQUIRE_FALSE(client_one_update.records[0].full);
    REQUIRE(client_two_update.records[0].full);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));

    for (const ecs::BitBuffer& ack : client_one.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }
    for (const ecs::BitBuffer& ack : client_two.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 2, ack));
    }

    const ecs::Entity client_one_local = client_one.local_entity(first_allocated_client_entity_network_id(1));
    const ecs::Entity client_two_local = client_two.local_entity(first_allocated_client_entity_network_id(2));
    REQUIRE(client_one_registry.get<NetworkedPosition>(client_one_local).x == 2.0f);
    REQUIRE(client_one_registry.get<NetworkedPosition>(client_one_local).y == 3.0f);
    REQUIRE(client_two_registry.get<NetworkedPosition>(client_two_local).x == 2.0f);
    REQUIRE(client_two_registry.get<NetworkedPosition>(client_two_local).y == 3.0f);

    server_registry.write<NetworkedPosition>(server_entity) = NetworkedPosition{4.0f, 5.0f};
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 2);
    REQUIRE_FALSE(read_update(packet_for(packets, 1)).records[0].full);
    REQUIRE_FALSE(read_update(packet_for(packets, 2)).records[0].full);
}

TEST_CASE("replication clients ACK destroy records independently") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.bandwidth_limit_bytes_per_tick = 1024;
    server_options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& packet) {
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

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 2);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    const ecs::Entity client_one_local = client_one.local_entity(first_allocated_client_entity_network_id(1));
    const ecs::Entity client_two_local = client_two.local_entity(first_allocated_client_entity_network_id(2));
    REQUIRE(client_one_local);
    REQUIRE(client_two_local);
    for (const ecs::BitBuffer& ack : client_one.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }
    for (const ecs::BitBuffer& ack : client_two.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 2, ack));
    }

    REQUIRE(server_registry.destroy(server_entity));
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 2);
    REQUIRE(client_one.receive(client_one_registry, packet_for(packets, 1)));
    REQUIRE(client_two.receive(client_two_registry, packet_for(packets, 2)));
    REQUIRE_FALSE(client_one_registry.alive(client_one_local));
    REQUIRE_FALSE(client_two_registry.alive(client_two_local));

    std::vector<ecs::BitBuffer> client_one_acks = client_one.drain_ack_packets();
    std::vector<ecs::BitBuffer> client_two_acks = client_two.drain_ack_packets();
    REQUIRE(client_one_acks.size() == 1);
    REQUIRE(client_two_acks.size() == 1);
    REQUIRE(read_acks(client_one_acks[0]).size() == 1);
    REQUIRE(read_acks(client_two_acks[0]).size() == 1);
    REQUIRE(server.process_packet(server_registry, 1, client_one_acks[0]));

    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);
    REQUIRE(packets[0].first == 2);
    REQUIRE(server.process_packet(server_registry, 2, client_two_acks[0]));

    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
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

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& packet) {
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

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 2);
    for (const auto& sent : packets) {
        if (sent.first == 1) {
            REQUIRE(client_one.receive(client_one_registry, sent.second));
        } else {
            REQUIRE(client_two.receive(client_two_registry, sent.second));
        }
    }
    for (const ecs::BitBuffer& ack : client_one.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }
    for (const ecs::BitBuffer& ack : client_two.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 2, ack));
    }

    const ecs::Entity client_one_local = client_one.local_entity(first_allocated_client_entity_network_id(1));
    const ecs::Entity client_two_local = client_two.local_entity(first_allocated_client_entity_network_id(2));
    REQUIRE(client_one_registry.contains<Health>(client_one_local));
    REQUIRE_FALSE(client_two_registry.contains<Health>(client_two_local));

    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 2));
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
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

    std::vector<std::pair<kage::sync::ClientId, ecs::BitBuffer>> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId client, const ecs::BitBuffer& packet) {
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
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(owner_client.receive(owner_registry, packet_for(packets, 1)));
    REQUIRE(non_owner_client.receive(non_owner_registry, packet_for(packets, 2)));

    const ecs::Entity owner_local = owner_client.local_entity(first_allocated_client_entity_network_id(1));
    const ecs::Entity non_owner_local = non_owner_client.local_entity(first_allocated_client_entity_network_id(2));
    REQUIRE(owner_registry.has(owner_local, owner_visible));
    REQUIRE(owner_registry.has(owner_local, owner_secret));
    REQUIRE(non_owner_registry.has(non_owner_local, non_owner_visible));
    REQUIRE_FALSE(non_owner_registry.has(non_owner_local, non_owner_secret));

    for (const ecs::BitBuffer& ack : owner_client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 1, ack));
    }
    for (const ecs::BitBuffer& ack : non_owner_client.drain_ack_packets()) {
        REQUIRE(server.process_packet(server_registry, 2, ack));
    }

    REQUIRE(kage::sync::set_owner(server_registry, server_entity, 2));
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(owner_client.receive(owner_registry, packet_for(packets, 1)));
    REQUIRE(non_owner_client.receive(non_owner_registry, packet_for(packets, 2)));
    REQUIRE(owner_registry.has(owner_local, owner_visible));
    REQUIRE_FALSE(owner_registry.has(owner_local, owner_secret));
    REQUIRE(non_owner_registry.has(non_owner_local, non_owner_visible));
    REQUIRE(non_owner_registry.has(non_owner_local, non_owner_secret));
}

TEST_CASE("buffered interpolation rejects stale duplicate packets without ACKing") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<Position>(server_entity, Position{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
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

    server.tick(server_registry, server.options().fixed_dt_seconds);
    const ecs::BitBuffer full_packet = packets.back();
    REQUIRE(client.receive(client_registry, full_packet));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.receive(client_registry, full_packet));
    REQUIRE(client.pending_ack_count() == 0);

    server_registry.write<Position>(server_entity) = Position{3.0f, 4.0f};
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    const ecs::BitBuffer stale_full_packet = full_packet;
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.receive(client_registry, stale_full_packet));
    REQUIRE(client.pending_ack_count() == 0);

    REQUIRE(server_registry.destroy(server_entity));
    packets.clear();
    server.tick(server_registry, server.options().fixed_dt_seconds);
    const ecs::BitBuffer destroy_packet = packets.back();
    REQUIRE(client.receive(client_registry, destroy_packet));
    REQUIRE(client.drain_ack_packets().size() == 1);
    REQUIRE_FALSE(client.receive(client_registry, destroy_packet));
    REQUIRE(client.pending_ack_count() == 0);
}

TEST_CASE("replication client rejects stale full after destroy and accepts reused network id") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
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
        for (const ecs::BitBuffer& ack : client.drain_ack_packets()) {
            REQUIRE(server.process_packet(server_registry, 1, ack));
        }
    };

    const ecs::Entity first = server_registry.create();
    REQUIRE(server_registry.add<Position>(first, Position{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, first, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    const ecs::BitBuffer first_full = packets.back();
    const UpdatePacket first_update = read_update(first_full);
    REQUIRE(first_update.records.size() == 1);
    const std::uint32_t reused_network_id = first_update.records[0].network_id;
    REQUIRE(client.receive(client_registry, first_full));
    process_client_acks();

    packets.clear();
    REQUIRE(server_registry.destroy(first));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    const ecs::BitBuffer destroy = packets.back();
    REQUIRE(client.receive(client_registry, destroy));
    process_client_acks();
    REQUIRE_FALSE(client.receive(client_registry, first_full));
    REQUIRE(client.pending_ack_count() == 0);

    packets.clear();
    const ecs::Entity second = server_registry.create();
    REQUIRE(server_registry.add<Position>(second, Position{3.0f, 4.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, second, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    const UpdatePacket second_update = read_update(packets.back());
    REQUIRE(second_update.records.size() == 1);
    REQUIRE(second_update.records[0].network_id == reused_network_id);
    REQUIRE(client.receive(client_registry, packets.back()));
    REQUIRE(client.local_entity(test_client_entity_network_id(1, reused_network_id, 2U)));
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

    const std::vector<ecs::BitBuffer> acks = client.drain_ack_packets();
    REQUIRE(acks.size() == 1);
    const std::vector<AckRecord> records = read_acks(acks[0]);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].packet_id == 4);
}

TEST_CASE("replication client evicts destroy tombstones by deterministic age") {
    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype.value == 0);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClient client;
    constexpr std::uint32_t first_wire_id = 2U;
    constexpr std::uint16_t first_packet_destroy_count =
        static_cast<std::uint16_t>(client_destroy_tombstone_capacity - 1U);
    REQUIRE(client.receive(
        client_registry,
        make_destroy_packet_for_wire_ids(1, 1, first_wire_id, first_packet_destroy_count)));
    REQUIRE(client.receive(
        client_registry,
        make_destroy_packet_for_wire_ids(2, 2, first_wire_id + first_packet_destroy_count, 2U)));
    REQUIRE_FALSE(client.drain_ack_packets().empty());

    const ecs::Entity evicted_server_entity{first_wire_id - 1U};
    const kage::sync::ClientEntityNetworkId evicted_id =
        test_client_entity_network_id(1, first_wire_id, 1U);
    REQUIRE(client.receive(
        client_registry,
        make_position_packet(1, {{evicted_server_entity, Position{1.0f, 2.0f}}}, 2U, 3U)));
    const ecs::Entity evicted_local = client.local_entity(evicted_id);
    REQUIRE(evicted_local);
    REQUIRE(client_registry.alive(evicted_local));

    constexpr std::uint32_t retained_wire_id = first_wire_id + 1U;
    const ecs::Entity retained_server_entity{retained_wire_id - 1U};
    REQUIRE_FALSE(client.receive(
        client_registry,
        make_position_packet(1, {{retained_server_entity, Position{3.0f, 4.0f}}}, 2U, 4U)));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, retained_wire_id, 1U)));
}
