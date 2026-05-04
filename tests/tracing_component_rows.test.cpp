#include "test_tracing_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#ifdef KAGE_SYNC_ENABLE_TRACING

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace kage_sync_tests;

TEST_CASE("ktrace reader groups server input events with server entity rows and visible cells") {
    const std::string directory = "/tmp/kage_sync_server_input_grouping_test";
    std::filesystem::remove_all(directory);

    const ecs::Entity server_entity{42};
    const ecs::Entity input_component{7};
    const kage::sync::ClientId client = 1;
    const std::uint32_t wire_id = 3;
    const std::uint32_t version = 1;
    const kage::sync::ClientEntityNetworkId client_network_id =
        kage::sync::make_client_entity_network_id(client, wire_id, version);

    kage::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

    kage::sync::SyncTraceEvent transform;
    transform.type = kage::sync::SyncTraceEventType::FrameComponent;
    transform.role = kage::sync::SyncTraceRole::Server;
    transform.frame = 10;
    transform.server_entity = server_entity;
    transform.component = ecs::Entity{5};
    writer.tracer().trace(transform);

    kage::sync::SyncTraceEvent input;
    input.type = kage::sync::SyncTraceEventType::FrameComponent;
    input.role = kage::sync::SyncTraceRole::Server;
    input.client = client;
    input.frame = 10;
    input.server_entity = server_entity;
    input.client_network_id = client_network_id;
    input.wire_network_id = wire_id;
    input.network_version = version;
    input.component = input_component;
    input.data = "x=50,y=60";
    writer.tracer().trace(input);

    kage::sync::SyncTraceEvent starved = input;
    starved.type = kage::sync::SyncTraceEventType::InputStarved;
    starved.data = "input_frame=8";
    writer.tracer().trace(starved);
    writer.close();

    const kage::sync::SyncTraceHistory history = kage::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);
    REQUIRE(history.sources[0].entities[0].server_entity == server_entity);
    REQUIRE(history.sources[0].entities[0].components.size() == 2);

    const auto input_row = std::find_if(
        history.sources[0].entities[0].components.begin(),
        history.sources[0].entities[0].components.end(),
        [input_component](const kage::sync::KTraceComponentRow& row) {
            return row.component == input_component;
        });
    REQUIRE(input_row != history.sources[0].entities[0].components.end());
    REQUIRE(input_row->cells.size() == 1);
    REQUIRE(input_row->cells[0].frame == 10);
    REQUIRE((input_row->cells[0].state_mask &
        static_cast<std::uint16_t>(kage::sync::KTraceCellState::InputReceived)) != 0U);
    REQUIRE((input_row->cells[0].state_mask &
        static_cast<std::uint16_t>(kage::sync::KTraceCellState::Starved)) != 0U);
    REQUIRE(input_row->cells[0].event_indices.size() == 2);
    REQUIRE(history.sources[0].records[input_row->cells[0].event_indices[0]].event.data == "x=50,y=60");
    REQUIRE(history.sources[0].records[input_row->cells[0].event_indices[1]].event.data == "input_frame=8");
}

