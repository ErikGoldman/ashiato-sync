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

TEST_CASE("replication prioritizer reuses cached decisions until the configured interval expires") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{1.0f, 1.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    bool allow_replication = false;
    std::size_t prioritizer_calls = 0;
    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.prioritizer_interval_frames = 3;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>& objects,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        ++prioritizer_calls;
        REQUIRE(objects.size() == 1);
        decisions[0].replicate = allow_replication;
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));

    server.tick(registry);
    REQUIRE(prioritizer_calls == 1);
    REQUIRE(payloads.empty());

    allow_replication = true;
    server.tick(registry);
    server.tick(registry);
    REQUIRE(prioritizer_calls == 1);
    REQUIRE(payloads.empty());

    server.tick(registry);
    REQUIRE(prioritizer_calls == 2);
    REQUIRE(payloads.size() == 1);
    REQUIRE(read_server_update(payloads.back(), 2U, sizeof(kage_sync_tests::Position) * 8U).entities.size() == 1);
}

TEST_CASE("replication prioritizer filtering destroys an already visible entity for that client") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "FilterDestroyActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    bool allow_replication = true;
    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>&,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        decisions[0].replicate = allow_replication;
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE_FALSE(update.entities[0].destroy);
    const std::uint32_t visible_network_id = update.entities[0].network_id;
    REQUIRE(server.acknowledge_entity(1, entity, update.frame));

    allow_replication = false;
    payloads.clear();
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].destroy);
    REQUIRE(update.entities[0].network_id == visible_network_id);

    REQUIRE(server.process_packet(1, write_ack_packet(update.packet_id)));
    payloads.clear();
    server.tick(registry);
    REQUIRE(payloads.empty());

    allow_replication = true;
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE_FALSE(update.entities[0].destroy);
    REQUIRE(update.entities[0].full);
}

TEST_CASE("replication prioritizer priority affects serialized send order") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity low = registry.create();
    const ecs::Entity middle = registry.create();
    const ecs::Entity high = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(low, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(middle, NetworkedPosition{2.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(high, NetworkedPosition{3.0f, 3.0f}) != nullptr);
    REQUIRE(start_sync(registry, low, archetype));
    REQUIRE(start_sync(registry, middle, archetype));
    REQUIRE(start_sync(registry, high, archetype));

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.mtu_bytes = 21;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>& objects,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        for (std::size_t index = 0; index < objects.size(); ++index) {
            if (objects[index].entity == high) {
                decisions[index].priority = 100U;
            } else if (objects[index].entity == middle) {
                decisions[index].priority = 50U;
            }
        }
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    server.tick(registry);

    REQUIRE(payloads.size() == 3);
    const NetworkedPayload first = read_first_networked_payload(payloads[0]);
    REQUIRE_FALSE(first.delta);
    REQUIRE(first.x == 30);
    REQUIRE(first.y == 30);
}

TEST_CASE("entity references boost visible low priority targets on the next tick") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ecs::Entity reference_component =
        kage::sync::register_sync_component<TargetReference>(registry, "TargetReference");
    const kage::sync::SyncArchetypeId position_archetype = kage::sync::define_archetype(
        registry,
        "ReferencedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const kage::sync::SyncArchetypeId reference_archetype = kage::sync::define_archetype(
        registry,
        "ReferenceActor",
        {{reference_component, kage::sync::ReplicationAudience::All}});

    const ecs::Entity source = registry.create();
    const ecs::Entity target = registry.create();
    const ecs::Entity medium = registry.create();
    REQUIRE(registry.add<TargetReference>(
                source,
                TargetReference{kage::sync::EntityReference{target}}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(target, NetworkedPosition{2.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(medium, NetworkedPosition{3.0f, 3.0f}) != nullptr);
    REQUIRE(start_sync(registry, source, reference_archetype));
    REQUIRE(start_sync(registry, target, position_archetype));
    REQUIRE(start_sync(registry, medium, position_archetype));

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.mtu_bytes = 21;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>& objects,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        for (std::size_t index = 0; index < objects.size(); ++index) {
            if (objects[index].entity == source) {
                decisions[index].priority = 100U;
            } else if (objects[index].entity == medium) {
                decisions[index].priority = 50U;
            }
        }
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket update = read_server_update(
        payloads.back(),
        2U,
        1U + kage::sync::protocol::network_entity_id_encoded_bits(2U));
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].archetype == reference_archetype);
    REQUIRE(update.entities[0].components.size() == 1);
    REQUIRE(update.entities[0].components[0].payload.read_bool());
    std::uint32_t referenced_network_id = 0;
    REQUIRE(kage::sync::protocol::read_network_entity_id(
        update.entities[0].components[0].payload,
        referenced_network_id));
    REQUIRE(referenced_network_id != 0U);

    payloads.clear();
    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    update = read_server_update(payloads.back(), 2U);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].archetype == position_archetype);
    REQUIRE(update.entities[0].network_id == referenced_network_id);
}

