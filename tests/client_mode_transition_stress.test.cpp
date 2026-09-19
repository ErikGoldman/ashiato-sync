#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

using namespace ashiato_sync_tests;

namespace {

constexpr ashiato::sync::ClientId test_client_id = 1;

ashiato::sync::ReplicationClientOptions mode_test_options(
    ashiato::sync::ReplicationClientMode mode,
    ashiato::sync::SyncFrame buffered_lag = 3U) {
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = mode;
    options.buffered.auto_buffered_frame_lag = false;
    options.buffered.buffered_frame_lag = buffered_lag;
    options.prediction.auto_lead_frames = false;
    options.prediction.lead_frames = 2U;
    options.prediction.rollback_policy = ashiato::sync::ReplicationRollbackPolicy::All;
    return options;
}

ashiato::sync::SyncArchetypeId prepare_predicted_registry(ashiato::Registry& registry) {
    const ashiato::sync::SyncArchetypeId archetype = define_predicted_archetype(registry);
    configure_test_client_registry(registry, test_client_id);
    return archetype;
}

struct TaggedPredictedSchema {
    ashiato::sync::SyncArchetypeId archetype;
    ashiato::Entity visible;
};

TaggedPredictedSchema define_tagged_predicted_archetype(ashiato::Registry& registry) {
    const ashiato::Entity visible = registry.register_component<Visible>("Visible");
    const ashiato::Entity position =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    return TaggedPredictedSchema{
        ashiato::sync::define_archetype(
            registry,
            ashiato::sync::SyncArchetypeDesc{
                "TaggedPredictedActor",
                {{visible, ashiato::sync::ReplicationAudience::All}},
                {{position, ashiato::sync::ReplicationAudience::All}},
            }),
        visible};
}

class ModeTestClient {
public:
    explicit ModeTestClient(
        ashiato::sync::ReplicationClientMode initial_mode,
        ashiato::sync::SyncFrame buffered_lag = 3U)
        : archetype_(prepare_predicted_registry(registry_)),
          client_(registry_, make_test_client_options(registry_, mode_test_options(initial_mode, buffered_lag))) {
        REQUIRE(archetype_.value == 0U);
        client_.simulation_job<PredictedPosition>(registry_, 0).each(
            [](ashiato::Entity, PredictedPosition& position) {
                position.x += 1.0f;
            });
    }

    bool receive(ashiato::sync::SyncFrame frame, float x, std::uint32_t packet_id = 0U) {
        return client_.receive(
            registry_,
            make_predicted_position_packet(frame, server_entity_, PredictedPosition{x, 7.0f}, packet_id));
    }

    void set_mode(ashiato::sync::ReplicationClientMode mode) {
        client_.set_entity_mode(registry_, network_id(), mode);
    }

    void tick(ashiato::sync::SyncFrame count = 1U) {
        tick_client_fixed_frames(client_, registry_, count);
    }

    ashiato::sync::ClientEntityNetworkId network_id(std::uint32_t version = 1U) const {
        return test_client_entity_network_id(test_client_id, server_entity_, version);
    }

    ashiato::Entity local(std::uint32_t version = 1U) const {
        return client_.local_entity(network_id(version));
    }

    float x() const {
        return registry_.get<PredictedPosition>(local()).x;
    }

    ashiato::Registry& registry() {
        return registry_;
    }