TEST_CASE("ktrace reader groups cue lifecycle events on cue rows") {
    const std::string directory = "/tmp/kage_sync_ktrace_cue_rows_test";
    std::filesystem::remove_all(directory);

    const ecs::Entity server_entity{42};
    const kage::sync::ClientId client = 1;
    const std::uint32_t wire_id = 3;
    const std::uint32_t version = 1;
    const kage::sync::ClientEntityNetworkId client_network_id =
        kage::sync::make_client_entity_network_id(client, wire_id, version);
    constexpr kage::sync::SyncCueTypeId cue_type = 5;

    {
        kage::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});
        const std::array<kage::sync::SyncTraceEventType, 6> types{{
            kage::sync::SyncTraceEventType::CueEmitted,
            kage::sync::SyncTraceEventType::CueSent,
            kage::sync::SyncTraceEventType::CueReceived,
            kage::sync::SyncTraceEventType::CuePlayed,
            kage::sync::SyncTraceEventType::CueConfirmed,
            kage::sync::SyncTraceEventType::CueRolledBack,
        }};
        for (kage::sync::SyncTraceEventType type : types) {
            kage::sync::SyncTraceEvent event;
            event.type = type;
            event.role = kage::sync::SyncTraceRole::Client;
            event.client = client;
            event.frame = 10;
            event.server_entity = server_entity;
            event.local_entity = ecs::Entity{123};
            event.client_network_id = client_network_id;
            event.wire_network_id = wire_id;
            event.network_version = version;
            event.cue_type = cue_type;
            event.component_name = "TestCue";
            writer.tracer().trace(event);
        }
        writer.close();
    }

    const kage::sync::SyncTraceHistory history = kage::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);
    REQUIRE(history.sources[0].entities[0].server_entity == server_entity);
    REQUIRE(history.sources[0].entities[0].components.size() == 1);
    REQUIRE(history.sources[0].cue_names.at(cue_type) == "TestCue");
    REQUIRE(std::count_if(
        history.sources[0].records.begin(),
        history.sources[0].records.end(),
        [](const kage::sync::KTraceRecord& record) {
            return record.event.type == kage::sync::SyncTraceEventType::CueName;
        }) == 1);
    REQUIRE(std::none_of(
        history.sources[0].records.begin(),
        history.sources[0].records.end(),
        [](const kage::sync::KTraceRecord& record) {
            return record.event.type != kage::sync::SyncTraceEventType::CueName &&
                !record.event.component_name.empty();
        }));

    const kage::sync::KTraceComponentRow& cue_row = history.sources[0].entities[0].components[0];
    REQUIRE(cue_row.component.value == (std::uint64_t{1} << 63U));
    REQUIRE(cue_row.cells.size() == 1);
    REQUIRE(cue_row.cells[0].frame == 10);
    REQUIRE(cue_row.cells[0].event_indices.size() == 6);
    const std::uint32_t mask = cue_row.cells[0].state_mask;
    REQUIRE((mask & static_cast<std::uint32_t>(kage::sync::KTraceCellState::CueEmitted)) != 0U);
    REQUIRE((mask & static_cast<std::uint32_t>(kage::sync::KTraceCellState::CueSent)) != 0U);
    REQUIRE((mask & static_cast<std::uint32_t>(kage::sync::KTraceCellState::CueReceived)) != 0U);
    REQUIRE((mask & static_cast<std::uint32_t>(kage::sync::KTraceCellState::CuePlayed)) != 0U);
    REQUIRE((mask & static_cast<std::uint32_t>(kage::sync::KTraceCellState::CueConfirmed)) != 0U);
    REQUIRE((mask & static_cast<std::uint32_t>(kage::sync::KTraceCellState::CueRolledBack)) != 0U);
}

TEST_CASE("client frame component tracing records once per fixed receive frame") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_networked_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::NetworkedPosition>(
        server_entity,
        kage_sync_tests::NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_networked_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClient client;
    std::vector<kage::sync::SyncTraceEvent> client_events;
    kage::sync::SyncTracer client_tracer;
    client_tracer.set_frame_data_enabled(true);
    client_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { client_events.push_back(event); }});
    client.set_tracer(&client_tracer);

    REQUIRE(client.receive(client_registry, packets[0]));
    client_events.clear();

    const double substep = client.options().fixed_dt_seconds * 0.25;
    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(std::none_of(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent;
    }));

    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent && event.frame == 1;
    }) == 1);

    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent && event.frame == 1;
    }) == 1);

    REQUIRE(client.tick(client_registry, substep));
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent && event.frame == 2;
    }) == 1);

    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds * 2.0));
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent && event.frame == 3;
    }) == 1);
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent && event.frame == 4;
    }) == 1);
}

TEST_CASE("predicted client frame component tracing records predicted mode once") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_predicted_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::PredictedPosition>(
        server_entity,
        kage_sync_tests::PredictedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_predicted_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);
    kage::sync::ReplicationClientOptions client_options;
    client_options.default_entity_mode = kage::sync::ReplicationClientMode::Predict;
    kage::sync::ReplicationClient client(client_options);
    client.simulation_job<kage_sync_tests::PredictedPosition>(client_registry, 0).each(
        [](ecs::Entity, kage_sync_tests::PredictedPosition& position) {
            position.x += 1.0f;
        });

    std::vector<kage::sync::SyncTraceEvent> client_events;
    kage::sync::SyncTracer client_tracer;
    client_tracer.set_frame_data_enabled(true);
    client_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { client_events.push_back(event); }});
    client.set_tracer(&client_tracer);

    REQUIRE(client.receive(client_registry, packets[0]));
    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::ComponentReceived &&
            event.mode == kage::sync::ReplicationClientMode::Predict;
    }) == 1);
    REQUIRE(std::none_of(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::ComponentReceived &&
            event.mode == kage::sync::ReplicationClientMode::Snap;
    }));
    REQUIRE_FALSE(has_event(client_events, kage::sync::SyncTraceEventType::ComponentApplied));
    client_events.clear();
    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));

    REQUIRE(std::count_if(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent &&
            event.frame == 2 &&
            event.mode == kage::sync::ReplicationClientMode::Predict;
    }) == 1);
    REQUIRE(std::none_of(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent &&
            event.mode == kage::sync::ReplicationClientMode::Snap;
    }));
}

