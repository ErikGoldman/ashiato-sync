#include "test_protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace ashiato_sync_tests;

TEST_CASE("predicted client mode requires ShouldRollBack traits") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = ashiato_sync_tests::define_position_archetype(server_registry);
    const ashiato::Entity server_entity = server_registry.create();

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = ashiato_sync_tests::define_position_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));

    try {
        (void)client.receive(client_registry, make_position_packet(1, {{server_entity, Position{1.0f, 2.0f}}}));
        FAIL("expected missing prediction rollback trait error");
    } catch (const ashiato::sync::ClientError& error) {
        REQUIRE(error.status() == ashiato::sync::ClientStatus::MissingPredictionRollbackTrait);
    }
}

TEST_CASE("predicted client snaps first frame predicts locally and skips matching rollback") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition>(registry, 0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);

    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{1.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(registry.get<PredictedPosition>(local).x == 2.0f);
}

TEST_CASE("predicted client jobs use quantized input samples") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    ashiato::sync::register_sync_component<NetworkedPosition>(registry, "NetworkedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<NetworkedPosition>(registry));

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition, const NetworkedPosition>(registry, 0).each(
        [](ashiato::Entity, PredictedPosition& position, const NetworkedPosition& input) {
            position.x += input.x;
            position.y += input.y;
        });

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(ashiato::sync::set_owner(registry, local, 1));

    REQUIRE(client.set_input(registry, NetworkedPosition{1.29f, 2.39f}));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));

    REQUIRE(registry.get<PredictedPosition>(local).x == Catch::Approx(1.2f));
    REQUIRE(registry.get<PredictedPosition>(local).y == Catch::Approx(2.3f));
}

TEST_CASE("predicted client rolls back and resimulates mismatched frames") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    options.prediction.rollback_policy = ashiato::sync::ReplicationRollbackPolicy::All;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition>(registry, 0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(registry.get<PredictedPosition>(local).x == 3.0f);
}

#ifdef ASHIATO_SYNC_ENABLE_TRACING

TEST_CASE("predicted client starts bundled authoritative rollback from latest received baseline") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    options.prediction.rollback_policy = ashiato::sync::ReplicationRollbackPolicy::All;
    options.prediction.auto_lead_frames = false;
    options.prediction.lead_frames = 0;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition>(registry, 0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    std::vector<ashiato::sync::SyncTraceEvent> events;
    ashiato::sync::SyncTracer tracer;
    tracer.set_frame_data_enabled(true);
    tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { events.push_back(event); }});
    client.set_tracer(&tracer);

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);

    tick_client_fixed_frames(client, registry, 3);
    REQUIRE(registry.get<PredictedPosition>(local).x == 3.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f}, 2)));
    REQUIRE(client.receive(registry, make_predicted_position_packet(3, server_entity, PredictedPosition{3.0f, 0.0f}, 3)));
    tick_client_fixed_frames(client, registry, 1);

    std::vector<ashiato::sync::SyncFrame> resim_frames;
    for (const ashiato::sync::SyncTraceEvent& event : events) {
        if (event.type == ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent &&
            event.component == position_component) {
            resim_frames.push_back(event.frame);
        }
    }

    REQUIRE(std::find(resim_frames.begin(), resim_frames.end(), 3) == resim_frames.end());
    REQUIRE(std::count(resim_frames.begin(), resim_frames.end(), 4) == 1);
    REQUIRE(registry.get<PredictedPosition>(local).x == 5.0f);
}

#endif

TEST_CASE("predicted client rolls back locally predicted cues missing from authoritative frame") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(registry);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    bool emit_prediction_cue = true;
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    options.prediction.rollback_policy = ashiato::sync::ReplicationRollbackPolicy::All;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<
        PredictedPosition,
        ashiato::sync::SyncSettings,
        ashiato::sync::FrameInfo,
        ashiato::sync::CueDispatcher>(registry, 0).each(
        [&](ashiato::Entity entity,
            PredictedPosition& position,
            ashiato::sync::SyncSettings& settings,
            ashiato::sync::FrameInfo& frame,
            ashiato::sync::CueDispatcher& cues) {
            position.x += 1.0f;
            if (emit_prediction_cue) {
                REQUIRE(cues.emit(settings, frame, entity, TestCue{7}, 1.0f));
                emit_prediction_cue = false;
            }
        });

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(registry.get<CuePlayback>(local).plays == 1);
    REQUIRE(registry.get<CuePlayback>(local).rollbacks == 0);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(registry.get<CuePlayback>(local).rollbacks == 1);
}

