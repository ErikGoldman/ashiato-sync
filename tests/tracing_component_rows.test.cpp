#include "test_tracing_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#ifdef ASHIATO_SYNC_ENABLE_TRACING

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace ashiato_sync_tests;

TEST_CASE("ktrace reader groups server input events with server entity rows and visible cells") {
    const std::string directory = "/tmp/ashiato_sync_server_input_grouping_test";
    std::filesystem::remove_all(directory);

    const ashiato::Entity server_entity{42};
    const ashiato::Entity input_component{7};
    const ashiato::sync::ClientId client = 1;
    const std::uint32_t wire_id = 3;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId client_network_id =
        ashiato::sync::make_client_entity_network_id(client, wire_id, version);

    ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

    ashiato::sync::SyncTraceEvent transform;
    transform.type = ashiato::sync::SyncTraceEventType::FrameComponent;
    transform.role = ashiato::sync::SyncTraceRole::Server;
    transform.frame = 10;
    transform.server_entity = server_entity;
    transform.component = ashiato::Entity{5};
    writer.tracer().trace(transform);

    ashiato::sync::SyncTraceEvent input;
    input.type = ashiato::sync::SyncTraceEventType::FrameComponent;
    input.role = ashiato::sync::SyncTraceRole::Server;
    input.client = client;
    input.frame = 10;
    input.server_entity = server_entity;
    input.client_network_id = client_network_id;
    input.wire_network_id = wire_id;
    input.network_version = version;
    input.component = input_component;
    input.data = "x=50,y=60";
    writer.tracer().trace(input);

    ashiato::sync::SyncTraceEvent starved = input;
    starved.type = ashiato::sync::SyncTraceEventType::InputStarved;
    starved.data = "input_frame=8";
    writer.tracer().trace(starved);
    writer.close();

    const ashiato::sync::SyncTraceHistory history = ashiato::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);
    REQUIRE(history.sources[0].entities[0].server_entity == server_entity);
    REQUIRE(history.sources[0].entities[0].components.size() == 2);

    const auto input_row = std::find_if(
        history.sources[0].entities[0].components.begin(),
        history.sources[0].entities[0].components.end(),
        [input_component](const ashiato::sync::KTraceComponentRow& row) {
            return row.component == input_component;
        });
    REQUIRE(input_row != history.sources[0].entities[0].components.end());
    REQUIRE(input_row->runs[0].frames.size() == 1);
    REQUIRE(input_row->runs[0].frames[0].frame == 10);
    REQUIRE((input_row->runs[0].frames[0].state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::InputReceived)) != 0U);
    REQUIRE((input_row->runs[0].frames[0].state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Starved)) != 0U);
    REQUIRE(input_row->runs[0].frames[0].event_indices.size() == 2);
    REQUIRE(history.sources[0].records[input_row->runs[0].frames[0].event_indices[0]].event.data == "x=50,y=60");
    REQUIRE(history.sources[0].records[input_row->runs[0].frames[0].event_indices[1]].event.data == "input_frame=8");
}