TEST_CASE("ktrace reader groups resimulated component rows under rollback trigger component") {
    const std::string directory = "/tmp/kage_sync_ktrace_branch_trigger_test";
    std::filesystem::remove_all(directory);

    const kage::sync::ClientId client_id = 3;
    const std::uint32_t wire_id = 5;
    const std::uint32_t version = 1;
    const kage::sync::ClientEntityNetworkId network_id =
        kage::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ecs::Entity local_entity{321};
    const ecs::Entity trigger_component{42};
    const ecs::Entity resimulated_component{77};

    {
        kage::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        kage::sync::SyncTraceEvent conflict;
        conflict.type = kage::sync::SyncTraceEventType::PredictionRollbackConflict;
        conflict.role = kage::sync::SyncTraceRole::Client;
        conflict.client = client_id;
        conflict.frame = 10;
        conflict.server_entity = ecs::Entity{network_id};
        conflict.local_entity = local_entity;
        conflict.client_network_id = network_id;
        conflict.wire_network_id = wire_id;
        conflict.network_version = version;
        conflict.component = trigger_component;
        conflict.mode = kage::sync::ReplicationClientMode::Predict;
        writer.tracer().trace(conflict);

        kage::sync::SyncTraceEvent resimulated = conflict;
        resimulated.type = kage::sync::SyncTraceEventType::ResimulatedFrameComponent;
        resimulated.frame = 11;
        resimulated.component = resimulated_component;
        writer.tracer().trace(resimulated);
        writer.close();
    }

    const kage::sync::KTraceReader reader;
    const kage::sync::SyncTraceHistory history = reader.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);

    const kage::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    REQUIRE(entity.rollback_branches.size() == 1);
    const kage::sync::KTraceEntityBranch& branch = entity.rollback_branches[0];
    REQUIRE(branch.component == trigger_component);
    REQUIRE(branch.from_frame == 10);

    const auto resimulated_row = std::find_if(
        branch.components.begin(),
        branch.components.end(),
        [resimulated_component](const kage::sync::KTraceComponentRow& row) {
            return row.component == resimulated_component;
        });
    REQUIRE(resimulated_row != branch.components.end());
    REQUIRE(resimulated_row->cells.size() == 1);
    REQUIRE(resimulated_row->cells[0].frame == 11);
    REQUIRE((resimulated_row->cells[0].state_mask & static_cast<std::uint16_t>(kage::sync::KTraceCellState::Resimulated)) != 0U);
}

TEST_CASE("ktrace reader does not duplicate detail events when adding predicted-correct state") {
    const std::string directory = "/tmp/kage_sync_ktrace_unique_detail_events_test";
    std::filesystem::remove_all(directory);

    const kage::sync::ClientId client_id = 4;
    const std::uint32_t wire_id = 8;
    const std::uint32_t version = 1;
    const kage::sync::ClientEntityNetworkId network_id =
        kage::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ecs::Entity local_entity{777};
    const ecs::Entity component{42};

    {
        kage::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        kage::sync::SyncTraceEvent received;
        received.type = kage::sync::SyncTraceEventType::ComponentReceived;
        received.role = kage::sync::SyncTraceRole::Client;
        received.client = client_id;
        received.frame = 12;
        received.server_entity = ecs::Entity{network_id};
        received.local_entity = local_entity;
        received.client_network_id = network_id;
        received.wire_network_id = wire_id;
        received.network_version = version;
        received.component = component;
        received.mode = kage::sync::ReplicationClientMode::Predict;
        writer.tracer().trace(received);
        writer.close();
    }

    const kage::sync::KTraceReader reader;
    const kage::sync::SyncTraceHistory history = reader.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].entities.size() == 1);

    const kage::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    REQUIRE(entity.components.size() == 1);
    REQUIRE(entity.components[0].component == component);
    REQUIRE(entity.components[0].cells.size() == 1);

    const kage::sync::KTraceFrameCell& cell = entity.components[0].cells[0];
    REQUIRE(cell.frame == 12);
    REQUIRE((cell.state_mask &
        static_cast<std::uint16_t>(kage::sync::KTraceCellState::ReceivedFromServer)) != 0U);
    REQUIRE((cell.state_mask &
        static_cast<std::uint16_t>(kage::sync::KTraceCellState::PredictedCorrect)) != 0U);
    REQUIRE(cell.event_indices.size() == 1);
}

