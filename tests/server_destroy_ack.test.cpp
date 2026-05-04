#include "test_protocol.hpp"

#include "kage/sync/simulated_link.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

TEST_CASE("replication server interleaves pending destroys with bandwidth-limited updates") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity destroyed = registry.create();
    const ecs::Entity live = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(destroyed, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(live, NetworkedPosition{2.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.mtu_bytes = 21;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, destroyed, archetype));
    REQUIRE(start_sync(registry, live, archetype));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    payloads.clear();

    REQUIRE(registry.destroy(destroyed));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE_FALSE(update.entities[0].destroy);
    payloads.clear();

    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].destroy);
    REQUIRE(update.entities[0].network_id != 0);
}

TEST_CASE("replication server resends pending destroys until ACKed") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 30;
    options.mtu_bytes = 30;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));
    server.tick(registry);
    payloads.clear();

    REQUIRE(registry.destroy(entity));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket first_destroy = read_server_update(payloads.back());
    REQUIRE(first_destroy.entities.size() == 1);
    REQUIRE(first_destroy.entities[0].destroy);
    payloads.clear();

    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket resent_destroy = read_server_update(payloads.back());
    REQUIRE(resent_destroy.entities.size() == 1);
    REQUIRE(resent_destroy.entities[0].destroy);
    REQUIRE(resent_destroy.entities[0].network_id == first_destroy.entities[0].network_id);
    REQUIRE(resent_destroy.frame > first_destroy.frame);

    REQUIRE(server.process_packet(1, write_ack_packet(resent_destroy.packet_id)));
    payloads.clear();
    server.tick(registry);
    REQUIRE(payloads.empty());
}

TEST_CASE("replication server does not reuse client-local network ids before destroy ACK") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    auto spawn = [&](float x) {
        const ecs::Entity entity = registry.create();
        REQUIRE(registry.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{x, x}) != nullptr);
        REQUIRE(start_sync(registry, entity, archetype));
        return entity;
    };

    const ecs::Entity first = spawn(1.0f);
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket first_update =
        read_server_update(payloads.back(), 2U, sizeof(kage_sync_tests::Position) * 8U);
    REQUIRE(first_update.entities.size() == 1);
    const std::uint32_t first_network_id = first_update.entities[0].network_id;
    REQUIRE(first_network_id != 0);

    payloads.clear();
    REQUIRE(registry.destroy(first));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket destroy_update =
        read_server_update(payloads.back(), 2U, sizeof(kage_sync_tests::Position) * 8U);
    REQUIRE(destroy_update.entities.size() == 1);
    REQUIRE(destroy_update.entities[0].destroy);

    payloads.clear();
    spawn(2.0f);
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket second_update =
        read_server_update(payloads.back(), 2U, sizeof(kage_sync_tests::Position) * 8U);
    REQUIRE(second_update.entities.size() == 2);
    const auto upsert = std::find_if(
        second_update.entities.begin(),
        second_update.entities.end(),
        [](const EntityRecord& record) {
            return !record.destroy;
        });
    REQUIRE(upsert != second_update.entities.end());
    REQUIRE(upsert->network_id != first_network_id);
}

TEST_CASE("replication server reuses client-local network ids after each client's destroy ACK") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));

    auto spawn = [&](float x) {
        const ecs::Entity entity = registry.create();
        REQUIRE(registry.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{x, x}) != nullptr);
        REQUIRE(start_sync(registry, entity, archetype));
        return entity;
    };
    auto update_for = [&](kage::sync::ClientId client) {
        for (const auto& sent : payloads) {
            if (sent.first != client) {
                continue;
            }
            ServerUpdatePacket update = read_server_update(sent.second, 2U, sizeof(kage_sync_tests::Position) * 8U);
            const auto found = std::find_if(update.entities.begin(), update.entities.end(), [](const EntityRecord& record) {
                return !record.destroy;
            });
            if (found != update.entities.end()) {
                return update;
            }
        }
        return ServerUpdatePacket{};
    };
    auto destroy_for = [&](kage::sync::ClientId client, std::uint32_t network_id) {
        for (const auto& sent : payloads) {
            if (sent.first != client) {
                continue;
            }
            ServerUpdatePacket update = read_server_update(sent.second, 2U, sizeof(kage_sync_tests::Position) * 8U);
            const auto found = std::find_if(update.entities.begin(), update.entities.end(), [&](const EntityRecord& record) {
                return record.destroy && record.network_id == network_id;
            });
            if (found != update.entities.end()) {
                return update;
            }
        }
        return ServerUpdatePacket{};
    };
    auto packet_id = [](kage::sync::BitBuffer packet) {
        packet.read_bits(8U);
        packet.read_bits(32U);
        return static_cast<std::uint32_t>(packet.read_bits(kage::sync::protocol::server_packet_id_bits));
    };

    const ecs::Entity first = spawn(1.0f);
    server.tick(registry);
    ServerUpdatePacket first_update = update_for(1);
    REQUIRE(first_update.entities.size() == 1);
    const std::uint32_t reusable_network_id = first_update.entities[0].network_id;
    REQUIRE(reusable_network_id != 0);

    payloads.clear();
    REQUIRE(registry.destroy(first));
    server.tick(registry);
    ServerUpdatePacket client_one_destroy = destroy_for(1, reusable_network_id);
    ServerUpdatePacket client_two_destroy = destroy_for(2, reusable_network_id);
    REQUIRE(client_one_destroy.entities.size() == 1);
    REQUIRE(client_two_destroy.entities.size() == 1);
    REQUIRE(server.process_packet(1, write_ack_packet(client_one_destroy.packet_id)));

    payloads.clear();
    spawn(2.0f);
    server.tick(registry);
    ServerUpdatePacket second_update = update_for(1);
    REQUIRE(second_update.entities.size() == 1);
    REQUIRE(second_update.entities[0].network_id == reusable_network_id);
    ServerUpdatePacket client_two_second_update = update_for(2);
    REQUIRE(client_two_second_update.entities.size() == 2);
    const auto client_two_second = std::find_if(
        client_two_second_update.entities.begin(),
        client_two_second_update.entities.end(),
        [](const EntityRecord& record) {
            return !record.destroy;
        });
    REQUIRE(client_two_second != client_two_second_update.entities.end());
    REQUIRE(client_two_second->network_id != reusable_network_id);

    for (const auto& sent : payloads) {
        REQUIRE(server.process_packet(sent.first, write_ack_packet(packet_id(sent.second))));
    }
    payloads.clear();
    spawn(3.0f);
    server.tick(registry);
    ServerUpdatePacket third_update = update_for(2);
    const auto third_record = std::find_if(
        third_update.entities.begin(),
        third_update.entities.end(),
        [](const EntityRecord& record) {
            return !record.destroy;
        });
    REQUIRE(third_record != third_update.entities.end());
    const bool saw_reused_network_id = std::any_of(
        third_update.entities.begin(),
        third_update.entities.end(),
        [reusable_network_id](const EntityRecord& record) {
            return !record.destroy && record.network_id == reusable_network_id;
        });
    REQUIRE(saw_reused_network_id);
}