TEST_CASE("ktrace reader groups cue lifecycle events on cue rows") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_cue_rows_test";
    std::filesystem::remove_all(directory);

    const ashiato::Entity server_entity{42};
    const ashiato::sync::ClientId client = 1;
    const std::uint32_t wire_id = 3;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId client_network_id =
        ashiato::sync::make_client_entity_network_id(client, wire_id, version);
    constexpr ashiato::sync::SyncCueTypeId cue_type = 5;

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});
        const std::array<ashiato::sync::SyncTraceEventType, 6> types{{
            ashiato::sync::SyncTraceEventType::CueEmitted,
            ashiato::sync::SyncTraceEventType::CueSent,
            ashiato::sync::SyncTraceEventType::CueReceived,
            ashiato::sync::SyncTraceEventType::CuePlayed,
            ashiato::sync::SyncTraceEventType::CueConfirmed,
            ashiato::sync::SyncTraceEventType::CueRolledBack,
        }};
        for (ashiato::sync::SyncTraceEventType type : types) {
            ashiato::sync::SyncTraceEvent event;
            event.type = type;
            event.role = ashiato::sync::SyncTraceRole::Client;
            event.client = client;
            event.frame = 10;
            event.server_entity = server_entity;
            event.local_entity = ashiato::Entity{123};
            event.client_network_id = client_network_id;
            event.wire_network_id = wire_id;
            event.network_version = version;
            event.cue_type = cue_type;
            event.component_name = "TestCue";
            writer.tracer().trace(event);
        }
        writer.close();
    }

    const ashiato::sync::SyncTraceHistory history = ashiato::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);
    REQUIRE(history.sources[0].entities[0].server_entity == server_entity);
    REQUIRE(history.sources[0].entities[0].components.size() == 1);
    REQUIRE(history.sources[0].cue_names.at(cue_type) == "TestCue");
    REQUIRE(std::count_if(
        history.sources[0].records.begin(),
        history.sources[0].records.end(),
        [](const ashiato::sync::KTraceRecord& record) {
            return record.event.type == ashiato::sync::SyncTraceEventType::CueName;
        }) == 1);
    REQUIRE(std::none_of(
        history.sources[0].records.begin(),
        history.sources[0].records.end(),
        [](const ashiato::sync::KTraceRecord& record) {
            return record.event.type != ashiato::sync::SyncTraceEventType::CueName &&
                !record.event.component_name.empty();
        }));

    const ashiato::sync::KTraceComponentRow& cue_row = history.sources[0].entities[0].components[0];
    REQUIRE(cue_row.component.value == (std::uint64_t{1} << 63U));
    REQUIRE(cue_row.runs[0].frames.size() == 1);
    REQUIRE(cue_row.runs[0].frames[0].frame == 10);
    REQUIRE(cue_row.runs[0].frames[0].event_indices.size() == 6);
    const std::uint32_t mask = cue_row.runs[0].frames[0].state_mask;
    REQUIRE((mask & static_cast<std::uint32_t>(ashiato::sync::KTraceCellState::CueEmitted)) != 0U);
    REQUIRE((mask & static_cast<std::uint32_t>(ashiato::sync::KTraceCellState::CueSent)) != 0U);
    REQUIRE((mask & static_cast<std::uint32_t>(ashiato::sync::KTraceCellState::CueReceived)) != 0U);
    REQUIRE((mask & static_cast<std::uint32_t>(ashiato::sync::KTraceCellState::CuePlayed)) != 0U);
    REQUIRE((mask & static_cast<std::uint32_t>(ashiato::sync::KTraceCellState::CueConfirmed)) != 0U);
    REQUIRE((mask & static_cast<std::uint32_t>(ashiato::sync::KTraceCellState::CueRolledBack)) != 0U);
}

TEST_CASE("ktrace reader starts a new run for a later conflict that resimulates") {
    const std::string directory = "/tmp/ashiato_sync_later_rollback_run_test";
    std::filesystem::remove_all(directory);

    const ashiato::Entity component{7};
    const ashiato::sync::ClientId client = 1;
    const std::uint32_t wire_id = 3;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId client_network_id =
        ashiato::sync::make_client_entity_network_id(client, wire_id, version);

    auto make_event = [&](ashiato::sync::SyncTraceEventType type, ashiato::sync::SyncFrame frame) {
        ashiato::sync::SyncTraceEvent event;
        event.type = type;
        event.role = ashiato::sync::SyncTraceRole::Client;
        event.client = client;
        event.frame = frame;
        event.local_entity = ashiato::Entity{123};
        event.client_network_id = client_network_id;
        event.wire_network_id = wire_id;
        event.network_version = version;
        event.component = component;
        event.component_name = "PredictedTransform";
        event.mode = ashiato::sync::ReplicationClientMode::Predict;
        return event;
    };

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::PredictionRollbackConflict, 10));
        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent, 11));

        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::FrameComponent, 100));
        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::ComponentReceived, 100));
        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::RollbackReason, 100));
        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::PredictionRollbackConflict, 100));
        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent, 101));
        writer.close();
    }

    const ashiato::sync::SyncTraceHistory history = ashiato::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);

    const ashiato::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    const ashiato::sync::KTraceComponentRow& component_row = entity.components[0];
    REQUIRE(component_row.runs.size() == 3);
    REQUIRE(component_row.runs[1].start_frame == 10);
    REQUIRE(component_row.runs[2].start_frame == 100);
    const ashiato::sync::KTraceFrameCell* conflict_cell = ashiato_sync_tests::find_trace_cell(component_row, 100);
    REQUIRE(conflict_cell != nullptr);
    REQUIRE((conflict_cell->state_mask &
        static_cast<std::uint32_t>(ashiato::sync::KTraceCellState::Mispredicted)) != 0U);
}