TEST_CASE("entity reference priority boost respects prioritizer visibility filtering") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ecs::Entity reference_component =
        kage::sync::register_sync_component<TargetReference>(registry, "TargetReference");
    const kage::sync::SyncArchetypeId position_archetype = kage::sync::define_archetype(
        registry,
        "HiddenReferencedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const kage::sync::SyncArchetypeId reference_archetype = kage::sync::define_archetype(
        registry,
        "HiddenReferenceActor",
        {{reference_component, kage::sync::ReplicationAudience::All}});

    const ecs::Entity source = registry.create();
    const ecs::Entity target = registry.create();
    const ecs::Entity medium = registry.create();
    REQUIRE(registry.add<TargetReference>(
                source,
                TargetReference{kage::sync::EntityReference{target}}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(target, NetworkedPosition{2.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(medium, NetworkedPosition{3.0f, 3.0f}) != nullptr);
    REQUIRE(start_sync(registry, source, reference_archetype));
    REQUIRE(start_sync(registry, target, position_archetype));
    REQUIRE(start_sync(registry, medium, position_archetype));

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 21;
    options.mtu_bytes = 21;
    options.prioritizer_interval_frames = 1;
    std::uint64_t source_priority = 100U;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>& objects,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        for (std::size_t index = 0; index < objects.size(); ++index) {
            if (objects[index].entity == source) {
                decisions[index].priority = source_priority;
            } else if (objects[index].entity == target) {
                decisions[index].replicate = false;
            } else if (objects[index].entity == medium) {
                decisions[index].priority = 50U;
            }
        }
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket update = read_server_update(payloads.back(), 2U, 1U);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].archetype == reference_archetype);
    REQUIRE(update.entities[0].components.size() == 1);
    REQUIRE_FALSE(update.entities[0].components[0].payload.read_bool());

    source_priority = 0U;
    payloads.clear();
    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    update = read_server_update(payloads.back(), 2U);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].archetype == position_archetype);
    REQUIRE(update.entities[0].network_id != 0U);
    REQUIRE(update.entities[0].components.size() == 1);
    const NetworkedPayload payload = read_networked_payload(update.entities[0].components[0].payload);
    REQUIRE(payload.x == 30);
    REQUIRE(payload.y == 30);
}