    ashiato::sync::ReplicationClient& client() {
        return client_;
    }

private:
    ashiato::Registry registry_;
    ashiato::sync::SyncArchetypeId archetype_;
    ashiato::sync::ReplicationClient client_;
    const ashiato::Entity server_entity_{42U};
};

ashiato::BitBuffer make_multi_entity_position_packet(
    ashiato::sync::SyncFrame frame,
    const std::vector<std::pair<ashiato::Entity, PredictedPosition>>& records) {
    ashiato::BitBuffer packet;
    packet.write_bits(ashiato::sync::protocol::server_update_message, ashiato::sync::protocol::message_bits);
    packet.write_bits(frame, 32U);
    packet.write_bits(frame, ashiato::sync::protocol::server_packet_id_bits);
    packet.write_bits(0U, 32U);
    packet.write_bits(static_cast<std::uint16_t>(records.size()), 16U);
    for (const auto& [entity, position] : records) {
        packet.write_bool(false);
        ashiato::sync::protocol::write_network_entity_id(packet, test_network_id(entity));
        packet.write_bool(true);
        packet.write_bits(0U, 32U);
        packet.write_bool(false);
        packet.write_bits(1U, 16U);
        packet.write_bits(1U, ashiato::sync::protocol::bits_for_range(2U));
        packet.write_bits(static_cast<std::int32_t>(position.x * 10.0f), 16U);
        packet.write_bits(static_cast<std::int32_t>(position.y * 10.0f), 16U);
        packet.write_bool(false);
    }
    return packet;
}

}  // namespace

TEST_CASE(
    "client entities preserve identity and state through every directed mode transition",
    "[client][mode-transition][stress]") {
    using Mode = ashiato::sync::ReplicationClientMode;

    ModeTestClient harness(Mode::Snap);
    REQUIRE(harness.receive(1U, 5.0f));
    const ashiato::Entity original = harness.local();
    REQUIRE(original);

    const std::array<Mode, 7> transitions{
        Mode::BufferedInterpolation,
        Mode::Snap,
        Mode::Predict,
        Mode::Snap,
        Mode::BufferedInterpolation,
        Mode::Predict,
        Mode::BufferedInterpolation,
    };
    for (const Mode mode : transitions) {
        harness.set_mode(mode);
        CAPTURE(mode);
        REQUIRE(harness.client().entity_mode(harness.network_id()) == mode);
        REQUIRE(harness.local() == original);
        REQUIRE(harness.registry().contains<PredictedPosition>(original));
        REQUIRE(harness.registry().get<PredictedPosition>(original).y == Catch::Approx(7.0f));
    }

    for (int cycle = 0; cycle < 12; ++cycle) {
        for (const Mode mode : {Mode::Snap, Mode::BufferedInterpolation, Mode::Predict}) {
            harness.set_mode(mode);
            harness.tick();
            CAPTURE(cycle, mode);
            REQUIRE(harness.local() == original);
            REQUIRE(harness.registry().alive(original));
            REQUIRE(harness.registry().contains<PredictedPosition>(original));
        }
    }
}

TEST_CASE(
    "unmaterialized buffered entities can switch to either immediate mode",
    "[client][mode-transition][stress]") {
    using Mode = ashiato::sync::ReplicationClientMode;

    SECTION("snap") {
        ModeTestClient harness(Mode::BufferedInterpolation, 5U);
        REQUIRE(harness.receive(10U, 12.0f));
        REQUIRE(harness.client().has_entity(harness.network_id()));
        REQUIRE_FALSE(harness.local());

        harness.set_mode(Mode::Snap);
        REQUIRE(harness.local());
        REQUIRE(harness.x() == Catch::Approx(12.0f));
    }

    SECTION("predict") {
        ModeTestClient harness(Mode::BufferedInterpolation, 5U);
        REQUIRE(harness.receive(10U, 12.0f));
        REQUIRE_FALSE(harness.local());

        harness.set_mode(Mode::Predict);
        const ashiato::Entity materialized = harness.local();
        REQUIRE(materialized);
        REQUIRE(harness.x() == Catch::Approx(12.0f));
        harness.tick();
        REQUIRE(harness.local() == materialized);
        REQUIRE(harness.x() == Catch::Approx(13.0f));
    }
}

