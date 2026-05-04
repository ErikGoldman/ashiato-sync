#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

TEST_CASE("predicted client mode requires ShouldRollBack traits") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = kage_sync_tests::define_position_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = kage_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);

    try {
        (void)client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}));
        FAIL("expected missing prediction rollback trait error");
    } catch (const kage::sync::ClientError& error) {
        REQUIRE(error.status() == kage::sync::ClientStatus::MissingPredictionRollbackTrait);
    }
}

TEST_CASE("predicted client snaps first frame predicts locally and skips matching rollback") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);

    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{1.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 2.0f);
}

TEST_CASE("predicted client rolls back and resimulates mismatched frames") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    options.rollback_policy = kage::sync::ReplicationRollbackPolicy::All;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<PredictedPosition>(local).x == 3.0f);
}

TEST_CASE("predicted client rolls back locally predicted cues missing from authoritative frame") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(registry);
    kage::sync::configure_client(registry, 1);

    bool emit_prediction_cue = true;
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    options.rollback_policy = kage::sync::ReplicationRollbackPolicy::All;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition, kage::sync::SyncSettings>(registry, 0).each(
        [&](ecs::Entity entity, PredictedPosition& position, kage::sync::SyncSettings& settings) {
            position.x += 1.0f;
            if (emit_prediction_cue) {
                REQUIRE(kage::sync::emit_cue(settings, entity, TestCue{7}, 1.0f));
                emit_prediction_cue = false;
            }
        });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<CuePlayback>(local).plays == 1);
    REQUIRE(registry.get<CuePlayback>(local).rollbacks == 0);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(registry.get<CuePlayback>(local).rollbacks == 1);
}

TEST_CASE("predicted client keeps locally predicted cues replayed during resimulation") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(registry);
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    options.rollback_policy = kage::sync::ReplicationRollbackPolicy::All;
    kage::sync::ReplicationClient client(options);
    int prediction_jobs = 0;
    client.simulation_job<PredictedPosition, kage::sync::SyncSettings>(registry, 0).each(
        [&](ecs::Entity entity, PredictedPosition& position, kage::sync::SyncSettings& settings) {
            position.x += 1.0f;
            ++prediction_jobs;
            if (prediction_jobs == 2 || prediction_jobs == 3) {
                REQUIRE(kage::sync::emit_cue(settings, entity, TestCue{5}, 1.0f));
            }
        });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE_FALSE(registry.contains<CuePlayback>(local));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<CuePlayback>(local).plays == 1);
    REQUIRE(registry.get<CuePlayback>(local).rollbacks == 0);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(registry.get<CuePlayback>(local).rollbacks == 0);
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(registry.get<CuePlayback>(local).rollbacks == 0);
}

#ifdef KAGE_SYNC_ENABLE_TRACING

TEST_CASE("predicted client traces rollback reason separately from rollback conflict") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    std::vector<kage::sync::SyncTraceEvent> events;
    kage::sync::SyncTracer tracer;
    tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { events.push_back(event); }});
    client.set_tracer(&tracer);

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));

    const auto conflict = std::find_if(events.begin(), events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PredictionRollbackConflict;
    });
    REQUIRE(conflict != events.end());
    REQUIRE(conflict->component == position_component);
    REQUIRE(conflict->component_name == "PredictedPosition");
    REQUIRE(conflict->data.empty());

    const auto reason = std::find_if(events.begin(), events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::RollbackReason;
    });
    REQUIRE(reason != events.end());
    REQUIRE(std::count_if(events.begin(), events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::RollbackReason;
    }) == 1);
    REQUIRE(reason->component == position_component);
    REQUIRE(reason->component_name == "PredictedPosition");
    REQUIRE(reason->data == "PredictedPosition.x mismatch");
}

TEST_CASE("predicted cue rollback tracing records server mismatch reason") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(registry);
    kage::sync::configure_client(registry, 1);

    bool emit_prediction_cue = true;
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition, kage::sync::SyncSettings>(registry, 0).each(
        [&](ecs::Entity entity, PredictedPosition& position, kage::sync::SyncSettings& settings) {
            position.x += 1.0f;
            if (emit_prediction_cue) {
                REQUIRE(kage::sync::emit_cue(settings, entity, TestCue{7}, 1.0f));
                emit_prediction_cue = false;
            }
        });

    std::vector<kage::sync::SyncTraceEvent> events;
    kage::sync::SyncTracer tracer;
    tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { events.push_back(event); }});
    client.set_tracer(&tracer);

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));

    REQUIRE(std::any_of(events.begin(), events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::CueRolledBack &&
            event.data.find("rollback_reason=server_mismatch") != std::string::npos;
    }));
}