TEST_CASE("replication prioritizer component masks apply to delta updates") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ecs::Entity health_component = kage::sync::register_sync_component<Health>(registry, "Health");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "MaskedActor",
        {
            {position_component, kage::sync::ReplicationAudience::All},
            {health_component, kage::sync::ReplicationAudience::All},
        });
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add<Health>(entity, Health{25}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::uint64_t component_mask = std::numeric_limits<std::uint64_t>::max();
    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>&,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        for (kage::sync::ReplicationPriorityDecision& decision : decisions) {
            decision.component_mask = component_mask;
        }
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    ServerUpdatePacket update = read_server_update(payloads.back(), 3U);
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].full);
    REQUIRE(update.entities[0].components.size() == 2);
    REQUIRE(server.acknowledge_entity(1, entity, update.frame));

    registry.write<NetworkedPosition>(entity) = NetworkedPosition{4.0f, 6.0f};
    registry.write<Health>(entity) = Health{75};
    component_mask = std::uint64_t{1} << 0U;
    payloads.clear();
    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    update = read_server_update(payloads.back(), 3U);
    REQUIRE(update.entities.size() == 1);
    REQUIRE_FALSE(update.entities[0].full);
    REQUIRE(update.entities[0].components.size() == 1);
    REQUIRE(update.entities[0].components[0].component_index == 1);
    const NetworkedPayload fields = read_networked_payload(update.entities[0].components[0].payload);
    REQUIRE(fields.delta);
    REQUIRE(fields.x == 30);
    REQUIRE(fields.y == 40);
}

TEST_CASE("replication prioritizer can emit an entity record with an all-zero component mask") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "ZeroMaskActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId,
                              const std::vector<kage::sync::ReplicationPriorityObject>&,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        decisions[0].component_mask = 0;
    };
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    const ServerUpdatePacket update = read_server_update(payloads.back());
    REQUIRE(update.entities.size() == 1);
    REQUIRE(update.entities[0].full);
    REQUIRE(update.entities[0].components.empty());
}

TEST_CASE("replication prioritizer rejects callbacks that return the wrong decision count") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<kage_sync_tests::Position>(entity, kage_sync_tests::Position{1.0f, 1.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    kage::sync::ReplicationServerOptions options;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [](kage::sync::ClientId,
                             const std::vector<kage::sync::ReplicationPriorityObject>&,
                             std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        decisions.clear();
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE_THROWS_AS(server.tick(registry, kage::sync::ReplicationServer::ReplicateFn{}), std::invalid_argument);
}

TEST_CASE("replication prioritizer applies independent decisions per client") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PerClientActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{2.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId client,
                              const std::vector<kage::sync::ReplicationPriorityObject>& objects,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        for (std::size_t index = 0; index < objects.size(); ++index) {
            decisions[index].replicate = client == 1 ? objects[index].entity == first : objects[index].entity == second;
        }
    };
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    server.tick(registry);

    REQUIRE(payloads.size() == 2);
    for (const auto& sent : payloads) {
        const NetworkedPayload fields = read_first_networked_payload(sent.second);
        REQUIRE_FALSE(fields.delta);
        if (sent.first == 1) {
            REQUIRE(fields.x == 10);
        } else {
            REQUIRE(sent.first == 2);
            REQUIRE(fields.x == 20);
        }
    }
}

TEST_CASE("replication prioritizer decisions are honored by the parallel serialized path") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "ParallelPrioritizedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{2.0f, 2.0f}) != nullptr);
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    std::vector<std::pair<kage::sync::ClientId, kage::sync::BitBuffer>> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.serialized_worker_threads = 2;
    options.prioritizer_interval_frames = 1;
    options.prioritizer = [&](kage::sync::ClientId client,
                              const std::vector<kage::sync::ReplicationPriorityObject>& objects,
                              std::vector<kage::sync::ReplicationPriorityDecision>& decisions) {
        for (std::size_t index = 0; index < objects.size(); ++index) {
            decisions[index].replicate = client == 1 ? objects[index].entity == first : objects[index].entity == second;
        }
    };
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        payloads.push_back({client, payload});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    server.tick(registry);

    REQUIRE(payloads.size() == 2);
    for (const auto& sent : payloads) {
        const NetworkedPayload fields = read_first_networked_payload(sent.second);
        REQUIRE_FALSE(fields.delta);
        if (sent.first == 1) {
            REQUIRE(fields.x == 10);
        } else {
            REQUIRE(sent.first == 2);
            REQUIRE(fields.x == 20);
        }
    }
}
