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

TEST_CASE("packet log tracing is opt-in and records client and server packet details") {
    std::vector<kage::sync::SyncTraceEvent> gated_events;
    kage::sync::SyncTracer gated_tracer;
    gated_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { gated_events.push_back(event); }});
    kage::sync::SyncTraceEvent gated_event;
    gated_event.type = kage::sync::SyncTraceEventType::PacketLog;
    gated_event.role = kage::sync::SyncTraceRole::Client;
    gated_event.data = "direction=out";
    gated_tracer.trace(gated_event);
    REQUIRE(gated_events.empty());
    gated_tracer.set_packet_logs_enabled(true);
    gated_tracer.trace(gated_event);
    REQUIRE(gated_events.size() == 1);

    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_networked_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::NetworkedPosition>(
        server_entity,
        kage_sync_tests::NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> server_packets;
    std::vector<kage::sync::SyncTraceEvent> server_events;
    kage::sync::SyncTracer server_tracer;
    server_tracer.set_packet_logs_enabled(true);
    server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        server_packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    server.set_tracer(&server_tracer);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(server_packets.size() == 1);
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PacketLog &&
            event.role == kage::sync::SyncTraceRole::Server &&
            event.data.find("message=server_update") != std::string::npos &&
            event.data.find("sequence=1") != std::string::npos &&
            event.data.find("updated_server_entities=[" + std::to_string(server_entity.value) + "]") != std::string::npos;
    }));

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_networked_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);
    std::vector<kage::sync::SyncTraceEvent> client_events;
    kage::sync::SyncTracer client_tracer;
    client_tracer.set_packet_logs_enabled(true);
    client_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { client_events.push_back(event); }});
    kage::sync::ReplicationClient client;
    client.set_tracer(&client_tracer);
    REQUIRE(client.receive(client_registry, server_packets[0]));
    REQUIRE(std::any_of(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PacketLog &&
            event.role == kage::sync::SyncTraceRole::Client &&
            event.data.find("message=server_update") != std::string::npos &&
            event.data.find("sequence=1") != std::string::npos;
    }));
    std::vector<ecs::BitBuffer> ack_packets = client.drain_ack_packets();
    REQUIRE(ack_packets.size() == 1);
    REQUIRE(std::any_of(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PacketLog &&
            event.role == kage::sync::SyncTraceRole::Client &&
            event.data.find("message=client_ack") != std::string::npos &&
            event.data.find("acks=[1]") != std::string::npos;
    }));
    REQUIRE(server.process_packet(server_registry, 1, ack_packets[0]));
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PacketLog &&
            event.role == kage::sync::SyncTraceRole::Server &&
            event.data.find("message=client_ack") != std::string::npos &&
            event.data.find("acks=[1]") != std::string::npos;
    }));
}

TEST_CASE("packet log tracing records ACK-only traffic as client ACK packets") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_networked_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::NetworkedPosition>(
        server_entity,
        kage_sync_tests::NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> server_packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        server_packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(server_packets.size() == 1);

    ecs::Registry client_registry;
    const kage::sync::SyncArchetypeId client_archetype = define_networked_archetype(client_registry);
    REQUIRE(client_archetype == server_archetype);
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(client_registry));

    std::vector<kage::sync::SyncTraceEvent> client_events;
    kage::sync::SyncTracer client_tracer;
    client_tracer.set_packet_logs_enabled(true);
    client_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { client_events.push_back(event); }});
    kage::sync::ReplicationClient client;
    client.set_tracer(&client_tracer);
    REQUIRE(client.receive(client_registry, server_packets[0]));

    std::vector<ecs::BitBuffer> packets = client.drain_packets();
    REQUIRE(std::any_of(packets.begin(), packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_ack_message;
    }));
    REQUIRE(std::none_of(packets.begin(), packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    }));
    REQUIRE(std::any_of(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PacketLog &&
            event.role == kage::sync::SyncTraceRole::Client &&
            event.data.find("message=client_ack") != std::string::npos &&
            event.data.find("acks=[1]") != std::string::npos;
    }));
    REQUIRE(std::none_of(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PacketLog &&
            event.role == kage::sync::SyncTraceRole::Client &&
            event.data.find("message=client_input") != std::string::npos;
    }));
}

TEST_CASE("packet log tracing records cue summaries and cue payload data") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype =
        kage_sync_tests::define_position_archetype(server_registry);
    kage::sync::register_sync_cue<kage_sync_tests::TestCue>(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(
        server_entity,
        kage_sync_tests::Position{1.0f, 2.0f}) != nullptr);

    std::vector<ecs::BitBuffer> packets;
    std::vector<kage::sync::SyncTraceEvent> server_events;
    kage::sync::SyncTracer server_tracer;
    server_tracer.set_packet_logs_enabled(true);
    server_tracer.set_frame_data_enabled(true);
    server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    server.set_tracer(&server_tracer);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    REQUIRE(kage_sync_tests::emit_test_cue(server_registry, server_entity, 1, kage_sync_tests::TestCue{7}, 1.0f));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PacketLog &&
            event.role == kage::sync::SyncTraceRole::Server &&
            event.data.find("message=server_update") != std::string::npos &&
            event.data.find("cues=[{") != std::string::npos &&
            event.data.find("type=0") != std::string::npos &&
            event.data.find("data=id=7") != std::string::npos;
    }));

    ecs::Registry client_registry;
    REQUIRE(kage_sync_tests::define_position_archetype(client_registry) == server_archetype);
    client_registry.register_component<kage_sync_tests::CuePlayback>("CuePlayback");
    kage::sync::register_sync_cue<kage_sync_tests::TestCue>(client_registry);
    kage::sync::configure_client(client_registry, 1);
    std::vector<kage::sync::SyncTraceEvent> client_events;
    kage::sync::SyncTracer client_tracer;
    client_tracer.set_packet_logs_enabled(true);
    client_tracer.set_frame_data_enabled(true);
    client_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { client_events.push_back(event); }});
    kage::sync::ReplicationClient client;
    client.set_tracer(&client_tracer);
    REQUIRE(client.receive(client_registry, packets[0]));
    REQUIRE(std::any_of(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PacketLog &&
            event.role == kage::sync::SyncTraceRole::Client &&
            event.data.find("message=server_update") != std::string::npos &&
            event.data.find("cues=[{") != std::string::npos &&
            event.data.find("type=0") != std::string::npos &&
            event.data.find("data=id=7") != std::string::npos;
    }));
}

#endif