TEST_CASE("client frame component tracing records once per fixed local frame") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_networked_archetype(server_registry);
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::NetworkedPosition>(
        server_entity,
        ashiato_sync_tests::NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_networked_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    std::vector<ashiato::sync::SyncTraceEvent> client_events;
    ashiato::sync::SyncTracer client_tracer;
    client_tracer.set_frame_data_enabled(true);
    client_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { client_events.push_back(event); }});
    client.set_tracer(&client_tracer);

    REQUIRE(client.receive(client_registry, packets[0]));
    client_events.clear();

    const double substep = client.fixed_dt_seconds() * 0.25;
    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(std::none_of(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent;
    }));

    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent && event.frame == 1;
    }) == 1);

    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent && event.frame == 1;
    }) == 1);

    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent && event.frame == 2;
    }) == 1);

    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds() * 2.0));
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent && event.frame == 3;
    }) == 1);
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent && event.frame == 4;
    }) == 1);
}

TEST_CASE("predicted client frame component tracing records predicted mode once") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_predicted_archetype(server_registry);
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::PredictedPosition>(
        server_entity,
        ashiato_sync_tests::PredictedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);

    ashiato::Registry client_registry;
    const ashiato::sync::SyncArchetypeId client_archetype = define_predicted_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    ashiato::sync::ReplicationClientOptions client_options;
    client_options.entities.default_mode = ashiato::sync::ReplicationClientMode::Predict;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, client_options));
    client.simulation_job<ashiato_sync_tests::PredictedPosition>(client_registry, 0).each(
        [](ashiato::Entity, ashiato_sync_tests::PredictedPosition& position) {
            position.x += 1.0f;
        });

    std::vector<ashiato::sync::SyncTraceEvent> client_events;
    ashiato::sync::SyncTracer client_tracer;
    client_tracer.set_frame_data_enabled(true);
    client_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { client_events.push_back(event); }});
    client.set_tracer(&client_tracer);

    REQUIRE(client.receive(client_registry, packets[0]));
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::ComponentReceived &&
            event.mode == ashiato::sync::ReplicationClientMode::Predict;
    }) == 1);
    REQUIRE(std::none_of(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::ComponentReceived &&
            event.mode == ashiato::sync::ReplicationClientMode::Snap;
    }));
    REQUIRE_FALSE(has_event(client_events, ashiato::sync::SyncTraceEventType::ComponentApplied));
    client_events.clear();
    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));

    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent &&
            event.frame == 2 &&
            event.mode == ashiato::sync::ReplicationClientMode::Predict;
    }) == 1);
    REQUIRE(std::none_of(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent &&
            event.mode == ashiato::sync::ReplicationClientMode::Snap;
    }));
}

