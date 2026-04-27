#include "kage/sync/sync.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

struct Health {
    std::int32_t value = 100;
};

kage::sync::SyncArchetypeId define_position_archetype(ecs::Registry& registry) {
    const ecs::Entity position_component = registry.register_component<Position>("Position");
    return kage::sync::define_archetype(
        registry,
        "PositionActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
}

}  // namespace

TEST_CASE("sync components register into the ecs registry") {
    ecs::Registry registry;

    kage::sync::register_components(registry);

    REQUIRE(registry.component<kage::sync::SyncSettings>());
    REQUIRE(registry.component<kage::sync::Replicated>());
    REQUIRE(registry.component<kage::sync::NetworkOwner>());

    const kage::sync::SyncSettings& settings = registry.get<kage::sync::SyncSettings>();
    REQUIRE(settings.role == kage::sync::SyncRole::Server);
    REQUIRE(settings.local_client == kage::sync::invalid_client_id);
    REQUIRE(settings.archetypes.empty());
}

TEST_CASE("server and client configuration update singleton settings") {
    ecs::Registry registry;

    kage::sync::configure_client(registry, 42);

    REQUIRE(registry.get<kage::sync::SyncSettings>().role == kage::sync::SyncRole::Client);
    REQUIRE(registry.get<kage::sync::SyncSettings>().local_client == 42);

    kage::sync::configure_server(registry);

    REQUIRE(registry.get<kage::sync::SyncSettings>().role == kage::sync::SyncRole::Server);
    REQUIRE(registry.get<kage::sync::SyncSettings>().local_client == kage::sync::invalid_client_id);
}

TEST_CASE("sync archetypes store component replication settings in the singleton") {
    ecs::Registry registry;
    const ecs::Entity position_component = registry.register_component<Position>("Position");
    const ecs::Entity health_component = registry.register_component<Health>("Health");

    const kage::sync::SyncArchetypeId actor = kage::sync::define_archetype(
        registry,
        "Actor",
        {
            {position_component, kage::sync::ReplicationAudience::All},
            {health_component, kage::sync::ReplicationAudience::Owner},
        });

    const kage::sync::SyncArchetypeId projectile = kage::sync::define_archetype(
        registry,
        "Projectile",
        {{position_component, kage::sync::ReplicationAudience::All}});

    REQUIRE(actor.value == 0);
    REQUIRE(projectile.value == 1);

    const kage::sync::SyncArchetype* found_actor = kage::sync::find_archetype(registry, actor);
    REQUIRE(found_actor != nullptr);
    REQUIRE(found_actor->name == "Actor");
    REQUIRE(found_actor->components.size() == 2);
    REQUIRE(found_actor->components[0].component == position_component);
    REQUIRE(found_actor->components[0].audience == kage::sync::ReplicationAudience::All);
    REQUIRE(found_actor->components[1].component == health_component);
    REQUIRE(found_actor->components[1].audience == kage::sync::ReplicationAudience::Owner);

    REQUIRE(kage::sync::find_archetype(registry, kage::sync::SyncArchetypeId{99}) == nullptr);
}

TEST_CASE("sync archetypes reject unregistered component ids") {
    ecs::Registry registry;

    REQUIRE_THROWS_AS(
        kage::sync::define_archetype(
            registry,
            "Invalid",
            {{ecs::Entity{123}, kage::sync::ReplicationAudience::All}}),
        std::invalid_argument);
}

TEST_CASE("entities can be marked and unmarked as replicated") {
    ecs::Registry registry;
    const ecs::Entity position_component = registry.register_component<Position>("Position");
    const kage::sync::SyncArchetypeId actor = kage::sync::define_archetype(
        registry,
        "Actor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();

    REQUIRE_FALSE(kage::sync::mark_replicated(registry, ecs::Entity{}, actor));
    REQUIRE_FALSE(kage::sync::mark_replicated(registry, entity, kage::sync::SyncArchetypeId{22}));

    REQUIRE(kage::sync::mark_replicated(registry, entity, actor));
    REQUIRE(registry.contains<kage::sync::Replicated>(entity));
    REQUIRE(registry.get<kage::sync::Replicated>(entity).archetype == actor);

    REQUIRE(kage::sync::unmark_replicated(registry, entity));
    REQUIRE_FALSE(registry.contains<kage::sync::Replicated>(entity));
    REQUIRE_FALSE(kage::sync::unmark_replicated(registry, entity));
}

TEST_CASE("owners can be assigned and replaced independently of replication marker") {
    ecs::Registry registry;
    const ecs::Entity entity = registry.create();

    REQUIRE_FALSE(kage::sync::set_owner(registry, ecs::Entity{}, 7));

    REQUIRE(kage::sync::set_owner(registry, entity, 7));
    REQUIRE(registry.contains<kage::sync::NetworkOwner>(entity));
    REQUIRE(registry.get<kage::sync::NetworkOwner>(entity).client == 7);
    REQUIRE_FALSE(registry.contains<kage::sync::Replicated>(entity));

    REQUIRE(kage::sync::set_owner(registry, entity, 11));
    REQUIRE(registry.get<kage::sync::NetworkOwner>(entity).client == 11);
}

TEST_CASE("replication server tracks clients and explicit replicated entities") {
    ecs::Registry registry;
    const kage::sync::SyncArchetypeId archetype = define_position_archetype(registry);
    const ecs::Entity entity = registry.create();

    kage::sync::ReplicationServer server;

    REQUIRE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(7));
    REQUIRE_FALSE(server.add_client(kage::sync::invalid_client_id));
    REQUIRE(server.has_client(7));
    REQUIRE(server.client_count() == 1);

    REQUIRE_FALSE(server.add_replicated(registry, ecs::Entity{}, archetype));
    REQUIRE_FALSE(server.add_replicated(registry, entity, kage::sync::SyncArchetypeId{999}));
    REQUIRE(server.add_replicated(registry, entity, archetype));
    REQUIRE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 1);
    REQUIRE(registry.contains<kage::sync::Replicated>(entity));

    REQUIRE(server.remove_replicated(registry, entity));
    REQUIRE_FALSE(server.is_replicated(entity));
    REQUIRE(server.replicated_count() == 0);
    REQUIRE_FALSE(registry.contains<kage::sync::Replicated>(entity));
    REQUIRE_FALSE(server.remove_replicated(registry, entity));

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
        REQUIRE(server.add_replicated(registry, entity, archetype));
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
        REQUIRE(server.add_replicated(registry, entity, archetype));
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
    REQUIRE(server.add_replicated(registry, first, archetype));
    REQUIRE(server.add_replicated(registry, second, archetype));
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
    REQUIRE(server.add_replicated(registry, destroyed, archetype));
    REQUIRE(server.add_replicated(registry, unmarked, archetype));
    REQUIRE(server.add_replicated(registry, live, archetype));

    REQUIRE(registry.destroy(destroyed));
    REQUIRE(kage::sync::unmark_replicated(registry, unmarked));

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
    REQUIRE(server.add_replicated(registry, entity, archetype));

    int sends = 0;
    server.tick(registry, [&](kage::sync::ClientId, ecs::Entity) {
        ++sends;
    });

    REQUIRE(sends == 0);
    REQUIRE(server.priority(1, entity) == 1);
}