TEST_CASE("predicted client keeps locally predicted cues replayed during resimulation") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(registry);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    options.prediction.rollback_policy = ashiato::sync::ReplicationRollbackPolicy::All;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    int prediction_jobs = 0;
    client.simulation_job<
        PredictedPosition,
        ashiato::sync::SyncSettings,
        ashiato::sync::FrameInfo,
        ashiato::sync::CueDispatcher>(registry, 0).each(
        [&](ashiato::Entity entity,
            PredictedPosition& position,
            ashiato::sync::SyncSettings& settings,
            ashiato::sync::FrameInfo& frame,
            ashiato::sync::CueDispatcher& cues) {
            position.x += 1.0f;
            ++prediction_jobs;
            if (prediction_jobs == 2 || prediction_jobs == 3) {
                REQUIRE(cues.emit(settings, frame, entity, TestCue{5}, 1.0f));
            }
        });

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE_FALSE(registry.contains<CuePlayback>(local));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(registry.get<CuePlayback>(local).plays == 1);
    REQUIRE(registry.get<CuePlayback>(local).rollbacks == 0);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(registry.get<CuePlayback>(local).rollbacks == 0);
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(registry.get<CuePlayback>(local).rollbacks == 0);
}

#ifdef ASHIATO_SYNC_ENABLE_TRACING

TEST_CASE("predicted client traces rollback reason separately from rollback conflict") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition>(registry, 0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    std::vector<ashiato::sync::SyncTraceEvent> events;
    ashiato::sync::SyncTracer tracer;
    tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { events.push_back(event); }});
    client.set_tracer(&tracer);

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));

    const auto conflict = std::find_if(events.begin(), events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::PredictionRollbackConflict;
    });
    REQUIRE(conflict != events.end());
    REQUIRE(conflict->component == position_component);
    REQUIRE(conflict->component_name == "PredictedPosition");
    REQUIRE(conflict->data.empty());

    const auto reason = std::find_if(events.begin(), events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::RollbackReason;
    });
    REQUIRE(reason != events.end());
    REQUIRE(std::count_if(events.begin(), events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::RollbackReason;
    }) == 1);
    REQUIRE(reason->component == position_component);
    REQUIRE(reason->component_name == "PredictedPosition");
    REQUIRE(reason->data == "PredictedPosition.x mismatch");
}

TEST_CASE("predicted cue rollback tracing records server mismatch reason") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(registry);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    bool emit_prediction_cue = true;
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<
        PredictedPosition,
        ashiato::sync::SyncSettings,
        ashiato::sync::FrameInfo,
        ashiato::sync::CueDispatcher>(registry, 0).each(
        [&](ashiato::Entity entity,
            PredictedPosition& position,
            ashiato::sync::SyncSettings& settings,
            ashiato::sync::FrameInfo& frame,
            ashiato::sync::CueDispatcher& cues) {
            position.x += 1.0f;
            if (emit_prediction_cue) {
                REQUIRE(cues.emit(settings, frame, entity, TestCue{7}, 1.0f));
                emit_prediction_cue = false;
            }
        });

    std::vector<ashiato::sync::SyncTraceEvent> events;
    ashiato::sync::SyncTracer tracer;
    tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { events.push_back(event); }});
    client.set_tracer(&tracer);

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));

    REQUIRE(std::any_of(events.begin(), events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::CueRolledBack &&
            event.data.find("rollback_reason=server_mismatch") != std::string::npos;
    }));
}

TEST_CASE("predicted cue rollback tracing records resim omission reason") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    registry.register_component<CuePlayback>("CuePlayback");
    ashiato::sync::register_sync_cue<TestCue>(registry);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    int prediction_jobs = 0;
    bool emit_during_resim = false;
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    options.prediction.rollback_policy = ashiato::sync::ReplicationRollbackPolicy::All;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<
        PredictedPosition,
        ashiato::sync::SyncSettings,
        ashiato::sync::FrameInfo,
        ashiato::sync::CueDispatcher>(registry, 0).each(
        [&](ashiato::Entity entity,
            PredictedPosition& position,
            ashiato::sync::SyncSettings& settings,
            ashiato::sync::FrameInfo& frame,
            ashiato::sync::CueDispatcher& cues) {
            position.x += 1.0f;
            ++prediction_jobs;
            if (prediction_jobs == 2 || emit_during_resim) {
                REQUIRE(cues.emit(settings, frame, entity, TestCue{9}, 1.0f));
            }
        });

    std::vector<ashiato::sync::SyncTraceEvent> events;
    ashiato::sync::SyncTracer tracer;
    tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { events.push_back(event); }});
    client.set_tracer(&tracer);

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));

    REQUIRE(std::any_of(events.begin(), events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::CueRolledBack &&
            event.data.find("rollback_reason=resim_not_replayed") != std::string::npos;
    }));
}
#endif

