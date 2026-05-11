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

TEST_CASE("binary sync trace writer writes role-specific files") {
    const std::string server_path = "/tmp/ashiato_sync_trace_server_test.bin";
    const std::string client_path = "/tmp/ashiato_sync_trace_client_test.bin";
    {
        ashiato::sync::BinarySyncTraceWriter writer({server_path, client_path, 1});
        ashiato::sync::SyncTraceEvent server_event;
        server_event.role = ashiato::sync::SyncTraceRole::Server;
        server_event.type = ashiato::sync::SyncTraceEventType::ClientConnected;
        server_event.client = 1;
        writer.tracer().trace(server_event);

        ashiato::sync::SyncTraceEvent client_event;
        client_event.role = ashiato::sync::SyncTraceRole::Client;
        client_event.type = ashiato::sync::SyncTraceEventType::ClientConnected;
        client_event.client = 1;
        writer.tracer().trace(client_event);
        writer.close();
    }

    std::ifstream server_file(server_path, std::ios::binary);
    std::ifstream client_file(client_path, std::ios::binary);
    REQUIRE(server_file.good());
    REQUIRE(client_file.good());
    server_file.seekg(0, std::ios::end);
    client_file.seekg(0, std::ios::end);
    REQUIRE(server_file.tellg() > 10);
    REQUIRE(client_file.tellg() > 10);
}

TEST_CASE("replication server owns configured trace writer lifecycle") {
    const std::string directory = "/tmp/ashiato_sync_owned_trace_writer_test";
    std::filesystem::remove_all(directory);

    ashiato::Registry registry;
    ashiato::sync::ReplicationServerOptions options;
    options.trace.enabled = true;
    options.trace.directory = directory;
    options.trace.flush_threshold_bytes = 1;
    ashiato::sync::ReplicationServer server(registry, options);
    REQUIRE(server.add_client(1));
    server.flush_trace();
    server.close_trace();

    ashiato::sync::KTraceReader reader;
    ashiato::sync::SyncTraceHistory history = reader.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE(history.sources[0].role == ashiato::sync::SyncTraceRole::Server);
    std::vector<ashiato::sync::SyncTraceEvent> events;
    for (const ashiato::sync::KTraceRecord& record : history.sources[0].records) {
        events.push_back(record.event);
    }
    REQUIRE(has_event(events, ashiato::sync::SyncTraceEventType::ClientConnected));
}

