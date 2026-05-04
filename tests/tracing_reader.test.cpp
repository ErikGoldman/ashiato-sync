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

TEST_CASE("binary sync trace writer writes role-specific files") {
    const std::string server_path = "/tmp/kage_sync_trace_server_test.bin";
    const std::string client_path = "/tmp/kage_sync_trace_client_test.bin";
    {
        kage::sync::BinarySyncTraceWriter writer({server_path, client_path, 1});
        kage::sync::SyncTraceEvent server_event;
        server_event.role = kage::sync::SyncTraceRole::Server;
        server_event.type = kage::sync::SyncTraceEventType::ClientConnected;
        server_event.client = 1;
        writer.tracer().trace(server_event);

        kage::sync::SyncTraceEvent client_event;
        client_event.role = kage::sync::SyncTraceRole::Client;
        client_event.type = kage::sync::SyncTraceEventType::ClientConnected;
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

TEST_CASE("ktrace directory writer preserves every event and reader builds history") {
    const std::string directory = "/tmp/kage_sync_ktrace_directory_test";
    std::filesystem::remove_all(directory);

    const kage::sync::ClientId client_id = 7;
    const std::uint32_t wire_id = 3;
    const std::uint32_t version = 2;
    const kage::sync::ClientEntityNetworkId network_id =
        kage::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ecs::Entity server_entity{99};
    const ecs::Entity local_entity{123};
    const ecs::Entity component{42};

    {
        kage::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        kage::sync::SyncTraceEvent server_mapping;
        server_mapping.type = kage::sync::SyncTraceEventType::EntityStartedSyncing;
        server_mapping.role = kage::sync::SyncTraceRole::Server;
        server_mapping.client = client_id;
        server_mapping.frame = 1;
        server_mapping.server_entity = server_entity;
        server_mapping.client_network_id = network_id;
        server_mapping.wire_network_id = wire_id;
        server_mapping.network_version = version;
        writer.tracer().trace(server_mapping);

        for (int i = 0; i < 40; ++i) {
            kage::sync::SyncTraceEvent event;
            event.type = i == 5
                ? kage::sync::SyncTraceEventType::PredictionRollbackConflict
                : kage::sync::SyncTraceEventType::FrameComponent;
            event.role = kage::sync::SyncTraceRole::Client;
            event.client = client_id;
            event.frame = static_cast<kage::sync::SyncFrame>(i + 1);
            event.server_entity = ecs::Entity{network_id};
            event.local_entity = local_entity;
            event.client_network_id = network_id;
            event.wire_network_id = wire_id;
            event.network_version = version;
            event.component = component;
            event.component_name = "FriendlyPosition";
            event.mode = kage::sync::ReplicationClientMode::Predict;
            event.data = "payload";
            writer.tracer().trace(event);
        }

        kage::sync::SyncTraceEvent resimulated;
        resimulated.type = kage::sync::SyncTraceEventType::ResimulatedFrameComponent;
        resimulated.role = kage::sync::SyncTraceRole::Client;
        resimulated.client = client_id;
        resimulated.frame = 7;
        resimulated.server_entity = ecs::Entity{network_id};
        resimulated.local_entity = local_entity;
        resimulated.client_network_id = network_id;
        resimulated.wire_network_id = wire_id;
        resimulated.network_version = version;
        resimulated.component = component;
        resimulated.component_name = "FriendlyPosition";
        resimulated.mode = kage::sync::ReplicationClientMode::Predict;
        resimulated.data = "resim";
        writer.tracer().trace(resimulated);

        kage::sync::SyncTraceEvent destroy;
        destroy.type = kage::sync::SyncTraceEventType::EntityDestroyed;
        destroy.role = kage::sync::SyncTraceRole::Client;
        destroy.client = client_id;
        destroy.frame = 41;
        destroy.server_entity = ecs::Entity{network_id};
        destroy.local_entity = local_entity;
        destroy.client_network_id = network_id;
        destroy.wire_network_id = wire_id;
        destroy.network_version = version;
        writer.tracer().trace(destroy);
        writer.close();
    }

    REQUIRE(std::filesystem::exists(std::filesystem::path(directory) / "server.ktrace"));
    REQUIRE(std::filesystem::exists(std::filesystem::path(directory) / "7.ktrace"));

    const kage::sync::KTraceReader reader;
    const kage::sync::SyncTraceHistory history = reader.read_directory(directory);
    REQUIRE(history.sources.size() == 2);

    auto client_source = std::find_if(
        history.sources.begin(),
        history.sources.end(),
        [](const kage::sync::KTraceSourceHistory& source) {
            return source.role == kage::sync::SyncTraceRole::Client;
    });
    REQUIRE(client_source != history.sources.end());
    REQUIRE(client_source->client == client_id);
    REQUIRE(client_source->records.size() == 43);
    REQUIRE(client_source->component_names.at(component.value) == "FriendlyPosition");
    REQUIRE(std::count_if(
        client_source->records.begin(),
        client_source->records.end(),
        [](const kage::sync::KTraceRecord& record) {
            return record.event.type == kage::sync::SyncTraceEventType::ComponentName;
        }) == 1);
    REQUIRE(client_source->entities.size() == 1);
    REQUIRE(client_source->entities[0].server_entity == server_entity);
    REQUIRE(client_source->entities[0].rollback_branches.size() == 1);
    REQUIRE(client_source->entities[0].rollback_branches[0].components.size() == 1);

    const auto component_row = std::find_if(
        client_source->entities[0].components.begin(),
        client_source->entities[0].components.end(),
        [component](const kage::sync::KTraceComponentRow& row) {
            return row.component == component;
        });
    REQUIRE(component_row != client_source->entities[0].components.end());
    REQUIRE(component_row->cells.size() >= 40);
    const auto conflict_cell = std::find_if(
        component_row->cells.begin(),
        component_row->cells.end(),
        [](const kage::sync::KTraceFrameCell& cell) {
            return cell.frame == 6;
    });
    REQUIRE(conflict_cell != component_row->cells.end());
    REQUIRE((conflict_cell->state_mask & static_cast<std::uint16_t>(kage::sync::KTraceCellState::Mispredicted)) != 0U);

    const kage::sync::KTraceComponentRow& branch_component =
        client_source->entities[0].rollback_branches[0].components[0];
    const auto resimulated_cell = std::find_if(
        branch_component.cells.begin(),
        branch_component.cells.end(),
        [](const kage::sync::KTraceFrameCell& cell) {
            return cell.frame == 7;
        });
    REQUIRE(resimulated_cell != branch_component.cells.end());
    REQUIRE((resimulated_cell->state_mask & static_cast<std::uint16_t>(kage::sync::KTraceCellState::Resimulated)) != 0U);
}

#ifdef KAGE_SYNC_TRACE_PACKET_LOGS

TEST_CASE("ktrace directory writer marks files when packet logs are enabled") {
    const std::string directory = "/tmp/kage_sync_ktrace_packet_logs_flag_test";
    std::filesystem::remove_all(directory);

    {
        kage::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});
        writer.tracer().set_packet_logs_enabled(true);
        kage::sync::SyncTraceEvent event;
        event.type = kage::sync::SyncTraceEventType::PacketLog;
        event.role = kage::sync::SyncTraceRole::Server;
        event.frame = 3;
        event.data = "direction=out,message=server_update";
        writer.tracer().trace(event);
        writer.close();
    }

    const kage::sync::SyncTraceHistory history = kage::sync::KTraceReader{}.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE((history.sources[0].flags & kage::sync::ktrace_flag_packet_logs) != 0U);
    REQUIRE(history.sources[0].records.size() == 1);
    REQUIRE(history.sources[0].records[0].event.type == kage::sync::SyncTraceEventType::PacketLog);
    REQUIRE(history.sources[0].records[0].event.data.find("server_update") != std::string::npos);
}
#endif