TEST_CASE("ktrace reader groups resimulated component rows under rollback trigger component") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_branch_trigger_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 3;
    const std::uint32_t wire_id = 5;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{321};
    const ashiato::Entity trigger_component{42};
    const ashiato::Entity resimulated_component{77};

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent conflict;
        conflict.type = ashiato::sync::SyncTraceEventType::PredictionRollbackConflict;
        conflict.role = ashiato::sync::SyncTraceRole::Client;
        conflict.client = client_id;
        conflict.frame = 10;
        conflict.server_entity = ashiato::Entity{network_id};
        conflict.local_entity = local_entity;
        conflict.client_network_id = network_id;
        conflict.wire_network_id = wire_id;
        conflict.network_version = version;
        conflict.component = trigger_component;
        conflict.mode = ashiato::sync::ReplicationClientMode::Predict;
        writer.tracer().trace(conflict);

        ashiato::sync::SyncTraceEvent resimulated = conflict;
        resimulated.type = ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent;
        resimulated.frame = 11;
        resimulated.component = resimulated_component;
        writer.tracer().trace(resimulated);
        writer.close();
    }

    const ashiato::sync::KTraceReader reader;
    const ashiato::sync::SyncTraceHistory history = reader.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);

    const ashiato::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    const auto resimulated_row = std::find_if(
        entity.components.begin(),
        entity.components.end(),
        [resimulated_component](const ashiato::sync::KTraceComponentRow& row) {
            return row.component == resimulated_component;
        });
    REQUIRE(resimulated_row != entity.components.end());
    REQUIRE(resimulated_row->runs.size() == 1);
    REQUIRE(resimulated_row->runs[0].frames.size() == 1);
    REQUIRE(resimulated_row->runs[0].frames[0].frame == 11);
    REQUIRE((resimulated_row->runs[0].frames[0].state_mask & static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Resimulated)) != 0U);

    const auto trigger_row = std::find_if(
        entity.components.begin(),
        entity.components.end(),
        [trigger_component](const ashiato::sync::KTraceComponentRow& row) {
            return row.component == trigger_component;
        });
    REQUIRE(trigger_row != entity.components.end());
    REQUIRE(trigger_row->runs.size() == 1);
}

TEST_CASE("ktrace reader does not duplicate detail events when adding predicted-correct state") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_unique_detail_events_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 4;
    const std::uint32_t wire_id = 8;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{777};
    const ashiato::Entity component{42};

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent received;
        received.type = ashiato::sync::SyncTraceEventType::ComponentReceived;
        received.role = ashiato::sync::SyncTraceRole::Client;
        received.client = client_id;
        received.frame = 12;
        received.server_entity = ashiato::Entity{network_id};
        received.local_entity = local_entity;
        received.client_network_id = network_id;
        received.wire_network_id = wire_id;
        received.network_version = version;
        received.component = component;
        received.mode = ashiato::sync::ReplicationClientMode::Predict;
        writer.tracer().trace(received);
        writer.close();
    }

    const ashiato::sync::KTraceReader reader;
    const ashiato::sync::SyncTraceHistory history = reader.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);

    const ashiato::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    REQUIRE(entity.components.size() == 1);
    REQUIRE(entity.components[0].component == component);
    REQUIRE(entity.components[0].runs[0].frames.size() == 1);

    const ashiato::sync::KTraceFrameCell& cell = entity.components[0].runs[0].frames[0];
    REQUIRE(cell.frame == 12);
    REQUIRE((cell.state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::ReceivedFromServer)) != 0U);
    REQUIRE((cell.state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::PredictedCorrect)) != 0U);
    REQUIRE(cell.event_indices.size() == 1);
}

TEST_CASE("ktrace reader marks predicted-correct on the run that received the component") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_correct_receive_original_run_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 5;
    const std::uint32_t wire_id = 9;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{887};
    const ashiato::Entity component{42};

    auto make_event = [&](ashiato::sync::SyncTraceEventType type, ashiato::sync::SyncFrame frame) {
        ashiato::sync::SyncTraceEvent event;
        event.type = type;
        event.role = ashiato::sync::SyncTraceRole::Client;
        event.client = client_id;
        event.frame = frame;
        event.server_entity = ashiato::Entity{network_id};
        event.local_entity = local_entity;
        event.client_network_id = network_id;
        event.wire_network_id = wire_id;
        event.network_version = version;
        event.component = component;
        event.mode = ashiato::sync::ReplicationClientMode::Predict;
        return event;
    };

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::FrameComponent, 453));
        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::ComponentReceived, 453));
        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::PredictionRollbackConflict, 454));
        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent, 454));
        writer.tracer().trace(make_event(ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent, 455));
        writer.close();
    }

    const ashiato::sync::SyncTraceHistory history = ashiato::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);

    const ashiato::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    REQUIRE(entity.components.size() == 1);
    const ashiato::sync::KTraceComponentRow& row = entity.components[0];
    REQUIRE(row.runs.size() == 2);

    const ashiato::sync::KTraceFrameCell* root_cell = ashiato_sync_tests::find_trace_cell(row.runs[0], 453);
    REQUIRE(root_cell != nullptr);
    REQUIRE((root_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::ReceivedFromServer)) != 0U);
    REQUIRE((root_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::PredictedCorrect)) != 0U);

    const ashiato::sync::KTraceFrameCell* child_cell = ashiato_sync_tests::find_trace_cell(row.runs[1], 453);
    REQUIRE(child_cell == nullptr);
}

