#include "test_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kage_sync_tests;

namespace {

kage::sync::SyncArchetypeId define_smooth_display_archetype(ecs::Registry& registry) {
    const ecs::Entity smooth =
        kage::sync::register_sync_component<SmoothPosition>(registry, "SmoothPosition");
    REQUIRE(kage::sync::set_fractional_tick_sampled(registry, smooth));
    return kage::sync::define_archetype(
        registry,
        "SmoothDisplayActor",
        {{smooth, kage::sync::ReplicationAudience::All, kage::sync::ComponentInterpolation::Interpolate}});
}

}  // namespace

TEST_CASE("server-backed fractional tick frame interpolates between fixed simulation frames") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const kage::sync::SyncArchetypeId archetype = define_smooth_display_archetype(registry);

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<SmoothPosition>(entity, SmoothPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    kage::sync::ReplicationServerOptions options;
    options.fixed_dt_seconds = 1.0;
    kage::sync::ReplicationServer server(options);
    kage::sync::FractionalTickSampler display(server);

    server.tick(registry, server.options().fixed_dt_seconds);
    display.capture_server_frame(registry);

    registry.write<SmoothPosition>(entity) = SmoothPosition{10.0f, 20.0f};
    server.tick(registry, server.options().fixed_dt_seconds);
    display.capture_server_frame(registry);

    REQUIRE(server.tick(registry, 0.5));
    const std::vector<kage::sync::FractionalTickSample>& entities = display.entities(registry);
    REQUIRE(entities.size() == 1U);

    SmoothPosition sampled{};
    REQUIRE(entities[0].try_get_sampled_value(registry, sampled));
    REQUIRE(sampled.x == 5.0f);
    REQUIRE(sampled.y == 10.0f);
    REQUIRE(display.target_frame() == 1.5);
}

TEST_CASE("server-backed fractional tick frame uses current snapshot without a previous frame") {
    ecs::Registry registry;
    kage::sync::configure_server(registry);
    const kage::sync::SyncArchetypeId archetype = define_smooth_display_archetype(registry);

    const ecs::Entity entity = registry.create();
    REQUIRE(registry.add<SmoothPosition>(entity, SmoothPosition{3.0f, 4.0f}) != nullptr);
    REQUIRE(start_sync(registry, entity, archetype));

    kage::sync::ReplicationServer server;
    kage::sync::FractionalTickSampler display(server);

    server.tick(registry, server.options().fixed_dt_seconds);
    display.capture_server_frame(registry);

    const std::vector<kage::sync::FractionalTickSample>& entities = display.entities(registry);
    REQUIRE(entities.size() == 1U);
    SmoothPosition sampled{};
    REQUIRE(entities[0].try_get_sampled_value(registry, sampled));
    REQUIRE(sampled.x == 3.0f);
    REQUIRE(sampled.y == 4.0f);
}