TEST_CASE(
    "tags and components survive repeated mode changes without recreating identity",
    "[client][mode-transition][stress]") {
    using Mode = ashiato::sync::ReplicationClientMode;

    ashiato::Registry server_registry;
    const TaggedPredictedSchema server_schema = define_tagged_predicted_archetype(server_registry);
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<PredictedPosition>(server_entity, PredictedPosition{3.0f, 9.0f}) != nullptr);
    REQUIRE(server_registry.add_tag(server_entity, server_schema.visible));
    REQUIRE(start_sync(server_registry, server_entity, server_schema.archetype));

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(test_client_id));
    REQUIRE(server.tick(server_registry, server.options().fixed_dt_seconds));
    REQUIRE(packets.size() == 1U);

    ashiato::Registry client_registry;
    const TaggedPredictedSchema client_schema = define_tagged_predicted_archetype(client_registry);
    REQUIRE(client_schema.archetype == server_schema.archetype);
    configure_test_client_registry(client_registry, test_client_id);
    ashiato::sync::ReplicationClient client(
        client_registry,
        make_test_client_options(client_registry, mode_test_options(Mode::Snap, 2U)));
    REQUIRE(client.receive(client_registry, packets.front()));

    const auto network_id = first_allocated_client_entity_network_id(test_client_id);
    const ashiato::Entity original = client.local_entity(network_id);
    REQUIRE(original);
    for (int cycle = 0; cycle < 8; ++cycle) {
        for (const Mode mode : {Mode::BufferedInterpolation, Mode::Predict, Mode::Snap}) {
            client.set_entity_mode(client_registry, network_id, mode);
            CAPTURE(cycle, mode);
            REQUIRE(client.local_entity(network_id) == original);
            REQUIRE(client_registry.has(original, client_schema.visible));
            REQUIRE(client_registry.contains<PredictedPosition>(original));
            REQUIRE(client_registry.get<PredictedPosition>(original).x == Catch::Approx(3.0f));
            REQUIRE(client_registry.get<PredictedPosition>(original).y == Catch::Approx(9.0f));
        }
    }
}

TEST_CASE(
    "mode changes consume queued interpolation and survive late authoritative corrections",
    "[client][mode-transition][stress]") {
    using Mode = ashiato::sync::ReplicationClientMode;

    ModeTestClient harness(Mode::BufferedInterpolation, 3U);
    REQUIRE(harness.receive(1U, 1.0f));
    REQUIRE(harness.client().apply_frame(harness.registry(), 1U));
    const ashiato::Entity original = harness.local();
    REQUIRE(original);

    REQUIRE(harness.receive(2U, 2.0f));
    REQUIRE(harness.receive(4U, 4.0f));
    REQUIRE(harness.x() == Catch::Approx(1.0f));

    harness.set_mode(Mode::Snap);
    REQUIRE(harness.local() == original);
    REQUIRE(harness.x() == Catch::Approx(4.0f));

    // A delayed older full update is valid traffic but must not rewind the entity.
    (void)harness.receive(3U, 30.0f, 30U);
    REQUIRE(harness.local() == original);
    REQUIRE(harness.x() == Catch::Approx(4.0f));

    harness.set_mode(Mode::Predict);
    harness.tick(2U);
    REQUIRE(harness.x() == Catch::Approx(6.0f));

    // Correct the newest authoritative frame, then move through buffered mode while
    // prediction has local history. Applying the target must converge to authority.
    REQUIRE(harness.receive(5U, 20.0f));
    harness.set_mode(Mode::BufferedInterpolation);
    REQUIRE(harness.client().apply_frame(harness.registry(), 5U));
    REQUIRE(harness.local() == original);
    REQUIRE(harness.x() == Catch::Approx(20.0f));

    harness.set_mode(Mode::Predict);
    harness.tick();
    REQUIRE(harness.x() == Catch::Approx(21.0f));
}

