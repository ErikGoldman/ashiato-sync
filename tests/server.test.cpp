#include "test_components.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

using kage_sync_tests::Health;
using kage_sync_tests::NetworkedPayload;
using kage_sync_tests::NetworkedPosition;
using kage_sync_tests::define_position_archetype;
using kage_sync_tests::read_networked_payload;

namespace {

bool start_sync(ecs::Registry& registry, ecs::Entity entity, kage::sync::SyncArchetypeId archetype) {
    return registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{archetype}) != nullptr;
}

}  // namespace

TEST_CASE("replication server tracks clients and replicated component changes") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();

    kage::sync::ReplicationServer server;

    REQUIRE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(kage::sync::invalid_client_id));
    REQUIRE(server.has_client(7));
    REQUIRE(server.client_count() == 1);

    REQUIRE_FALSE(start_sync(registry, ecs::Entity{}, archetype));
    REQUIRE(start_sync(registry, entity, kage::sync::SyncArchetypeId{999}));
    server.refresh_replicated(registry);
    REQUIRE_FALSE(server.is_replicated(entity));
    REQUIRE(start_sync(registry, entity, archetype));
    server.refresh_replicated(registry);
    REQUIRE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 1);
    REQUIRE(registry.contains<kage::sync::Replicated>(entity));

    REQUIRE(registry.remove<kage::sync::Replicated>(entity));
    server.refresh_replicated(registry);
    REQUIRE_FALSE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 0);
    REQUIRE_FALSE(registry.contains<kage::sync::Replicated>(entity));
    REQUIRE_FALSE(registry.remove<kage::sync::Replicated>(entity));

    REQUIRE(server.remove_client(7));
    REQUIRE_FALSE(server.has_client(7));
}

TEST_CASE("replication server respects per-client bandwidth limits") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    std::vector<ecs::Entity> entities;
    for (int i = 0; i < 3; ++i) {
        entities.push_back(registry.create());
    }

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{256, 128});
    REQUIRE(server.add_client(1));
    for (const ecs::Entity entity : entities) {
        REQUIRE(start_sync(registry, entity, archetype));
    }

    std::vector<ecs::Entity> sent;
    server.tick(registry, [&](kage::sync::ClientId client, ecs::Entity entity) {
        REQUIRE(client == 1);
        sent.push_back(entity);
    });

    REQUIRE(sent.size() == 2);
    for (const ecs::Entity entity : sent) {
        REQUIRE(server.priority(1, entity) == 0);
    }

    auto unsent = std::find_if(entities.begin(), entities.end(), [&](ecs::Entity entity) {
        return std::find(sent.begin(), sent.end(), entity) == sent.end();
    });
    REQUIRE(unsent != entities.end());
    REQUIRE(server.priority(1, *unsent) == 1);
}

TEST_CASE("replication server rotates budget-limited sends by priority") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    std::vector<ecs::Entity> entities;
    for (int i = 0; i < 3; ++i) {
        entities.push_back(registry.create());
    }

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{128, 128});
    REQUIRE(server.add_client(1));
    for (const ecs::Entity entity : entities) {
        REQUIRE(start_sync(registry, entity, archetype));
    }

    std::vector<ecs::Entity> sent;
    for (int i = 0; i < 3; ++i) {
        server.tick(registry, [&](kage::sync::ClientId, ecs::Entity entity) {
            sent.push_back(entity);
        });
    }

    REQUIRE(sent.size() == 3);
    for (const ecs::Entity entity : entities) {
        REQUIRE(std::find(sent.begin(), sent.end(), entity) != sent.end());
        REQUIRE(server.priority(1, entity) <= 2);
    }
}

TEST_CASE("replication server keeps per-client priorities independent") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{128, 128});
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));
    REQUIRE(server.add_client(2));

    std::vector<std::pair<kage::sync::ClientId, ecs::Entity>> sent;
    server.tick(registry, [&](kage::sync::ClientId client, ecs::Entity entity) {
        sent.push_back({client, entity});
    });

    REQUIRE(sent.size() == 2);
    REQUIRE(sent[0].first == 1);
    REQUIRE(sent[1].first == 2);
    REQUIRE(server.priority(1, sent[0].second) == 0);
    REQUIRE(server.priority(2, sent[1].second) == 0);

    const ecs::Entity client_one_unsent = sent[0].second == first ? second : first;
    const ecs::Entity client_two_unsent = sent[1].second == first ? second : first;
    REQUIRE(server.priority(1, client_one_unsent) == 1);
    REQUIRE(server.priority(2, client_two_unsent) == 1);
}

