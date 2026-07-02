#include "test_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ashiato_sync_tests;

namespace {

ashiato::sync::SyncArchetypeId define_smooth_display_archetype(ashiato::Registry& registry) {
    const ashiato::Entity smooth =
        ashiato::sync::register_sync_component<SmoothPosition>(registry, "SmoothPosition");
    REQUIRE(ashiato::sync::set_fractional_tick_sampled(registry, smooth));
    return ashiato::sync::define_archetype(
        registry,
        "SmoothDisplayActor",
        {{smooth, ashiato::sync::ReplicationAudience::All, ashiato::sync::ComponentInterpolation::Interpolate}});
}

}  // namespace

TEST_CASE("server-backed fractional tick frame interpolates between fixed simulation frames") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = define_smooth_display_archetype(registry);

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<SmoothPosition>(entity, SmoothPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    ashiato::sync::ReplicationServer server(registry, options);
    ashiato::sync::FractionalTickSampler display(server);

    server.tick(registry, server.options().fixed_dt_seconds);
    display.capture_server_frame(registry);

    registry.write<SmoothPosition>(entity) = SmoothPosition{10.0f, 20.0f};
    server.tick(registry, server.options().fixed_dt_seconds);
    display.capture_server_frame(registry);

    REQUIRE(server.tick(registry, 0.5));
    const std::vector<ashiato::sync::FractionalTickSample>& entities = display.entities(registry);
    REQUIRE(entities.size() == 1U);

    SmoothPosition sampled{};
    REQUIRE(entities[0].try_get_sampled_value(registry, sampled));
    REQUIRE(sampled.x == 5.0f);
    REQUIRE(sampled.y == 10.0f);
    REQUIRE(display.target_frame() == 1.5);
}

TEST_CASE("server-backed fractional tick capture ignores render ticks without fixed simulation frames") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = define_smooth_display_archetype(registry);

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<SmoothPosition>(entity, SmoothPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    ashiato::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    ashiato::sync::ReplicationServer server(registry, options);
    ashiato::sync::FractionalTickSampler display(server);

    server.tick(registry, server.options().fixed_dt_seconds);
    display.capture_server_frame(registry);

    registry.write<SmoothPosition>(entity) = SmoothPosition{10.0f, 20.0f};
    server.tick(registry, server.options().fixed_dt_seconds);
    display.capture_server_frame(registry);

    REQUIRE(server.tick(registry, 0.5));
    display.capture_server_frame(registry);

    const std::vector<ashiato::sync::FractionalTickSample>& entities = display.entities(registry);
    REQUIRE(entities.size() == 1U);

    SmoothPosition sampled{};
    REQUIRE(entities[0].try_get_sampled_value(registry, sampled));
    REQUIRE(sampled.x == 5.0f);
    REQUIRE(sampled.y == 10.0f);
    REQUIRE(display.target_frame() == 1.5);
}

TEST_CASE("server-backed fractional tick frame uses current snapshot without a previous frame") {
    ashiato::Registry registry;
    ashiato_sync_tests::configure_test_server_registry(registry);
    const ashiato::sync::SyncArchetypeId archetype = define_smooth_display_archetype(registry);

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<SmoothPosition>(entity, SmoothPosition{3.0f, 4.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    ashiato::sync::ReplicationServer server(registry);
    ashiato::sync::FractionalTickSampler display(server);

    server.tick(registry, server.options().fixed_dt_seconds);
    display.capture_server_frame(registry);

    const std::vector<ashiato::sync::FractionalTickSample>& entities = display.entities(registry);
    REQUIRE(entities.size() == 1U);
    SmoothPosition sampled{};
    REQUIRE(entities[0].try_get_sampled_value(registry, sampled));
    REQUIRE(sampled.x == 3.0f);
    REQUIRE(sampled.y == 4.0f);
}