TEST_CASE("ktrace writer creates client-only nested rollback resimulation trace") {
    const std::string directory = "/tmp/kage_sync_ktrace_nested_client_only_test";
    std::filesystem::remove_all(directory);

    const kage::sync::ClientId client_id = 5;
    const std::uint32_t wire_id = 9;
    const std::uint32_t version = 1;
    const kage::sync::ClientEntityNetworkId network_id =
        kage::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ecs::Entity local_entity{444};
    const ecs::Entity component{42};

    auto make_event = [&]() {
        kage::sync::SyncTraceEvent event;
        event.role = kage::sync::SyncTraceRole::Client;
        event.client = client_id;
        event.server_entity = ecs::Entity{network_id};
        event.local_entity = local_entity;
        event.client_network_id = network_id;
        event.wire_network_id = wire_id;
        event.network_version = version;
        event.component = component;
        event.mode = kage::sync::ReplicationClientMode::Predict;
        return event;
    };

    {
        kage::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        kage::sync::SyncTraceEvent received = make_event();
        received.type = kage::sync::SyncTraceEventType::EntityReceived;
        received.frame = 100;
        writer.tracer().trace(received);

        for (kage::sync::SyncFrame frame = 100; frame <= 110; ++frame) {
            kage::sync::SyncTraceEvent predicted = make_event();
            predicted.type = kage::sync::SyncTraceEventType::FrameComponent;
            predicted.frame = frame;
            predicted.data = "original";
            writer.tracer().trace(predicted);
        }

        const std::array<std::pair<kage::sync::SyncFrame, kage::sync::SyncFrame>, 3> rollback_ranges{{
            {102, 112},
            {104, 114},
            {106, 116},
        }};
        for (const auto& range : rollback_ranges) {
            kage::sync::SyncTraceEvent conflict = make_event();
            conflict.type = kage::sync::SyncTraceEventType::PredictionRollbackConflict;
            conflict.frame = range.first;
            conflict.data = "rollback";
            writer.tracer().trace(conflict);

            for (kage::sync::SyncFrame frame = range.first; frame <= range.second; ++frame) {
                kage::sync::SyncTraceEvent resimulated = make_event();
                resimulated.type = kage::sync::SyncTraceEventType::ResimulatedFrameComponent;
                resimulated.frame = frame;
                resimulated.data = "resim";
                writer.tracer().trace(resimulated);
            }
        }
        writer.close();
    }

    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(directory) / "server.ktrace"));
    REQUIRE(std::filesystem::exists(std::filesystem::path(directory) / "5.ktrace"));

    const kage::sync::KTraceReader reader;
    const kage::sync::SyncTraceHistory history = reader.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].role == kage::sync::SyncTraceRole::Client);
    REQUIRE(history.sources[0].entities.size() == 1);

    const kage::sync::KTraceEntityRow& entity = history.sources[0].entities[0];
    REQUIRE(entity.rollback_branches.size() == 3);
    REQUIRE(entity.rollback_branches[0].from_frame == 102);
    REQUIRE(entity.rollback_branches[1].from_frame == 104);
    REQUIRE(entity.rollback_branches[2].from_frame == 106);

    for (const kage::sync::KTraceEntityBranch& branch : entity.rollback_branches) {
        REQUIRE(branch.component == component);
        REQUIRE(branch.components.size() == 1);
        REQUIRE(branch.components[0].component == component);
        REQUIRE(branch.components[0].cells.size() >= 11);
        REQUIRE((branch.components[0].cells.front().state_mask &
            static_cast<std::uint16_t>(kage::sync::KTraceCellState::Resimulated)) != 0U);
    }

    const kage::sync::KTraceComponentRow& first_branch_component = entity.rollback_branches[0].components[0];
    const auto nested_conflict_cell = std::find_if(
        first_branch_component.cells.begin(),
        first_branch_component.cells.end(),
        [](const kage::sync::KTraceFrameCell& cell) {
            return cell.frame == 104;
        });
    REQUIRE(nested_conflict_cell != first_branch_component.cells.end());
    REQUIRE((nested_conflict_cell->state_mask &
        static_cast<std::uint16_t>(kage::sync::KTraceCellState::Mispredicted)) != 0U);
}

#endif