TEST_CASE("replication server reuses network ids immediately when no clients have pending destroys") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    const ecs::Entity first = registry.create();
    REQUIRE(registry.add<kage_sync_tests::Position>(first, kage_sync_tests::Position{1.0f, 1.0f}) != nullptr);
    REQUIRE(start_sync(registry, first, archetype));
    server.tick(registry);
    REQUIRE(registry.destroy(first));
    server.tick(registry);

    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<kage_sync_tests::Position>(second, kage_sync_tests::Position{2.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, second, archetype));
    REQUIRE(server.add_client(1));
    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].network_id == 1);
}

TEST_CASE("replication server accepts delayed entity ACKs for retained quantized frames") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);
    const kage::sync::SyncFrame first_frame = read_server_update(payloads.back()).frame;
    registry.write<NetworkedPosition>(entity) = NetworkedPosition{2.0f, 3.0f};
    server.tick(registry);
    REQUIRE(read_first_networked_payload(payloads.back()).delta == false);
    REQUIRE(server.acknowledge_entity(1, entity, first_frame));
    REQUIRE_FALSE(server.acknowledge_entity(1, entity, first_frame));

    const kage::sync::SyncFrame second_frame = read_server_update(payloads.back()).frame;
    REQUIRE(server.acknowledge_entity(1, entity, second_frame));
    REQUIRE_FALSE(server.acknowledge_entity(1, entity, second_frame));

    registry.write<NetworkedPosition>(entity) = NetworkedPosition{3.0f, 4.0f};
    server.tick(registry);
    REQUIRE(read_first_networked_payload(payloads.back()).delta);
}

TEST_CASE("replication server shares ACKed quantized frames across clients and frees them") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);
    REQUIRE(payloads.size() == 2);
    REQUIRE(server.retained_quantized_frame_count() == 1);
    REQUIRE(server.retained_quantized_frame_bytes() == sizeof(kage_sync_tests::QuantizedNetworkedPosition));

    REQUIRE(server.acknowledge_entity(1, entity, read_server_update(payloads[0].second).frame));
    REQUIRE(server.acknowledge_entity(2, entity, read_server_update(payloads[1].second).frame));
    REQUIRE(server.retained_quantized_frame_count() == 1);
    REQUIRE(server.retained_quantized_frame_bytes() == sizeof(kage_sync_tests::QuantizedNetworkedPosition));

    REQUIRE(server.remove_client(1));
    REQUIRE(server.retained_quantized_frame_count() == 1);
    REQUIRE(server.remove_client(2));
    REQUIRE(server.retained_quantized_frame_count() == 0);
    REQUIRE(server.retained_quantized_frame_bytes() == 0);
}

TEST_CASE("replication server keeps swapped clients addressable after removal") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);
    REQUIRE(payloads.size() == 2);

    std::uint32_t client_two_packet_id = 0;
    for (const auto& sent : payloads) {
        if (sent.first == 2) {
            client_two_packet_id = read_server_update(sent.second).packet_id;
        }
    }
    REQUIRE(client_two_packet_id != 0);

    REQUIRE(server.remove_client(1));
    REQUIRE(server.has_client(2));
    REQUIRE(server.process_packet(2, write_ack_packet(client_two_packet_id)));
}

TEST_CASE("replication server records bandwidth savings for ACKed delta updates") {
    ecs::Registry registry;
    const ecs::Entity probe_component =
        kage::sync::register_sync_component<BandwidthProbe>(registry, "BandwidthProbe");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "BandwidthActor",
        {{probe_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<BandwidthProbe>(entity, BandwidthProbe{100}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);
    REQUIRE(payloads.back().byte_size() == 23);
    REQUIRE(server.acknowledge_entity(1, entity, read_server_update(payloads.back()).frame));

    registry.write<BandwidthProbe>(entity) = BandwidthProbe{105};
    server.tick(registry);

    const std::size_t expected_delta_bits = kage::sync::protocol::server_update_header_bits +
        1U + kage::sync::protocol::network_entity_id_encoded_bits(1U) + 1U +
        (1U + kage::sync::protocol::baseline_frame_delta_bits) + 1U + 9U;
    REQUIRE(payloads.back().byte_size() == kage::sync::protocol::bytes_for_bits(expected_delta_bits));
}