TEST_CASE("ktrace reader moves conflicting received component onto carried resim cell") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_conflicting_receive_resim_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 5;
    const std::uint32_t wire_id = 9;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{888};
    const ashiato::Entity component{42};

    auto make_event = [&]() {
        ashiato::sync::SyncTraceEvent event;
        event.role = ashiato::sync::SyncTraceRole::Client;
        event.client = client_id;
        event.frame = 10;
        event.server_entity = ashiato::Entity{network_id};
        event.local_entity = local_entity;
        event.client_network_id = network_id;
        event.wire_network_id = wire_id;
        event.network_version = version;
        event.component = component;
        event.mode = ashiato::sync::ReplicationClientMode::Predict;
        return event;
    };

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent predicted = make_event();
        predicted.type = ashiato::sync::SyncTraceEventType::FrameComponent;
        predicted.data = "original";
        writer.tracer().trace(predicted);

        ashiato::sync::SyncTraceEvent first_conflict = make_event();
        first_conflict.type = ashiato::sync::SyncTraceEventType::PredictionRollbackConflict;
        first_conflict.frame = 9;
        first_conflict.data = "first conflict";
        writer.tracer().trace(first_conflict);

        ashiato::sync::SyncTraceEvent resimulated = make_event();
        resimulated.type = ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent;
        resimulated.data = "resimulated";
        writer.tracer().trace(resimulated);

        ashiato::sync::SyncTraceEvent received = make_event();
        received.type = ashiato::sync::SyncTraceEventType::ComponentReceived;
        received.data = "received";
        writer.tracer().trace(received);

        ashiato::sync::SyncTraceEvent conflict = make_event();
        conflict.type = ashiato::sync::SyncTraceEventType::PredictionRollbackConflict;
        conflict.data = "second conflict";
        writer.tracer().trace(conflict);
        writer.close();
    }

    const ashiato::sync::SyncTraceHistory history = ashiato::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);

    const ashiato::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    REQUIRE(entity.components.size() == 1);
    const auto component_cell = std::find_if(
        entity.components[0].runs[0].frames.begin(),
        entity.components[0].runs[0].frames.end(),
        [](const ashiato::sync::KTraceFrameCell& cell) {
            return cell.frame == 10;
        });
    REQUIRE(component_cell != entity.components[0].runs[0].frames.end());
    REQUIRE((component_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::ReceivedFromServer)) == 0U);
    REQUIRE(component_cell->event_indices.size() == 1);
    REQUIRE(history.sources[0].records[component_cell->event_indices[0]].event.data == "original");

    REQUIRE(entity.components[0].runs.size() == 2);
    REQUIRE(entity.components[0].runs[1].start_frame == 9);
    const ashiato::sync::KTraceFrameCell* carried_cell = ashiato_sync_tests::find_trace_cell(entity.components[0].runs[1], 10);
    REQUIRE(carried_cell != nullptr);
    REQUIRE((carried_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Resimulated)) != 0U);
    REQUIRE((carried_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::ReceivedFromServer)) != 0U);
    REQUIRE((carried_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Mispredicted)) != 0U);
    REQUIRE(carried_cell->event_indices.size() == 3);
}