TEST_CASE("ktrace reader keeps complete records when final record is truncated") {
    const std::string directory = "/tmp/kage_sync_ktrace_truncated_tail_test";
    std::filesystem::remove_all(directory);

    const kage::sync::ClientId client_id = 3;
    const std::uint32_t wire_id = 11;
    const std::uint32_t version = 1;
    const kage::sync::ClientEntityNetworkId network_id =
        kage::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ecs::Entity local_entity{7};
    const ecs::Entity component{13};

    {
        kage::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        kage::sync::SyncTraceEvent received;
        received.type = kage::sync::SyncTraceEventType::ComponentReceived;
        received.role = kage::sync::SyncTraceRole::Client;
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

    const kage::sync::KTraceReader reader;
    const kage::sync::SyncTraceHistory history = reader.read_directory(directory);
    REQUIRE(history.sources.size() == 1);
    REQUIRE_FALSE(history.sources[0].records.empty());
    REQUIRE(history.sources[0].entities.size() == 1);
    REQUIRE(history.sources[0].entities[0].components.size() == 1);
    REQUIRE(history.sources[0].entities[0].components[0].cells.size() == 1);
    REQUIRE(history.sources[0].entities[0].components[0].cells[0].frame == 4);
}

TEST_CASE("ktrace stream reader exposes header metadata and streams records") {
    const std::string directory = "/tmp/kage_sync_ktrace_stream_reader_test";
    std::filesystem::remove_all(directory);

    const kage::sync::ClientId client_id = 9;
    const std::uint32_t wire_id = 17;
    const std::uint32_t version = 2;
    const kage::sync::ClientEntityNetworkId network_id =
        kage::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ecs::Entity local_entity{33};
    const ecs::Entity component{44};

    {
        kage::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        kage::sync::SyncTraceEvent received;
        received.type = kage::sync::SyncTraceEventType::ComponentReceived;
        received.role = kage::sync::SyncTraceRole::Client;
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
    kage::sync::KTraceStreamReader reader(client_path.string());
    const kage::sync::KTraceFileHeader& header = reader.header();
    REQUIRE(header.path == client_path.string());
    REQUIRE(header.version == kage::sync::ktrace_format_version);
    REQUIRE(header.role == kage::sync::SyncTraceRole::Client);
    REQUIRE(header.client == client_id);
    REQUIRE(header.data_offset > 0U);
    REQUIRE(header.file_size == std::filesystem::file_size(client_path));
    REQUIRE(reader.position() == header.data_offset);

    kage::sync::KTraceRecord record;
    REQUIRE(reader.read_next(record));
    REQUIRE(record.event.type == kage::sync::SyncTraceEventType::ComponentName);
    REQUIRE(record.event.component == component);
    REQUIRE(reader.position() > header.data_offset);

    REQUIRE(reader.read_next(record));
    REQUIRE(record.event.type == kage::sync::SyncTraceEventType::ComponentReceived);
    REQUIRE(record.event.frame == 7);

    REQUIRE(reader.read_next(record));
    REQUIRE(record.event.type == kage::sync::SyncTraceEventType::ComponentReceived);
    REQUIRE(record.event.frame == 8);

    REQUIRE_FALSE(reader.read_next(record));
}

TEST_CASE("ktrace stream reader stops after complete records when final record is truncated") {
    const std::string directory = "/tmp/kage_sync_ktrace_stream_truncated_tail_test";
    std::filesystem::remove_all(directory);

    const kage::sync::ClientId client_id = 6;
    const std::uint32_t wire_id = 12;
    const std::uint32_t version = 1;
    const kage::sync::ClientEntityNetworkId network_id =
        kage::sync::make_client_entity_network_id(client_id, wire_id, version);
    const ecs::Entity local_entity{55};
    const ecs::Entity component{66};

    {
        kage::sync::KTraceDirectoryWriter writer({directory, 64, 32, true});

        kage::sync::SyncTraceEvent received;
        received.type = kage::sync::SyncTraceEventType::ComponentReceived;
        received.role = kage::sync::SyncTraceRole::Client;
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

    kage::sync::KTraceStreamReader reader(client_path.string());
    std::vector<kage::sync::KTraceRecord> records;
    kage::sync::KTraceRecord record;
    while (reader.read_next(record)) {
        records.push_back(std::move(record));
    }

    REQUIRE(records.size() == 1);
    REQUIRE(records[0].event.type == kage::sync::SyncTraceEventType::ComponentReceived);
    REQUIRE(records[0].event.frame == 3);
}

#endif