TEST_CASE("predicted client keeps local prediction phase when authoritative frames arrive early") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition>(registry, 0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f}, 1)));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);

    REQUIRE(client.tick(registry, client.fixed_dt_seconds() * 0.5));
    REQUIRE(registry.get<PredictedPosition>(local).x == 0.0f);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{1.0f, 0.0f}, 2)));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds() * 0.5));
    REQUIRE(registry.get<PredictedPosition>(local).x == 1.0f);
}

TEST_CASE("predicted client ONLY_AFFECTED resim runs jobs for affected entities") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    int calls = 0;

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    options.prediction.rollback_policy = ashiato::sync::ReplicationRollbackPolicy::OnlyAffected;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));
    client.simulation_job<PredictedPosition>(registry, 0).each([&](ashiato::Entity, PredictedPosition& position) {
        ++calls;
        position.x += 1.0f;
    });

    const ashiato::Entity first_server = registry.create();
    const ashiato::Entity second_server = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, first_server, PredictedPosition{0.0f, 0.0f}, 1)));
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, second_server, PredictedPosition{0.0f, 0.0f}, 2)));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(calls == 4);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, first_server, PredictedPosition{2.0f, 0.0f}, 3)));
    REQUIRE(client.receive(registry, make_predicted_position_packet(2, second_server, PredictedPosition{1.0f, 0.0f}, 4)));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));

    REQUIRE(calls == 7);
    REQUIRE(registry.get<PredictedPosition>(client.local_entity(test_client_entity_network_id(1, first_server))).x == 4.0f);
    REQUIRE(registry.get<PredictedPosition>(client.local_entity(test_client_entity_network_id(1, second_server))).x == 3.0f);
}

TEST_CASE("predicted client cosmetic jobs do not run during resimulation") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    int simulation_calls = 0;
    int cosmetic_calls = 0;

    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    options.prediction.rollback_policy = ashiato::sync::ReplicationRollbackPolicy::All;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));

    client.simulation_job<PredictedPosition>(registry, 0).each([&](ashiato::Entity, PredictedPosition& position) {
        ++simulation_calls;
        position.x += 1.0f;
    });
    const ashiato::Entity cosmetic_job =
        client.cosmetic_job<PredictedPosition>(registry, 1).each([&](ashiato::Entity, PredictedPosition&) {
            ++cosmetic_calls;
        });
    REQUIRE(registry.has<ashiato::sync::NoResim>(cosmetic_job));

    const ashiato::Entity server_entity = registry.create();
    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));
    REQUIRE(simulation_calls == 2);
    REQUIRE(cosmetic_calls == 2);

    REQUIRE(client.receive(registry, make_predicted_position_packet(2, server_entity, PredictedPosition{2.0f, 0.0f})));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));

    REQUIRE(simulation_calls == 4);
    REQUIRE(cosmetic_calls == 3);
}