TEST_CASE("ktrace reader keeps correct received component with active resim cell") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_correct_receive_resim_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 5;
    const std::uint32_t wire_id = 9;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{889};
    const ashiato::Entity component{42};

    auto make_event = [&]() {
        ashiato::sync::SyncTraceEvent event;
        event.role = ashiato::sync::SyncTraceRole::Client;
        event.client = client_id;
        event.frame = 11;
        event.server_entity = ashiato::Entity{network_id};
        event.local_entity = local_entity;
        event.client_network_id = network_id;
        event.wire_network_id = wire_id;
        event.network_version = version;
        event.component = component;
        event.mode = ashiato::sync::ReplicationClientMode::Predict;
        return event;
    };

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent predicted = make_event();
        predicted.type = ashiato::sync::SyncTraceEventType::FrameComponent;
        predicted.data = "original";
        writer.tracer().trace(predicted);

        ashiato::sync::SyncTraceEvent conflict = make_event();
        conflict.type = ashiato::sync::SyncTraceEventType::PredictionRollbackConflict;
        conflict.frame = 10;
        conflict.data = "conflict";
        writer.tracer().trace(conflict);

        ashiato::sync::SyncTraceEvent received = make_event();
        received.type = ashiato::sync::SyncTraceEventType::ComponentReceived;
        received.data = "received";
        writer.tracer().trace(received);

        ashiato::sync::SyncTraceEvent resimulated = make_event();
        resimulated.type = ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent;
        resimulated.data = "resimulated";
        writer.tracer().trace(resimulated);
        writer.close();
    }

    const ashiato::sync::SyncTraceHistory history = ashiato::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);

    const ashiato::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    REQUIRE(entity.components.size() == 1);
    const auto component_cell = std::find_if(
        entity.components[0].runs[0].frames.begin(),
        entity.components[0].runs[0].frames.end(),
        [](const ashiato::sync::KTraceFrameCell& cell) {
            return cell.frame == 11;
        });
    REQUIRE(component_cell != entity.components[0].runs[0].frames.end());
    REQUIRE((component_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::ReceivedFromServer)) != 0U);
    REQUIRE((component_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::PredictedCorrect)) == 0U);

    const auto run_cell = std::find_if(
        entity.components[0].runs[1].frames.begin(),
        entity.components[0].runs[1].frames.end(),
        [](const ashiato::sync::KTraceFrameCell& cell) {
            return cell.frame == 11;
        });
    REQUIRE(run_cell != entity.components[0].runs[1].frames.end());
    REQUIRE((run_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Resimulated)) != 0U);
    REQUIRE((run_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::ReceivedFromServer)) != 0U);
    REQUIRE((run_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::PredictedCorrect)) != 0U);
}

TEST_CASE("ktrace reader keeps standalone rollback conflict on component row") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_standalone_conflict_row_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 5;
    const std::uint32_t wire_id = 9;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{891};
    const ashiato::Entity component{42};

    auto make_event = [&]() {
        ashiato::sync::SyncTraceEvent event;
        event.role = ashiato::sync::SyncTraceRole::Client;
        event.client = client_id;
        event.frame = 12;
        event.server_entity = ashiato::Entity{network_id};
        event.local_entity = local_entity;
        event.client_network_id = network_id;
        event.wire_network_id = wire_id;
        event.network_version = version;
        event.component = component;
        event.mode = ashiato::sync::ReplicationClientMode::Predict;
        return event;
    };

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent predicted = make_event();
        predicted.type = ashiato::sync::SyncTraceEventType::FrameComponent;
        predicted.data = "predicted";
        writer.tracer().trace(predicted);

        ashiato::sync::SyncTraceEvent conflict = make_event();
        conflict.type = ashiato::sync::SyncTraceEventType::PredictionRollbackConflict;
        conflict.data = "conflict";
        writer.tracer().trace(conflict);

        ashiato::sync::SyncTraceEvent received = make_event();
        received.type = ashiato::sync::SyncTraceEventType::ComponentReceived;
        received.data = "received";
        writer.tracer().trace(received);
        writer.close();
    }

    const ashiato::sync::SyncTraceHistory history = ashiato::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);

    const ashiato::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    REQUIRE(entity.components.size() == 1);
    REQUIRE(entity.components[0].runs[0].frames.size() == 1);
    const ashiato::sync::KTraceFrameCell& component_cell = entity.components[0].runs[0].frames[0];
    REQUIRE(component_cell.frame == 12);
    REQUIRE((component_cell.state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Mispredicted)) != 0U);
    REQUIRE((component_cell.state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::ReceivedFromServer)) != 0U);

    REQUIRE(entity.components[0].runs.size() == 1);
}

