#include "test_components.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <stdexcept>
#include <type_traits>

using kage_sync_tests::Health;
using kage_sync_tests::NetworkedPayload;
using kage_sync_tests::NetworkedPosition;
using kage_sync_tests::Position;
using kage_sync_tests::Secret;
using kage_sync_tests::Visible;
using kage_sync_tests::read_networked_payload;

static_assert(
    std::is_nothrow_move_constructible<kage::sync::SyncSettings>::value,
    "SyncSettings must satisfy ecs component storage requirements");

TEST_CASE("sync components register into the ecs registry") {
    ecs::Registry registry;

    kage::sync::register_components(registry);

    REQUIRE(registry.component<kage::sync::SyncSettings>());
    REQUIRE(registry.component<kage::sync::SyncAuthority>());
    REQUIRE(registry.component<kage::sync::Replicated>());
    REQUIRE(registry.component<kage::sync::NetworkOwner>());
    REQUIRE(registry.component<kage::sync::FractionalTickSampled>());

    const kage::sync::SyncSettings& settings = registry.get<kage::sync::SyncSettings>();
    REQUIRE(settings.role == kage::sync::SyncRole::Server);
    REQUIRE(settings.local_client == kage::sync::invalid_client_id);
    REQUIRE(settings.archetypes.empty());
    REQUIRE(registry.get<kage::sync::SyncAuthority>().is_authoritative());
}

TEST_CASE("fractional tick sample marker is stored as a component entity tag") {
    ecs::Registry registry;
    const ecs::Entity smooth_component =
        kage::sync::register_sync_component<kage_sync_tests::SmoothPosition>(registry, "SmoothPosition");
    const ecs::Entity position_component = kage::sync::register_sync_component<Position>(registry, "Position");

    REQUIRE(kage::sync::set_fractional_tick_sampled(registry, smooth_component));
    REQUIRE(kage::sync::is_fractional_tick_sampled(registry, smooth_component));
    REQUIRE(registry.has<kage::sync::FractionalTickSampled>(smooth_component));

    REQUIRE_FALSE(kage::sync::set_fractional_tick_sampled(registry, position_component));
    REQUIRE_FALSE(kage::sync::is_fractional_tick_sampled(registry, position_component));

    REQUIRE(kage::sync::set_fractional_tick_sampled<kage_sync_tests::SmoothPosition>(registry, false));
    REQUIRE_FALSE(kage::sync::is_fractional_tick_sampled<kage_sync_tests::SmoothPosition>(registry));
}

TEST_CASE("server and client configuration update singleton settings") {
    ecs::Registry registry;

    kage::sync::configure_client(registry, 42);

    REQUIRE(registry.get<kage::sync::SyncSettings>().role == kage::sync::SyncRole::Client);
    REQUIRE(registry.get<kage::sync::SyncSettings>().local_client == 42);
    REQUIRE_FALSE(registry.get<kage::sync::SyncAuthority>().is_authoritative());

    kage::sync::configure_server(registry);

    REQUIRE(registry.get<kage::sync::SyncSettings>().role == kage::sync::SyncRole::Server);
    REQUIRE(registry.get<kage::sync::SyncSettings>().local_client == kage::sync::invalid_client_id);
    REQUIRE(registry.get<kage::sync::SyncAuthority>().is_authoritative());
}