TEST_CASE("ktrace directory writer preserves every event and reader builds history") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_directory_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 7;
    const std::uint32_t wire_id = 3;
    const std::uint32_t version = 2;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity server_entity{99};
    const ashiato::Entity local_entity{123};
    const ashiato::Entity component{42};

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent server_mapping;
        server_mapping.type = ashiato::sync::SyncTraceEventType::EntityStartedSyncing;
        server_mapping.role = ashiato::sync::SyncTraceRole::Server;
        server_mapping.client = client_id;
        server_mapping.frame = 1;
        server_mapping.server_entity = server_entity;
        server_mapping.client_network_id = network_id;
        server_mapping.wire_network_id = wire_id;
        server_mapping.network_version = version;
        writer.tracer().trace(server_mapping);

        for (int i = 0; i < 40; ++i) {
            ashiato::sync::SyncTraceEvent event;
            event.type = i == 5
                ? ashiato::sync::SyncTraceEventType::PredictionRollbackConflict
                : ashiato::sync::SyncTraceEventType::FrameComponent;
            event.role = ashiato::sync::SyncTraceRole::Client;
            event.client = client_id;
            event.frame = static_cast<ashiato::sync::SyncFrame>(i + 1);
            event.server_entity = ashiato::Entity{network_id};
            event.local_entity = local_entity;
            event.client_network_id = network_id;
            event.wire_network_id = wire_id;
            event.network_version = version;
            event.component = component;
            event.component_name = "FriendlyPosition";
            event.mode = ashiato::sync::ReplicationClientMode::Predict;
            event.data = "payload";
            writer.tracer().trace(event);
        }

        ashiato::sync::SyncTraceEvent resimulated;
        resimulated.type = ashiato::sync::SyncTraceEventType::ResimulatedFrameComponent;
        resimulated.role = ashiato::sync::SyncTraceRole::Client;
        resimulated.client = client_id;
        resimulated.frame = 7;
        resimulated.server_entity = ashiato::Entity{network_id};
        resimulated.local_entity = local_entity;
        resimulated.client_network_id = network_id;
        resimulated.wire_network_id = wire_id;
        resimulated.network_version = version;
        resimulated.component = component;
        resimulated.component_name = "FriendlyPosition";
        resimulated.mode = ashiato::sync::ReplicationClientMode::Predict;
        resimulated.data = "resim";
        writer.tracer().trace(resimulated);

        ashiato::sync::SyncTraceEvent destroy;
        destroy.type = ashiato::sync::SyncTraceEventType::EntityDestroyed;
        destroy.role = ashiato::sync::SyncTraceRole::Client;
        destroy.client = client_id;
        destroy.frame = 41;
        destroy.server_entity = ashiato::Entity{network_id};
        destroy.local_entity = local_entity;
        destroy.client_network_id = network_id;
        destroy.wire_network_id = wire_id;
        destroy.network_version = version;
        writer.tracer().trace(destroy);
        writer.close();
    }

    REQUIRE(std::filesystem::exists(std::filesystem::path(directory) / "server.ktrace"));
    REQUIRE(std::filesystem::exists(std::filesystem::path(directory) / "7.ktrace"));

    const ashiato::sync::KTraceReader reader;
    const ashiato::sync::SyncTraceHistory history = reader.read_directory(directory);
    REQUIRE(history.sources.size() == 2);

    auto client_source = std::find_if(
        history.sources.begin(),
        history.sources.end(),
        [](const ashiato::sync::KTraceSourceHistory& source) {
            return source.role == ashiato::sync::SyncTraceRole::Client;
    });
    REQUIRE(client_source != history.sources.end());
    REQUIRE(client_source->client == client_id);
    REQUIRE(client_source->records.size() == 43);
    REQUIRE(client_source->component_names.at(component.value) == "FriendlyPosition");
    REQUIRE(std::count_if(
        client_source->records.begin(),
        client_source->records.end(),
        [](const ashiato::sync::KTraceRecord& record) {
            return record.event.type == ashiato::sync::SyncTraceEventType::ComponentName;
        }) == 1);
    REQUIRE(client_source->entities.size() == 1);
    REQUIRE(client_source->entities[0].server_entity == server_entity);

    const auto component_row = std::find_if(
        client_source->entities[0].components.begin(),
        client_source->entities[0].components.end(),
        [component](const ashiato::sync::KTraceComponentRow& row) {
            return row.component == component;
        });
    REQUIRE(component_row != client_source->entities[0].components.end());
    REQUIRE(component_row->runs.size() >= 2);
    REQUIRE(component_row->runs[0].frames.size() >= 39);
    const auto conflict_cell = std::find_if(
        component_row->runs[0].frames.begin(),
        component_row->runs[0].frames.end(),
        [](const ashiato::sync::KTraceFrameCell& cell) {
            return cell.frame == 6;
    });
    REQUIRE(conflict_cell != component_row->runs[0].frames.end());
    REQUIRE((conflict_cell->state_mask & static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Mispredicted)) != 0U);

    const auto resimulated_cell = std::find_if(
        component_row->runs[1].frames.begin(),
        component_row->runs[1].frames.end(),
        [](const ashiato::sync::KTraceFrameCell& cell) {
            return cell.frame == 7;
        });
    REQUIRE(resimulated_cell != component_row->runs[1].frames.end());
    REQUIRE((resimulated_cell->state_mask & static_cast<std::uint16_t>(ashiato::sync::KTraceCellState::Resimulated)) != 0U);
}

#ifdef ASHIATO_SYNC_TRACE_PACKET_LOGS

TEST_CASE("ktrace directory writer marks files when packet logs are enabled") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_packet_logs_flag_test";
    std::filesystem::remove_all(directory);

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});
        writer.tracer().set_packet_logs_enabled(true);
        ashiato::sync::SyncTraceEvent event;
        event.type = ashiato::sync::SyncTraceEventType::PacketLog;
        event.role = ashiato::sync::SyncTraceRole::Server;
        event.frame = 3;
        event.data = "direction=out,message=server_update";
        writer.tracer().trace(event);
        writer.close();
    }

    const ashiato::sync::SyncTraceHistory history = ashiato::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE((history.sources[0].flags & ashiato::sync::ktrace_flag_packet_logs) != 0U);
    REQUIRE(history.sources[0].records.size() == 1);
    REQUIRE(history.sources[0].records[0].event.type == ashiato::sync::SyncTraceEventType::PacketLog);
    REQUIRE(history.sources[0].records[0].event.data.find("server_update") != std::string::npos);
}
#endif