TEST_CASE("ktrace reader does not create a run for isolated carried rollback conflict") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_isolated_carried_conflict_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 5;
    const std::uint32_t wire_id = 9;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{892};
    const ashiato::Entity component{42};

    auto make_event = [&]() {
        ashiato::sync::SyncTraceEvent event;
        event.role = ashiato::sync::SyncTraceRole::Client;
        event.client = client_id;
        event.server_entity = ashiato::Entity{network_id};
        event.local_entity = local_entity;
        event.client_network_id = network_id;
        event.wire_network_id = wire_id;
        event.network_version = version;
        event.component = component;
        event.mode = ashiato::sync::ReplicationClientMode::Predict;
        return event;
    };

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent first_conflict = make_event();
        first_conflict.type = ashiato::sync::SyncTraceEventType::PredictionRollbackConflict;
        first_conflict.frame = 10;
        writer.tracer().trace(first_conflict);

        ashiato::sync::SyncTraceEvent isolated_resim = make_event();
        isolated_resim.type = ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent;
        isolated_resim.frame = 12;
        writer.tracer().trace(isolated_resim);

        ashiato::sync::SyncTraceEvent later_conflict = make_event();
        later_conflict.type = ashiato::sync::SyncTraceEventType::PredictionRollbackConflict;
        later_conflict.frame = 12;
        writer.tracer().trace(later_conflict);
        writer.close();
    }

    const ashiato::sync::SyncTraceHistory history = ashiato::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);

    const ashiato::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    REQUIRE(entity.components[0].runs.size() == 2);
    const ashiato::sync::KTraceFrameCell* component_cell = ashiato_sync_tests::find_trace_cell(entity.components[0].runs[1], 12);
    REQUIRE(component_cell != nullptr);
    REQUIRE((component_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Mispredicted)) != 0U);
}

TEST_CASE("ktrace reader preserves superseded resim line when later conflict does not resimulate") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_preserve_superseded_resim_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 5;
    const std::uint32_t wire_id = 9;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{890};
    const ashiato::Entity component{42};

    auto make_event = [&]() {
        ashiato::sync::SyncTraceEvent event;
        event.role = ashiato::sync::SyncTraceRole::Client;
        event.client = client_id;
        event.server_entity = ashiato::Entity{network_id};
        event.local_entity = local_entity;
        event.client_network_id = network_id;
        event.wire_network_id = wire_id;
        event.network_version = version;
        event.component = component;
        event.mode = ashiato::sync::ReplicationClientMode::Predict;
        return event;
    };

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent conflict = make_event();
        conflict.type = ashiato::sync::SyncTraceEventType::PredictionRollbackConflict;
        conflict.frame = 10;
        writer.tracer().trace(conflict);

        for (ashiato::sync::SyncFrame frame = 11; frame <= 13; ++frame) {
            ashiato::sync::SyncTraceEvent resimulated = make_event();
            resimulated.type = ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent;
            resimulated.frame = frame;
            resimulated.data = "old resim";
            writer.tracer().trace(resimulated);
        }

        ashiato::sync::SyncTraceEvent later_conflict = make_event();
        later_conflict.type = ashiato::sync::SyncTraceEventType::PredictionRollbackConflict;
        later_conflict.frame = 12;
        writer.tracer().trace(later_conflict);
        writer.close();
    }

    const ashiato::sync::SyncTraceHistory history = ashiato::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);

    const ashiato::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    REQUIRE(entity.components[0].runs.size() == 2);
    REQUIRE(entity.components[0].runs[1].start_frame == 10);
    const ashiato::sync::KTraceFrameRun& old_run = entity.components[0].runs[1];
    REQUIRE(std::any_of(
        old_run.frames.begin(),
        old_run.frames.end(),
        [](const ashiato::sync::KTraceFrameCell& cell) {
            return cell.frame == 11 &&
                (cell.state_mask & static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Resimulated)) != 0U;
        }));
    REQUIRE(std::any_of(
        old_run.frames.begin(),
        old_run.frames.end(),
        [](const ashiato::sync::KTraceFrameCell& cell) {
            return cell.frame == 12 &&
                (cell.state_mask & static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Resimulated)) != 0U;
        }));
    REQUIRE(std::any_of(
        old_run.frames.begin(),
        old_run.frames.end(),
        [](const ashiato::sync::KTraceFrameCell& cell) {
            return cell.frame == 13 &&
                (cell.state_mask & static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Resimulated)) != 0U;
        }));

    REQUIRE(std::any_of(
        old_run.frames.begin(),
        old_run.frames.end(),
        [](const ashiato::sync::KTraceFrameCell& cell) {
            return cell.frame == 12 &&
                (cell.state_mask & static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Mispredicted)) != 0U &&
                (cell.state_mask & static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Resimulated)) != 0U;
        }));
}