TEST_CASE("replication server skips destroyed and externally unmarked entities") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity destroyed = registry.create();
    const ecs::Entity unmarked = registry.create();
    const ecs::Entity live = registry.create();

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{512, 128});
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, destroyed, archetype));
    REQUIRE(start_sync(registry, unmarked, archetype));
    REQUIRE(start_sync(registry, live, archetype));

    REQUIRE(registry.destroy(destroyed));
    REQUIRE(registry.remove<kage::sync::Replicated>(unmarked));

    std::vector<ecs::Entity> sent;
    server.tick(registry, [&](kage::sync::ClientId, ecs::Entity entity) {
        sent.push_back(entity);
    });

    REQUIRE(sent == std::vector<ecs::Entity>{live});
    REQUIRE_FALSE(server.is_replicated(destroyed));
    REQUIRE_FALSE(server.is_replicated(unmarked));
    REQUIRE(server.is_replicated(live));
    REQUIRE(server.replicated_count() == 1);
}

TEST_CASE("replication server sends nothing when bandwidth cannot cover the fixed cost") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();

    kage::sync::ReplicationServer server(kage::sync::ReplicationServerOptions{127, 128});
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    int sends = 0;
    server.tick(registry, [&](kage::sync::ClientId, ecs::Entity) {
        ++sends;
    });

    REQUIRE(sends == 0);
    REQUIRE(server.priority(1, entity) == 1);
}

TEST_CASE("replication server serializes full and delta component payloads through transport") {
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
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        REQUIRE(client == 1);
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);
    REQUIRE(payloads.size() == 1);
    NetworkedPayload fields = read_networked_payload(payloads[0]);
    REQUIRE_FALSE(fields.delta);
    REQUIRE(fields.x == 10);
    REQUIRE(fields.y == 20);

    registry.write<NetworkedPosition>(entity) = NetworkedPosition{2.0f, 3.0f};
    server.tick(registry);

    REQUIRE(payloads.size() == 2);
    fields = read_networked_payload(payloads[1]);
    REQUIRE(fields.delta);
    REQUIRE(fields.x == 10);
    REQUIRE(fields.y == 10);
}

TEST_CASE("replication server applies bandwidth limits to actual serialized byte counts") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity first = registry.create();
    const ecs::Entity second = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(first, NetworkedPosition{1.0f, 1.0f}) != nullptr);
    REQUIRE(registry.add<NetworkedPosition>(second, NetworkedPosition{2.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> payloads;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 3;
    options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& payload) {
        payloads.push_back(payload);
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(registry, first, archetype));
    REQUIRE(start_sync(registry, second, archetype));

    server.tick(registry);

    REQUIRE(payloads.size() == 1);
    NetworkedPayload fields = read_networked_payload(payloads[0]);
    REQUIRE_FALSE(fields.delta);
    REQUIRE(fields.x == 10);
    REQUIRE(fields.y == 10);
    REQUIRE(server.priority(1, first) == 0);
    REQUIRE(server.priority(1, second) == 1);

    registry.write<NetworkedPosition>(second) = NetworkedPosition{7.0f, 8.0f};
    server.tick(registry);

    REQUIRE(payloads.size() == 2);
    fields = read_networked_payload(payloads[1]);
    REQUIRE_FALSE(fields.delta);
    REQUIRE(fields.x == 70);
    REQUIRE(fields.y == 80);
    REQUIRE(server.priority(1, second) == 0);
}

TEST_CASE("replication server filters owner-only serialized components") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ecs::Entity health_component = kage::sync::register_sync_component<Health>(registry, "Health");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "OwnedActor",
        {
            {position_component, kage::sync::ReplicationAudience::All},
            {health_component, kage::sync::ReplicationAudience::Owner},
        });
    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<NetworkedPosition>(entity, NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(registry.add<Health>(entity, Health{42}) != nullptr);
    REQUIRE(kage::sync::set_owner(registry, entity, 2));

    std::vector<std::pair<kage::sync::ClientId, std::size_t>> sends;
    kage::sync::ReplicationServerOptions options;
    options.bandwidth_limit_bytes_per_tick = 1024;
    options.transport = [&](kage::sync::ClientId client, const kage::sync::BitBuffer& payload) {
        sends.push_back({client, payload.byte_size()});
    };

    kage::sync::ReplicationServer server(options);
    REQUIRE(server.add_client(1));
    REQUIRE(server.add_client(2));
    REQUIRE(start_sync(registry, entity, archetype));

    server.tick(registry);

    REQUIRE(sends.size() == 2);
    REQUIRE(sends[0] == std::pair<kage::sync::ClientId, std::size_t>{1, 3});
    REQUIRE(sends[1] == std::pair<kage::sync::ClientId, std::size_t>{2, 3 + sizeof(Health)});
}