TEST_CASE("ktrace reader keeps complete records when final record is truncated") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_truncated_tail_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 3;
    const std::uint32_t wire_id = 11;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{7};
    const ashiato::Entity component{13};

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent received;
        received.type = ashiato::sync::SyncTraceEventType::ComponentReceived;
        received.role = ashiato::sync::SyncTraceRole::Client;
        received.client = client_id;
        received.frame = 4;
        received.local_entity = local_entity;
        received.client_network_id = network_id;
        received.wire_network_id = wire_id;
        received.network_version = version;
        received.component = component;
        received.component_name = "TailSafeComponent";
        writer.tracer().trace(received);

        received.frame = 5;
        writer.tracer().trace(received);
        writer.close();
    }

    const std::filesystem::path client_path = std::filesystem::path(directory) / "3.ktrace";
    const std::uintmax_t size = std::filesystem::file_size(client_path);
    REQUIRE(size > 8U);
    std::filesystem::resize_file(client_path, size - 8U);

    const ashiato::sync::KTraceReader reader;
    const ashiato::sync::SyncTraceHistory history = reader.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE_FALSE(history.sources[0].records.empty());
    REQUIRE(history.sources[0].entities.size() == 1);
    REQUIRE(history.sources[0].entities[0].components.size() == 1);
    REQUIRE(history.sources[0].entities[0].components[0].runs[0].frames.size() == 1);
    REQUIRE(history.sources[0].entities[0].components[0].runs[0].frames[0].frame == 4);
}

TEST_CASE("ktrace stream reader exposes header metadata and streams records") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_stream_reader_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 9;
    const std::uint32_t wire_id = 17;
    const std::uint32_t version = 2;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{33};
    const ashiato::Entity component{44};

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent received;
        received.type = ashiato::sync::SyncTraceEventType::ComponentReceived;
        received.role = ashiato::sync::SyncTraceRole::Client;
        received.client = client_id;
        received.frame = 7;
        received.local_entity = local_entity;
        received.client_network_id = network_id;
        received.wire_network_id = wire_id;
        received.network_version = version;
        received.component = component;
        received.component_name = "StreamedComponent";
        writer.tracer().trace(received);

        received.frame = 8;
        writer.tracer().trace(received);
        writer.close();
    }

    const std::filesystem::path client_path = std::filesystem::path(directory) / "9.ktrace";
    ashiato::sync::KTraceStreamReader reader(client_path.string());
    const ashiato::sync::KTraceFileHeader& header = reader.header();
    REQUIRE(header.path == client_path.string());
    REQUIRE(header.version == ashiato::sync::ktrace_format_version);
    REQUIRE(header.role == ashiato::sync::SyncTraceRole::Client);
    REQUIRE(header.client == client_id);
    REQUIRE(header.data_offset > 0U);
    REQUIRE(header.file_size == std::filesystem::file_size(client_path));
    REQUIRE(reader.position() == header.data_offset);

    ashiato::sync::KTraceRecord record;
    REQUIRE(reader.read_next(record));
    REQUIRE(record.event.type == ashiato::sync::SyncTraceEventType::ComponentName);
    REQUIRE(record.event.component == component);
    REQUIRE(reader.position() > header.data_offset);

    REQUIRE(reader.read_next(record));
    REQUIRE(record.event.type == ashiato::sync::SyncTraceEventType::ComponentReceived);
    REQUIRE(record.event.frame == 7);

    REQUIRE(reader.read_next(record));
    REQUIRE(record.event.type == ashiato::sync::SyncTraceEventType::ComponentReceived);
    REQUIRE(record.event.frame == 8);

    REQUIRE_FALSE(reader.read_next(record));
}

TEST_CASE("ktrace stream reader stops after complete records when final record is truncated") {
    const std::string directory = "/tmp/ashiato_sync_ktrace_stream_truncated_tail_test";
    std::filesystem::remove_all(directory);

    const ashiato::sync::ClientId client_id = 6;
    const std::uint32_t wire_id = 12;
    const std::uint32_t version = 1;
    const ashiato::sync::ClientEntityNetworkId network_id =
        ashiato::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ashiato::Entity local_entity{55};
    const ashiato::Entity component{66};

    {
        ashiato::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        ashiato::sync::SyncTraceEvent received;
        received.type = ashiato::sync::SyncTraceEventType::ComponentReceived;
        received.role = ashiato::sync::SyncTraceRole::Client;
        received.client = client_id;
        received.frame = 3;
        received.local_entity = local_entity;
        received.client_network_id = network_id;
        received.wire_network_id = wire_id;
        received.network_version = version;
        received.component = component;
        writer.tracer().trace(received);

        received.frame = 4;
        writer.tracer().trace(received);
        writer.close();
    }

    const std::filesystem::path client_path = std::filesystem::path(directory) / "6.ktrace";
    const std::uintmax_t size = std::filesystem::file_size(client_path);
    REQUIRE(size > 8U);
    std::filesystem::resize_file(client_path, size - 8U);

    ashiato::sync::KTraceStreamReader reader(client_path.string());
    std::vector<ashiato::sync::KTraceRecord> records;
    ashiato::sync::KTraceRecord record;
    while (reader.read_next(record)) {
        records.push_back(std::move(record));
    }

    REQUIRE(records.size() == 1);
    REQUIRE(records[0].event.type == ashiato::sync::SyncTraceEventType::ComponentReceived);
    REQUIRE(records[0].event.frame == 3);
}

#endif