TEST_CASE(
    "manual mode selection remains attached to an identity while selectors handle recreates",
    "[client][mode-transition][stress]") {
    using Mode = ashiato::sync::ReplicationClientMode;

    ashiato::Registry registry;
    REQUIRE(prepare_predicted_registry(registry).value == 0U);
    int selector_calls = 0;
    ashiato::sync::ReplicationClientOptions options = mode_test_options(Mode::Snap, 2U);
    options.entities.mode_selector = [&](const ashiato::sync::ReplicatedEntityUpdateView& update) {
        PredictedPosition position;
        REQUIRE(update.try_get(registry, position));
        ++selector_calls;
        return position.x >= 100.0f ? Mode::BufferedInterpolation : Mode::Snap;
    };
    ashiato::sync::ReplicationClient client(registry, make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition>(registry, 0).each(
        [](ashiato::Entity, PredictedPosition& position) {
            position.x += 1.0f;
        });

    const ashiato::Entity server_entity{42U};
    const auto first_id = test_client_entity_network_id(test_client_id, server_entity);
    REQUIRE(client.receive(registry, make_predicted_position_packet(1U, server_entity, {1.0f, 7.0f})));
    REQUIRE(selector_calls == 1);
    client.set_entity_mode(registry, first_id, Mode::Predict);
    REQUIRE(client.receive(registry, make_predicted_position_packet(2U, server_entity, {2.0f, 7.0f})));
    REQUIRE(selector_calls == 1);
    REQUIRE(client.entity_mode(first_id) == Mode::Predict);

    client.set_entity_mode(registry, first_id, Mode::BufferedInterpolation);
    REQUIRE(client.receive(registry, make_destroy_packet(3U, server_entity)));
    const ashiato::Entity old_local = client.local_entity(first_id);
    REQUIRE(old_local);
    REQUIRE(registry.alive(old_local));
    client.set_entity_mode(registry, first_id, Mode::Snap);
    REQUIRE_FALSE(client.local_entity(first_id));
    REQUIRE_FALSE(registry.alive(old_local));
    REQUIRE(client.receive(registry, make_predicted_position_packet(4U, server_entity, {100.0f, 7.0f})));

    const auto recreated_id = test_client_entity_network_id(test_client_id, server_entity, 2U);
    REQUIRE(selector_calls == 2);
    REQUIRE(client.entity_mode(recreated_id) == Mode::BufferedInterpolation);
    REQUIRE_FALSE(client.local_entity(recreated_id));
    REQUIRE(client.apply_frame(registry, 4U));
    REQUIRE(client.local_entity(recreated_id));
}

TEST_CASE(
    "independent entities converge after sustained mode churn loss jitter and reordering",
    "[client][mode-transition][stress]") {
    using Mode = ashiato::sync::ReplicationClientMode;

    ashiato::Registry registry;
    REQUIRE(prepare_predicted_registry(registry).value == 0U);
    ashiato::sync::ReplicationClient client(
        registry,
        make_test_client_options(registry, mode_test_options(Mode::Snap, 3U)));
    client.simulation_job<PredictedPosition>(registry, 0).each(
        [](ashiato::Entity, PredictedPosition& position) {
            position.x += 1.0f;
        });

    const std::array<ashiato::Entity, 4> server_entities{
        ashiato::Entity{40U}, ashiato::Entity{41U}, ashiato::Entity{42U}, ashiato::Entity{43U}};
    auto make_records = [&](ashiato::sync::SyncFrame frame) {
        std::vector<std::pair<ashiato::Entity, PredictedPosition>> records;
        for (std::size_t index = 0; index < server_entities.size(); ++index) {
            records.push_back({
                server_entities[index],
                PredictedPosition{static_cast<float>(frame * 10U + index), static_cast<float>(index)}});
        }
        return records;
    };

    REQUIRE(client.receive(registry, make_multi_entity_position_packet(1U, make_records(1U))));
    std::array<ashiato::Entity, 4> local_entities{};
    for (std::size_t index = 0; index < server_entities.size(); ++index) {
        local_entities[index] = client.local_entity(
            test_client_entity_network_id(test_client_id, server_entities[index]));
        REQUIRE(local_entities[index]);
    }

    std::vector<ashiato::BitBuffer> delayed;
    for (ashiato::sync::SyncFrame frame = 2U; frame <= 180U; ++frame) {
        ashiato::BitBuffer packet = make_multi_entity_position_packet(frame, make_records(frame));
        if (frame % 11U != 0U) {
            if (frame % 7U == 0U) {
                delayed.push_back(std::move(packet));
            } else {
                REQUIRE(client.receive(registry, std::move(packet)));
            }
        }

        if (frame % 5U == 0U && !delayed.empty()) {
            (void)client.receive(registry, std::move(delayed.back()));
            delayed.pop_back();
        }

        const std::size_t selected = static_cast<std::size_t>(frame % server_entities.size());
        const Mode mode = static_cast<Mode>(frame % 3U);
        client.set_entity_mode(
            registry,
            test_client_entity_network_id(test_client_id, server_entities[selected]),
            mode);
        REQUIRE(client.tick(registry, client.fixed_dt_seconds()));

        for (std::size_t index = 0; index < server_entities.size(); ++index) {
            CAPTURE(frame, index, selected, mode);
            const auto network_id = test_client_entity_network_id(test_client_id, server_entities[index]);
            REQUIRE(client.local_entity(network_id) == local_entities[index]);
            REQUIRE(registry.alive(local_entities[index]));
            REQUIRE(registry.contains<PredictedPosition>(local_entities[index]));
            REQUIRE(registry.get<PredictedPosition>(local_entities[index]).y == Catch::Approx(static_cast<float>(index)));
        }
    }

    for (ashiato::BitBuffer& packet : delayed) {
        (void)client.receive(registry, std::move(packet));
    }

    constexpr ashiato::sync::SyncFrame recovery_frame = 200U;
    REQUIRE(client.receive(
        registry,
        make_multi_entity_position_packet(recovery_frame, make_records(recovery_frame))));
    for (std::size_t index = 0; index < server_entities.size(); ++index) {
        const auto network_id = test_client_entity_network_id(test_client_id, server_entities[index]);
        client.set_entity_mode(registry, network_id, Mode::Snap);
        REQUIRE(client.local_entity(network_id) == local_entities[index]);
        REQUIRE(registry.get<PredictedPosition>(local_entities[index]).x ==
                Catch::Approx(static_cast<float>(recovery_frame * 10U + index)));
        REQUIRE(registry.get<PredictedPosition>(local_entities[index]).y ==
                Catch::Approx(static_cast<float>(index)));
    }
}

TEST_CASE(
    "switching away from buffered mode neither loses nor replays queued cues",
    "[client][mode-transition][stress]") {
    using Mode = ashiato::sync::ReplicationClientMode;

    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_predicted_archetype(server_registry);
    ashiato::sync::register_sync_cue<TestCue>(server_registry);
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<PredictedPosition>(server_entity, PredictedPosition{1.0f, 7.0f}) != nullptr);
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(test_client_id));
    REQUIRE(emit_test_cue(server_registry, server_entity, 1U, TestCue{77}, 1.0f));
    REQUIRE(server.tick(server_registry, server.options().fixed_dt_seconds));
    REQUIRE(packets.size() == 1U);

    ashiato::Registry client_registry;
    REQUIRE(prepare_predicted_registry(client_registry) == server_archetype);
    client_registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(client_registry);
    ashiato::sync::ReplicationClient client(
        client_registry,
        make_test_client_options(client_registry, mode_test_options(Mode::BufferedInterpolation, 3U)));

    REQUIRE(client.receive(client_registry, packets.front()));
    const auto network_id = first_allocated_client_entity_network_id(test_client_id);
    REQUIRE_FALSE(client.local_entity(network_id));

    client.set_entity_mode(client_registry, network_id, Mode::Snap);
    const ashiato::Entity local = client.local_entity(network_id);
    REQUIRE(local);
    REQUIRE(client_registry.contains<CuePlayback>(local));
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);

    client.set_entity_mode(client_registry, network_id, Mode::BufferedInterpolation);
    REQUIRE(client.apply_frame(client_registry, 1U));
    client.set_entity_mode(client_registry, network_id, Mode::Predict);
    REQUIRE(client_registry.get<CuePlayback>(local).plays == 1);
}