TEST_CASE("ktrace writer creates client-only nested rollback resimulation trace") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_nested_client_only_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 5;
    const std::uint32_t wire_id = 9;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{444};
    const ashiato::Entity component{42};

    auto make_event = [&]() {
        ashiato::sync::SyncTraceEvent event;
        event.role = ashiato::sync::SyncTraceRole::Client;
        event.client = client_id;
        event.server_entity = ashiato::Entity{network_id};
        event.local_entity = local_entity;
        event.client_network_id = network_id;
        event.wire_network_id = wire_id;
        event.network_version = version;
        event.component = component;
        event.mode = ashiato::sync::ReplicationClientMode::Predict;
        return event;
    };

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent received = make_event();
        received.type = ashiato::sync::SyncTraceEventType::EntityReceived;
        received.frame = 100;
        writer.tracer().trace(received);

        for (ashiato::sync::SyncFrame frame = 100; frame <= 110; ++frame) {
            ashiato::sync::SyncTraceEvent predicted = make_event();
            predicted.type = ashiato::sync::SyncTraceEventType::FrameComponent;
            predicted.frame = frame;
            predicted.data = "original";
            writer.tracer().trace(predicted);
        }

        const std::array<std::pair<ashiato::sync::SyncFrame, ashiato::sync::SyncFrame>, 3> rollback_ranges{{
            {102, 112},
            {104, 114},
            {106, 116},
        }};
        for (const auto& range : rollback_ranges) {
            ashiato::sync::SyncTraceEvent conflict = make_event();
            conflict.type = ashiato::sync::SyncTraceEventType::PredictionRollbackConflict;
            conflict.frame = range.first;
            conflict.data = "rollback";
            writer.tracer().trace(conflict);

            for (ashiato::sync::SyncFrame frame = range.first; frame <= range.second; ++frame) {
                ashiato::sync::SyncTraceEvent resimulated = make_event();
                resimulated.type = ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent;
                resimulated.frame = frame;
                resimulated.data = "resim";
                writer.tracer().trace(resimulated);
            }
        }
        writer.close();
    }

    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(directory) / "server.ktrace"));
    REQUIRE(std::filesystem::exists(std::filesystem::path(directory) / "5.ktrace"));

    const ashiato::sync::KTraceReader reader;
    const ashiato::sync::SyncTraceHistory history = reader.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].role == ashiato::sync::SyncTraceRole::Client);
    REQUIRE(history.sources[0].entities.size() == 1);

    const ashiato::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    REQUIRE(entity.components.size() == 1);
    REQUIRE(entity.components[0].runs.size() == 4);
    REQUIRE(entity.components[0].runs[1].start_frame == 102);
    REQUIRE(entity.components[0].runs[2].start_frame == 104);
    REQUIRE(entity.components[0].runs[3].start_frame == 106);

    for (std::size_t run_index = 1; run_index < entity.components[0].runs.size(); ++run_index) {
        const ashiato::sync::KTraceFrameRun& run = entity.components[0].runs[run_index];
        REQUIRE(run.frames.size() >= 11);
        REQUIRE((run.frames.front().state_mask &
            static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Resimulated)) != 0U);
    }

    const auto nested_conflict_cell = std::find_if(
        entity.components[0].runs[1].frames.begin(),
        entity.components[0].runs[1].frames.end(),
        [](const ashiato::sync::KTraceFrameCell& cell) {
            return cell.frame == 104;
        });
    REQUIRE(nested_conflict_cell != entity.components[0].runs[1].frames.end());
    REQUIRE((nested_conflict_cell->state_mask &
        static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Mispredicted)) != 0U);
}

#endif