TEST_CASE("client simulation jobs skip NoSimulate entities") {
    ashiato::Registry registry;
    registry.register_component<PredictedPosition>("PredictedPosition");
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    const ashiato::Entity simulated = registry.create();
    const ashiato::Entity skipped = registry.create();
    REQUIRE(registry.add<PredictedPosition>(simulated, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<PredictedPosition>(skipped, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(registry.add<ashiato::sync::NoSimulate>(skipped));

    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, {}));
    client.simulation_job<PredictedPosition>(registry, 0).each([](ashiato::Entity, PredictedPosition& position) {
        position.x += 1.0f;
    });

    registry.run_jobs();

    REQUIRE(registry.get<PredictedPosition>(simulated).x == 1.0f);
    REQUIRE(registry.get<PredictedPosition>(skipped).x == 0.0f);
}

TEST_CASE("role-tagged simulation jobs register endpoint-specific job roles") {
    ashiato::Registry client_registry;
    client_registry.register_component<PredictedPosition>("PredictedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);

    const ashiato::Entity simulated = client_registry.create();
    const ashiato::Entity skipped = client_registry.create();
    REQUIRE(client_registry.add<PredictedPosition>(simulated, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(client_registry.add<PredictedPosition>(skipped, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(client_registry.add<ashiato::sync::NoSimulate>(skipped));

    ashiato::sync::SimulationJobs client_jobs(
        client_registry,
        ashiato::sync::SimulationJobEndpointRole::DedicatedClient);
    const ashiato::Entity shared_client_job =
        client_jobs.shared<PredictedPosition>(0).each([](ashiato::Entity, PredictedPosition& position) {
            position.x += 1.0f;
        });
    const ashiato::Entity cosmetic_client_job =
        client_jobs.client_only<PredictedPosition>(1).each([](ashiato::Entity, PredictedPosition& position) {
            position.y += 1.0f;
        });
    const ashiato::Entity server_only_client_job =
        client_jobs.server_only<PredictedPosition>(2).each([](ashiato::Entity, PredictedPosition& position) {
            position.x += 100.0f;
        });

    REQUIRE(client_registry.has<ashiato::sync::PredictiveSimulationJob>(shared_client_job));
    REQUIRE(client_registry.has<ashiato::sync::NoResim>(cosmetic_client_job));
    REQUIRE_FALSE(server_only_client_job);

    client_registry.run_jobs();
    REQUIRE(client_registry.get<PredictedPosition>(simulated).x == 1.0f);
    REQUIRE(client_registry.get<PredictedPosition>(skipped).x == 0.0f);
    REQUIRE(client_registry.get<PredictedPosition>(simulated).y == 1.0f);
    REQUIRE(client_registry.get<PredictedPosition>(skipped).y == 1.0f);

    ashiato::Registry server_registry;
    server_registry.register_component<PredictedPosition>("PredictedPosition");
    ashiato::sync::SimulationJobs server_jobs(
        server_registry,
        ashiato::sync::SimulationJobEndpointRole::DedicatedServer);
    const ashiato::Entity shared_server_job =
        server_jobs.shared<PredictedPosition>(0).each([](ashiato::Entity, PredictedPosition&) {});
    const ashiato::Entity client_only_server_job =
        server_jobs.client_only<PredictedPosition>(1).each([](ashiato::Entity, PredictedPosition&) {});
    const ashiato::Entity server_only_server_job =
        server_jobs.server_only<PredictedPosition>(2).each([](ashiato::Entity, PredictedPosition&) {});

    REQUIRE(shared_server_job);
    server_registry.register_component<ashiato::sync::PredictiveSimulationJob>(
        "ashiato.sync.PredictiveSimulationJob");
    REQUIRE_FALSE(server_registry.has<ashiato::sync::PredictiveSimulationJob>(shared_server_job));
    REQUIRE_FALSE(client_only_server_job);
    REQUIRE(server_only_server_job);

    ashiato::Registry listen_registry;
    listen_registry.register_component<PredictedPosition>("PredictedPosition");
    ashiato::sync::SimulationJobs listen_jobs(
        listen_registry,
        ashiato::sync::SimulationJobEndpointRole::ListenServer);
    const ashiato::Entity shared_listen_job =
        listen_jobs.shared<PredictedPosition>(0).each([](ashiato::Entity, PredictedPosition&) {});
    const ashiato::Entity client_only_listen_job =
        listen_jobs.client_only<PredictedPosition>(1).each([](ashiato::Entity, PredictedPosition&) {});
    const ashiato::Entity server_only_listen_job =
        listen_jobs.server_only<PredictedPosition>(2).each([](ashiato::Entity, PredictedPosition&) {});

    REQUIRE(shared_listen_job);
    listen_registry.register_component<ashiato::sync::PredictiveSimulationJob>(
        "ashiato.sync.PredictiveSimulationJob");
    REQUIRE_FALSE(listen_registry.has<ashiato::sync::PredictiveSimulationJob>(shared_listen_job));
    REQUIRE(client_only_listen_job);
    REQUIRE(listen_registry.has<ashiato::sync::NoResim>(client_only_listen_job));
    REQUIRE(server_only_listen_job);
}

TEST_CASE("role-tagged simulation jobs support advanced builder modifiers") {
    ashiato::Registry client_registry;
    client_registry.register_component<PredictedPosition>("PredictedPosition");
    client_registry.register_component<NetworkedPosition>("NetworkedPosition");
    ashiato::sync::SimulationJobs client_jobs(
        client_registry,
        ashiato::sync::SimulationJobEndpointRole::DedicatedClient);

    const ashiato::Entity optional_source = client_registry.create();
    const ashiato::Entity no_optional = client_registry.create();
    const ashiato::Entity no_simulate = client_registry.create();
    const ashiato::Entity untagged = client_registry.create();
    REQUIRE(client_registry.add<PredictedPosition>(optional_source, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(client_registry.add<PredictedPosition>(no_optional, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(client_registry.add<PredictedPosition>(no_simulate, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(client_registry.add<PredictedPosition>(untagged, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(client_registry.add<NetworkedPosition>(optional_source, NetworkedPosition{3.0f, 4.0f}) != nullptr);
    REQUIRE(client_registry.add<ashiato::sync::NoResim>(optional_source));
    REQUIRE(client_registry.add<ashiato::sync::NoResim>(no_optional));
    REQUIRE(client_registry.add<ashiato::sync::NoResim>(no_simulate));
    REQUIRE(client_registry.add<ashiato::sync::NoSimulate>(no_simulate));

    int optional_calls = 0;
    const ashiato::Entity optional_job = client_jobs.shared<PredictedPosition>(0)
        .name("advanced shared optional")
        .single_thread()
        .min_entities_per_thread(1)
        .optional<NetworkedPosition>()
        .with_tags<const ashiato::sync::NoResim>()
        .without_tags<const ashiato::sync::NoSimulate>()
        .each([&](
            auto& view,
            const ashiato::sync::JobRunContext& context,
            ashiato::Entity,
            PredictedPosition& position) {
            REQUIRE(context.is_predicting_client());
            ++optional_calls;
            if (const NetworkedPosition* networked = view.template try_get<NetworkedPosition>()) {
                position.x += networked->x;
                position.y += networked->y;
            } else {
                position.x += 1.0f;
            }
        });
    REQUIRE(client_registry.has<ashiato::sync::PredictiveSimulationJob>(optional_job));

    client_registry.run_jobs();
    REQUIRE(optional_calls == 2);
    REQUIRE(client_registry.get<PredictedPosition>(optional_source).x == 3.0f);
    REQUIRE(client_registry.get<PredictedPosition>(optional_source).y == 4.0f);
    REQUIRE(client_registry.get<PredictedPosition>(no_optional).x == 1.0f);
    REQUIRE(client_registry.get<PredictedPosition>(no_simulate).x == 0.0f);
    REQUIRE(client_registry.get<PredictedPosition>(untagged).x == 0.0f);

    ashiato::Registry server_registry;
    server_registry.register_component<PredictedPosition>("PredictedPosition");
    server_registry.register_component<NetworkedPosition>("NetworkedPosition");
    ashiato::sync::SimulationJobs server_jobs(
        server_registry,
        ashiato::sync::SimulationJobEndpointRole::DedicatedServer);

    const ashiato::Entity accessed = server_registry.create();
    const ashiato::Entity target = server_registry.create();
    REQUIRE(server_registry.add<PredictedPosition>(accessed, PredictedPosition{0.0f, 0.0f}) != nullptr);
    REQUIRE(server_registry.add<NetworkedPosition>(target, NetworkedPosition{5.0f, 0.0f}) != nullptr);

    server_jobs.shared<PredictedPosition>(0)
        .access_other_entities<const NetworkedPosition>()
        .each([&](
            auto& view,
            const ashiato::sync::JobRunContext& context,
            ashiato::Entity,
            PredictedPosition& position) {
            REQUIRE(context.is_authority());
            position.x += view.template get<const NetworkedPosition>(target).x;
        });

    server_jobs.server_only<PredictedPosition>(1)
        .max_threads(2)
        .structural<ashiato::sync::NoSimulate>()
        .each([](
            auto& structural,
            const ashiato::sync::JobRunContext& context,
            ashiato::Entity entity,
            PredictedPosition& position) {
            REQUIRE(context.is_server_only());
            position.y += 2.0f;
            REQUIRE(structural.template add<ashiato::sync::NoSimulate>(entity));
        });

    server_registry.run_jobs();
    REQUIRE(server_registry.get<PredictedPosition>(accessed).x == 5.0f);
    REQUIRE(server_registry.get<PredictedPosition>(accessed).y == 2.0f);
    REQUIRE(server_registry.has<ashiato::sync::NoSimulate>(accessed));
}

TEST_CASE("predicted client applies authoritative destroys immediately") {
    ashiato::Registry registry;
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<PredictedPosition>(registry, "PredictedPosition");
    const ashiato::sync::SyncArchetypeId archetype = ashiato::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
    REQUIRE(archetype.value == 0);
    ashiato_sync_tests::configure_test_client_registry(registry, 1);

    const ashiato::Entity server_entity = registry.create();
    ashiato::sync::ReplicationClientOptions options;
    options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, options));

    REQUIRE(client.receive(registry, make_predicted_position_packet(1, server_entity, PredictedPosition{0.0f, 0.0f})));
    const ashiato::Entity local = client.local_entity(test_client_entity_network_id(1, server_entity));
    REQUIRE(local);
    REQUIRE(registry.alive(local));

    REQUIRE(client.receive(registry, make_destroy_packet(2, server_entity)));
    REQUIRE_FALSE(registry.alive(local));
    REQUIRE_FALSE(client.local_entity(test_client_entity_network_id(1, server_entity)));
}
