#include "client/frame_data.hpp"

#include "test_components.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

TEST_CASE("client frame data initializes and accesses component payloads") {
    ashiato::Registry registry;
    const ashiato::Entity position = ashiato::sync::register_sync_component<ashiato_sync_tests::Position>(registry, "Position");
    const ashiato::sync::SyncArchetypeId archetype_id = ashiato::sync::define_archetype(
        registry,
        "Actor",
        {{position, ashiato::sync::ReplicationAudience::All}});

    const ashiato::sync::SyncArchetype& archetype = registry.get<ashiato::sync::SyncSettings>().archetypes[archetype_id.value];
    ashiato::sync::QuantizedFrameData frame;
    REQUIRE(ashiato::sync::client_detail::init_frame_data(archetype, frame));
    REQUIRE(frame.present_mask == 0);
    REQUIRE(frame.bytes.size() == sizeof(ashiato_sync_tests::Position));
    REQUIRE(ashiato::sync::client_detail::frame_component_data(archetype, frame, 0) == nullptr);

    ashiato_sync_tests::Position value{3.0f, 4.0f};
    std::uint8_t* bytes = ashiato::sync::client_detail::mutable_frame_component_data(archetype, frame, 0);
    REQUIRE(bytes != nullptr);
    std::memcpy(bytes, &value, sizeof(value));

    REQUIRE(frame.present_mask == 1);
    const std::uint8_t* read = ashiato::sync::client_detail::frame_component_data(archetype, frame, 0);
    REQUIRE(read == bytes);
}

TEST_CASE("client frame data rejects malformed archetype and frame layouts") {
    ashiato::Registry registry;
    const ashiato::Entity position = ashiato::sync::register_sync_component<ashiato_sync_tests::Position>(registry, "Position");
    const ashiato::sync::SyncArchetypeId archetype_id = ashiato::sync::define_archetype(
        registry,
        "Actor",
        {{position, ashiato::sync::ReplicationAudience::All}});

    const ashiato::sync::SyncArchetype& archetype = registry.get<ashiato::sync::SyncSettings>().archetypes[archetype_id.value];
    ashiato::sync::QuantizedFrameData frame;
    REQUIRE(ashiato::sync::client_detail::init_frame_data(archetype, frame));

    ashiato::sync::SyncArchetype missing_offset = archetype;
    missing_offset.component_offsets.clear();
    REQUIRE_FALSE(ashiato::sync::client_detail::init_frame_data(missing_offset, frame));
    REQUIRE(ashiato::sync::client_detail::frame_component_data(missing_offset, frame, 0) == nullptr);
    REQUIRE(ashiato::sync::client_detail::mutable_frame_component_data(missing_offset, frame, 0) == nullptr);

    ashiato::sync::SyncArchetype oversized_tags = archetype;
    oversized_tags.tags.resize(65);
    REQUIRE_FALSE(ashiato::sync::client_detail::init_frame_data(oversized_tags, frame));

    ashiato::sync::QuantizedFrameData truncated = frame;
    truncated.bytes.clear();
    truncated.present_mask = 1;
    REQUIRE(ashiato::sync::client_detail::frame_component_data(archetype, truncated, 0) == nullptr);
    REQUIRE(ashiato::sync::client_detail::mutable_frame_component_data(archetype, truncated, 0) == nullptr);

    REQUIRE_FALSE(ashiato::sync::client_detail::frame_has_component(frame, 64));
    REQUIRE_FALSE(ashiato::sync::client_detail::tag_bit_set(1, 64));
}

TEST_CASE("client frame data applies and removes archetype tags") {
    ashiato::Registry registry;
    ashiato::sync::register_components(registry);
    const ashiato::Entity tag = registry.register_tag("LocalTag");
    const ashiato::sync::SyncArchetypeId archetype_id = ashiato::sync::define_archetype(
        registry,
        ashiato::sync::SyncArchetypeDesc{
            "Tagged",
            {{tag, ashiato::sync::ReplicationAudience::All}},
            {}});
    const ashiato::sync::SyncArchetype& archetype = registry.get<ashiato::sync::SyncSettings>().archetypes[archetype_id.value];

    const ashiato::Entity entity = registry.create();
    REQUIRE(ashiato::sync::client_detail::apply_archetype_tags(registry, entity, archetype, 1));
    REQUIRE(registry.has(entity, tag));

    REQUIRE(ashiato::sync::client_detail::apply_archetype_tags(registry, entity, archetype, 0));
    REQUIRE_FALSE(registry.has(entity, tag));

    REQUIRE(ashiato::sync::client_detail::apply_archetype_tags(registry, entity, archetype, 1));
    ashiato::sync::client_detail::remove_archetype_tags(registry, entity, archetype);
    REQUIRE_FALSE(registry.has(entity, tag));
}