TEST_CASE("sync archetypes store component replication settings in the singleton") {
    ecs::Registry registry;
    const ecs::Entity position_component = kage::sync::register_sync_component<Position>(registry, "Position");
    const ecs::Entity health_component = kage::sync::register_sync_component<Health>(registry, "Health");

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

TEST_CASE("sync archetypes store tag replication settings in reserved slot zero") {
    ecs::Registry registry;
    const ecs::Entity visible = registry.register_component<Visible>("Visible");
    const ecs::Entity secret = registry.register_component<Secret>("Secret");
    const ecs::Entity position_component = kage::sync::register_sync_component<Position>(registry, "Position");

    const kage::sync::SyncArchetypeId actor = kage::sync::define_archetype(
        registry,
        kage::sync::SyncArchetypeDesc{
            "TaggedActor",
            {
                {visible, kage::sync::ReplicationAudience::All},
                {secret, kage::sync::ReplicationAudience::Owner},
            },
            {{position_component, kage::sync::ReplicationAudience::All}},
        });

    const kage::sync::SyncArchetype* found = kage::sync::find_archetype(registry, actor);
    REQUIRE(found != nullptr);
    REQUIRE(found->tags.size() == 2);
    REQUIRE(found->tags[0].tag == visible);
    REQUIRE(found->tags[0].audience == kage::sync::ReplicationAudience::All);
    REQUIRE(found->tags[1].tag == secret);
    REQUIRE(found->tags[1].audience == kage::sync::ReplicationAudience::Owner);
    REQUIRE(found->components.size() == 1);
}

TEST_CASE("sync archetypes reject invalid tag declarations") {
    ecs::Registry registry;
    const ecs::Entity visible = registry.register_component<Visible>("Visible");
    const ecs::Entity position = registry.register_component<Position>("Position");

    REQUIRE_THROWS_AS(
        kage::sync::define_archetype(
            registry,
            kage::sync::SyncArchetypeDesc{"Invalid", {{position, kage::sync::ReplicationAudience::All}}, {}}),
        std::invalid_argument);

    REQUIRE_THROWS_AS(
        kage::sync::define_archetype(
            registry,
            kage::sync::SyncArchetypeDesc{
                "Duplicate",
                {
                    {visible, kage::sync::ReplicationAudience::All},
                    {visible, kage::sync::ReplicationAudience::Owner},
                },
                {}}),
        std::invalid_argument);
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

TEST_CASE("sync archetypes reject duplicate component declarations") {
    ecs::Registry registry;
    const ecs::Entity position_component = kage::sync::register_sync_component<Position>(registry, "Position");

    REQUIRE_THROWS_AS(
        kage::sync::define_archetype(
            registry,
            "Duplicate",
            {
                {position_component, kage::sync::ReplicationAudience::All},
                {position_component, kage::sync::ReplicationAudience::Owner},
            }),
        std::invalid_argument);
}

TEST_CASE("sync archetypes reject components without sync traits") {
    ecs::Registry registry;
    const ecs::Entity position_component = registry.register_component<Position>("Position");

    REQUIRE_THROWS_AS(
        kage::sync::define_archetype(
            registry,
            "Invalid",
            {{position_component, kage::sync::ReplicationAudience::All}}),
        std::invalid_argument);
}

TEST_CASE("sync component traits provide type-erased quantization and serialization ops") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const kage::sync::SyncComponentOps* ops = kage::sync::find_component_ops(registry, position_component);
    REQUIRE(ops != nullptr);

    const NetworkedPosition position{1.0f, 2.0f};
    std::array<std::uint8_t, sizeof(kage_sync_tests::QuantizedNetworkedPosition)> quantized{};
    ops->quantize(&position, quantized.data());

    ecs::BitBuffer payload;
    ops->serialize(nullptr, quantized.data(), payload, nullptr);
    const NetworkedPayload fields = read_networked_payload(payload);
    REQUIRE_FALSE(fields.delta);
    REQUIRE(fields.x == 10);
    REQUIRE(fields.y == 20);

    payload.reset_read();
    std::array<std::uint8_t, sizeof(kage_sync_tests::QuantizedNetworkedPosition)> decoded{};
    REQUIRE(ops->deserialize(payload, nullptr, decoded.data(), nullptr));

    NetworkedPosition dequantized;
    ops->dequantize(decoded.data(), &dequantized);
    REQUIRE(dequantized.x == 1.0f);
    REQUIRE(dequantized.y == 2.0f);

    const ecs::Entity entity = registry.create();
    REQUIRE(ops->push_to_ecs(registry, entity, decoded.data()));
    REQUIRE(registry.contains<NetworkedPosition>(entity));
    REQUIRE(registry.remove(entity, position_component));
    REQUIRE_FALSE(registry.contains<NetworkedPosition>(entity));
}

TEST_CASE("replication configuration is a direct ecs component") {
    ecs::Registry registry;
    const ecs::Entity position_component = kage::sync::register_sync_component<Position>(registry, "Position");
    const kage::sync::SyncArchetypeId actor = kage::sync::define_archetype(
        registry,
        "Actor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    const ecs::Entity entity = registry.create();

    REQUIRE(registry.add<kage::sync::Replicated>(ecs::Entity{}, kage::sync::Replicated{actor}) == nullptr);

    REQUIRE(registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{actor}) != nullptr);
    REQUIRE(registry.contains<kage::sync::Replicated>(entity));
    REQUIRE(registry.get<kage::sync::Replicated>(entity).archetype == actor);

    REQUIRE(registry.add<kage::sync::Replicated>(
                entity,
                kage::sync::Replicated{kage::sync::SyncArchetypeId{22}}) != nullptr);
    REQUIRE(registry.get<kage::sync::Replicated>(entity).archetype == kage::sync::SyncArchetypeId{22});

    REQUIRE(registry.remove<kage::sync::Replicated>(entity));
    REQUIRE_FALSE(registry.contains<kage::sync::Replicated>(entity));
    REQUIRE_FALSE(registry.remove<kage::sync::Replicated>(entity));
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