TEST_CASE("predicted cue rollback tracing records resim omission reason") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    registry.register_component<CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<TestCue>(registry);
    kage::sync::configure_client(registry, 1);

    int prediction_jobs = 0;
    bool emit_during_resim = false;
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    options.rollback_policy = kage::sync::ReplicationRollbackPolicy::All;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition, kage::sync::SyncSettings>(registry, 0).each(
        [&](ecs::Entity entity, PredictedPosition& position, kage::sync::SyncSettings& settings) {
            position.x += 1.0f;
            ++prediction_jobs;
            if (prediction_jobs == 2 || emit_during_resim) {
                REQUIRE(kage::sync::emit_cue(settings, entity, TestCue{9}, 1.0f));
            }
        });

    std::vector<kage::sync::SyncTraceEvent> events;
    kage::sync::SyncTracer tracer;
    tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { events.push_back(event); }});
    client.set_tracer(&tracer);

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));

    REQUIRE(std::any_of(events.begin(), events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::CueRolledBack &&
            event.data.find("rollback_reason=resim_not_replayed") != std::string::npos;
    }));
}
#endif

TEST_CASE("predicted client keeps local prediction phase when authoritative frames arrive early") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f}, 1)));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);

    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds * 0.5));
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{1.0f, 0.0f}, 2)));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds * 0.5));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
}

TEST_CASE("predicted client ONLY_AFFECTED resim runs jobs for affected entities") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);
    int calls = 0;

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    options.rollback_policy = kage::sync::ReplicationRollbackPolicy::OnlyAffected;
    kage::sync::ReplicationClient client(options);
    client.simulation_job<PredictedPosition>(registry, 0).each([&](ecs::Entity, PredictedPosition& position) {
        ++calls;
        position.x += 1.0f;
    });

    const ecs::Entity first_server = registry.create();
    const ecs::Entity second_server = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, first_server, PredictedPosition{0.0f, 0.0f}, 1)));
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, second_server, PredictedPosition{0.0f, 0.0f}, 2)));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(calls == 4);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, first_server, PredictedPosition{2.0f, 0.0f}, 3)));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, second_server, PredictedPosition{1.0f, 0.0f}, 4)));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));

    REQUIRE(calls == 7);
    REQUIRE(registry.get<PredictedPosition>(client.local_entity(test_client_entity_network_id(1, first_server))).x == 4.0f);
    REQUIRE(registry.get<PredictedPosition>(client.local_entity(test_client_entity_network_id(1, second_server))).x == 3.0f);
}

TEST_CASE("predicted client cosmetic jobs do not run during resimulation") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    int simulation_calls = 0;
    int cosmetic_calls = 0;

    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    options.rollback_policy = kage::sync::ReplicationRollbackPolicy::All;
    kage::sync::ReplicationClient client(options);

    client.simulation_job<PredictedPosition>(registry, 0).each([&](ecs::Entity, PredictedPosition& position) {
        ++simulation_calls;
        position.x += 1.0f;
    });
    const ecs::Entity cosmetic_job =
        client.cosmetic_job<PredictedPosition>(registry, 1).each([&](ecs::Entity, PredictedPosition&) {
            ++cosmetic_calls;
        });
    REQUIRE(registry.has<kage::sync::NoResim>(cosmetic_job));

    const ecs::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));
    REQUIRE(simulation_calls == 2);
    REQUIRE(cosmetic_calls == 2);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));

    REQUIRE(simulation_calls == 4);
    REQUIRE(cosmetic_calls == 3);
}

TEST_CASE("client simulation jobs skip NoSimulate entities") {
    ecs::Registry registry;
    registry.register_component<PredictedPosition>("PredictedPosition");
    kage::sync::configure_client(registry, 1);

    const ecs::Entity simulated = registry.create();
    const ecs::Entity skipped = registry.create();
    REQUIRE(registry.add<PredictedPosition>(simulated, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<PredictedPosition>(skipped, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<kage::sync::NoSimulate>(skipped));

    kage::sync::ReplicationClient client;
    client.simulation_job<PredictedPosition>(registry, 0).each([](ecs::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    registry.run_jobs();

    REQUIRE(registry.get<PredictedPosition>(simulated).x == 1.0f);
    REQUIRE(registry.get<PredictedPosition>(skipped).x == 0.0f);
}

TEST_CASE("predicted client applies authoritative destroys immediately") {
    ecs::Registry registry;
    const ecs::Entity position_component =
        kage::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const kage::sync::SyncArchetypeId archetype = kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    kage::sync::configure_client(registry, 1);

    const ecs::Entity server_entity = registry.create();
    kage::sync::ReplicationClientOptions options;
    options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(options);

    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ecs::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(registry.alive(local));

    REQUIRE(client.receive(registry, make_destroy_packet(2, server_entity)));
    REQUIRE_FALSE(registry.alive(local));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, server_entity)));
}
