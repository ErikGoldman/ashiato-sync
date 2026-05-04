#include "client/frame_data.hpp"

#include "test_components.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

TEST_CASE("client frame data initializes and accesses component payloads") {
    ecs::Registry registry;
    const ecs::Entity position = kage::sync::register_sync_component<kage_sync_tests::Position>(registry, "Position");
    const kage::sync::SyncArchetypeId archetype_id = kage::sync::define_archetype(
        registry,
        "Actor",
        {{position, kage::sync::ReplicationAudience::All}});

    const kage::sync::SyncArchetype& archetype = registry.get<kage::sync::SyncSettings>().archetypes[archetype_id.value];
    kage::sync::QuantizedFrameData frame;
    REQUIRE(kage::sync::client_detail::init_frame_data(archetype, frame));
    REQUIRE(frame.present_mask == 0);
    REQUIRE(frame.bytes.size() == sizeof(kage_sync_tests::Position));
    REQUIRE(kage::sync::client_detail::frame_component_data(archetype, frame, 0) == nullptr);

    kage_sync_tests::Position value{3.0f, 4.0f};
    std::uint8_t* bytes = kage::sync::client_detail::mutable_frame_component_data(archetype, frame, 0);
    REQUIRE(bytes != nullptr);
    std::memcpy(bytes, &value, sizeof(value));

    REQUIRE(frame.present_mask == 1);
    const std::uint8_t* read = kage::sync::client_detail::frame_component_data(archetype, frame, 0);
    REQUIRE(read == bytes);
}

TEST_CASE("client frame data applies and removes archetype tags") {
    ecs::Registry registry;
    kage::sync::register_components(registry);
    const ecs::Entity tag = registry.register_tag("LocalTag");
    const kage::sync::SyncArchetypeId archetype_id = kage::sync::define_archetype(
        registry,
        kage::sync::SyncArchetypeDesc{
            "Tagged",
            {{tag, kage::sync::ReplicationAudience::All}},
            {}});
    const kage::sync::SyncArchetype& archetype = registry.get<kage::sync::SyncSettings>().archetypes[archetype_id.value];

    const ecs::Entity entity = registry.create();
    REQUIRE(kage::sync::client_detail::apply_archetype_tags(registry, entity, archetype, 1));
    REQUIRE(registry.has(entity, tag));

    REQUIRE(kage::sync::client_detail::apply_archetype_tags(registry, entity, archetype, 0));
    REQUIRE_FALSE(registry.has(entity, tag));

    REQUIRE(kage::sync::client_detail::apply_archetype_tags(registry, entity, archetype, 1));
    kage::sync::client_detail::remove_archetype_tags(registry, entity, archetype);
    REQUIRE_FALSE(registry.has(entity, tag));
}
