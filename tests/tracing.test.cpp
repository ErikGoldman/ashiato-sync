#include "test_components.hpp"

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

namespace {

bool has_event(
    const std::vector<kage::sync::SyncTraceEvent>& events,
    kage::sync::SyncTraceEventType type) {
    return std::any_of(events.begin(), events.end(), [type](const kage::sync::SyncTraceEvent& event) {
        return event.type == type;
    });
}

bool has_cue_event(
    const std::vector<kage::sync::SyncTraceEvent>& events,
    kage::sync::SyncTraceEventType type,
    kage::sync::SyncCueTypeId cue_type) {
    return std::any_of(events.begin(), events.end(), [type, cue_type](const kage::sync::SyncTraceEvent& event) {
        return event.type == type && event.cue_type == cue_type;
    });
}

bool has_cue_event_data(
    const std::vector<kage::sync::SyncTraceEvent>& events,
    kage::sync::SyncTraceEventType type,
    const std::string& data) {
    return std::any_of(events.begin(), events.end(), [type, &data](const kage::sync::SyncTraceEvent& event) {
        return event.type == type && event.data.find(data) != std::string::npos;
    });
}

kage::sync::SyncArchetypeId define_networked_archetype(ecs::Registry& registry) {
    const ecs::Entity position =
        kage::sync::register_sync_component<kage_sync_tests::NetworkedPosition>(registry, "NetworkedPosition");
    return kage::sync::define_archetype(
        registry,
        "NetworkedActor",
        {{position, kage::sync::ReplicationAudience::All}});
}

kage::sync::SyncArchetypeId define_predicted_archetype(ecs::Registry& registry) {
    const ecs::Entity position =
        kage::sync::register_sync_component<kage_sync_tests::PredictedPosition>(registry, "PredictedPosition");
    return kage::sync::define_archetype(
        registry,
        "PredictedActor",
        {{position, kage::sync::ReplicationAudience::All}});
}

bool start_sync(ecs::Registry& registry, ecs::Entity entity, kage::sync::SyncArchetypeId archetype) {
    return registry.add<kage::sync::Replicated>(entity, kage::sync::Replicated{archetype}) != nullptr;
}

struct AnonymousCueNameProbe {};

}  // namespace

TEST_CASE("sync cue default type names omit anonymous namespace qualifiers") {
    REQUIRE(kage::sync::detail::default_type_name<AnonymousCueNameProbe>() == "AnonymousCueNameProbe");
}

TEST_CASE("sync tracing records server send client receive and apply events") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_networked_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::NetworkedPosition>(
        server_entity,
        kage_sync_tests::NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    std::vector<kage::sync::SyncTraceEvent> server_events;
    kage::sync::SyncTracer server_tracer;
    server_tracer.set_frame_data_enabled(true);
    server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
    server.set_tracer(&server_tracer);

    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);
    REQUIRE(packets.size() == 1);
    REQUIRE(has_event(server_events, kage::sync::SyncTraceEventType::ClientConnected));
    REQUIRE(has_event(server_events, kage::sync::SyncTraceEventType::EntityStartedSyncing));
    REQUIRE(has_event(server_events, kage::sync::SyncTraceEventType::ComponentSent));
    REQUIRE(has_event(server_events, kage::sync::SyncTraceEventType::FrameComponent));

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
    REQUIRE(has_event(client_events, kage::sync::SyncTraceEventType::EntityReceived));
    REQUIRE(has_event(client_events, kage::sync::SyncTraceEventType::ComponentReceived));
    REQUIRE_FALSE(has_event(client_events, kage::sync::SyncTraceEventType::ComponentApplied));
}

TEST_CASE("cue tracing records lifecycle events and uses frame data gating") {
    auto run_case = [](bool frame_data_enabled) {
        ecs::Registry server_registry;
        const kage::sync::SyncArchetypeId server_archetype =
            kage_sync_tests::define_position_archetype(server_registry);
        const kage::sync::SyncCueTypeId cue_type =
            kage::sync::register_sync_cue<kage_sync_tests::TestCue>(server_registry);
        const ecs::Entity server_entity = server_registry.create();
        REQUIRE(server_registry.add<kage_sync_tests::Position>(
            server_entity,
            kage_sync_tests::Position{1.0f, 2.0f}) != nullptr);

        std::vector<kage::sync::BitBuffer> packets;
        kage::sync::ReplicationServerOptions server_options;
        server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
            packets.push_back(packet);
        };
        kage::sync::ReplicationServer server(server_options);
        std::vector<kage::sync::SyncTraceEvent> server_events;
        kage::sync::SyncTracer server_tracer;
        server_tracer.set_frame_data_enabled(frame_data_enabled);
        server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
            [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
        server.set_tracer(&server_tracer);

        REQUIRE(server.add_client(1));
        REQUIRE(start_sync(server_registry, server_entity, server_archetype));
        REQUIRE(kage::sync::emit_cue(server_registry, server_entity, 1, kage_sync_tests::TestCue{7}, 1.0f));
        server.tick(server_registry);
        REQUIRE(packets.size() == 1);
        REQUIRE(has_cue_event(server_events, kage::sync::SyncTraceEventType::CueEmitted, cue_type));
        REQUIRE(has_cue_event(server_events, kage::sync::SyncTraceEventType::CueSent, cue_type));

        ecs::Registry client_registry;
        const kage::sync::SyncArchetypeId client_archetype =
            kage_sync_tests::define_position_archetype(client_registry);
        REQUIRE(client_archetype == server_archetype);
        client_registry.register_component<kage_sync_tests::CuePlayback>("CuePlayback");
        REQUIRE(kage::sync::register_sync_cue<kage_sync_tests::TestCue>(client_registry) == cue_type);
        kage::sync::configure_client(client_registry, 1);
        std::vector<kage::sync::SyncTraceEvent> client_events;
        kage::sync::SyncTracer client_tracer;
        client_tracer.set_frame_data_enabled(frame_data_enabled);
        client_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
            [&](const kage::sync::SyncTraceEvent& event) { client_events.push_back(event); }});
        kage::sync::ReplicationClient client;
        client.set_tracer(&client_tracer);

        REQUIRE(client.receive(client_registry, packets[0]));
        REQUIRE(has_cue_event(client_events, kage::sync::SyncTraceEventType::CueReceived, cue_type));
        REQUIRE(has_cue_event(client_events, kage::sync::SyncTraceEventType::CuePlayed, cue_type));
        REQUIRE_FALSE(has_cue_event(client_events, kage::sync::SyncTraceEventType::CueConfirmed, cue_type));
        return std::make_pair(std::move(server_events), std::move(client_events));
    };

    auto without_data = run_case(false);
    REQUIRE_FALSE(has_cue_event_data(without_data.first, kage::sync::SyncTraceEventType::CueEmitted, "id=7"));
    REQUIRE_FALSE(has_cue_event_data(without_data.first, kage::sync::SyncTraceEventType::CueSent, "id=7"));
    REQUIRE_FALSE(has_cue_event_data(without_data.second, kage::sync::SyncTraceEventType::CueReceived, "id=7"));
    REQUIRE_FALSE(has_cue_event_data(without_data.second, kage::sync::SyncTraceEventType::CuePlayed, "id=7"));

    auto with_data = run_case(true);
#ifdef KAGE_SYNC_TRACE_COMPONENT_DATA
    REQUIRE(has_cue_event_data(with_data.first, kage::sync::SyncTraceEventType::CueEmitted, "id=7"));
    REQUIRE(has_cue_event_data(with_data.first, kage::sync::SyncTraceEventType::CueSent, "id=7"));
    REQUIRE(has_cue_event_data(with_data.second, kage::sync::SyncTraceEventType::CueReceived, "id=7"));
    REQUIRE(has_cue_event_data(with_data.second, kage::sync::SyncTraceEventType::CuePlayed, "id=7"));
#else
    (void)with_data;
#endif
}

TEST_CASE("server transport traces job-emitted cues at authoritative snapshot frame") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype =
        kage_sync_tests::define_position_archetype(server_registry);
    const kage::sync::SyncCueTypeId cue_type =
        kage::sync::register_sync_cue<kage_sync_tests::TestCue>(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::Position>(
        server_entity,
        kage_sync_tests::Position{1.0f, 2.0f}) != nullptr);

    bool fired = false;
    server_registry.job<kage_sync_tests::Position>(0).each(
        [&](ecs::Entity entity, kage_sync_tests::Position&) {
            if (entity == server_entity && !fired) {
                REQUIRE(kage::sync::emit_cue(server_registry, entity, kage_sync_tests::TestCue{7}, 1.0f));
                fired = true;
            }
        });

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    std::vector<kage::sync::SyncTraceEvent> server_events;
    kage::sync::SyncTracer server_tracer;
    server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
    server.set_tracer(&server_tracer);

    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    REQUIRE(server.tick(server_registry, server.options().fixed_dt_seconds));
    REQUIRE(fired);
    REQUIRE(server.frame() == 1);
    REQUIRE(packets.size() == 1);

    const auto emitted = std::find_if(
        server_events.begin(),
        server_events.end(),
        [cue_type](const kage::sync::SyncTraceEvent& event) {
            return event.type == kage::sync::SyncTraceEventType::CueEmitted && event.cue_type == cue_type;
        });
    REQUIRE(emitted != server_events.end());
    REQUIRE(emitted->frame == 1);

    const auto sent = std::find_if(
        server_events.begin(),
        server_events.end(),
        [cue_type](const kage::sync::SyncTraceEvent& event) {
            return event.type == kage::sync::SyncTraceEventType::CueSent && event.cue_type == cue_type;
        });
    REQUIRE(sent != server_events.end());
    REQUIRE(sent->frame == 1);
}

TEST_CASE("client tracing records clock skew timing decisions") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_networked_archetype(server_registry);
    const ecs::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::NetworkedPosition>(
        server_entity,
        kage_sync_tests::NetworkedPosition{1.0f, 2.0f}) != nullptr);

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
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
    REQUIRE(kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(client_registry));

    std::vector<kage::sync::SyncTraceEvent> client_events;
    kage::sync::SyncTracer client_tracer;
    client_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { client_events.push_back(event); }});

    kage::sync::ReplicationClientOptions options;
    options.input_buffer_capacity_frames = 32;
    kage::sync::ReplicationClient client(options);
    client.set_tracer(&client_tracer);
    REQUIRE(client.set_input(client_registry, kage_sync_tests::NetworkedPosition{3.0f, 4.0f}));
    REQUIRE(client.receive(client_registry, packets[0], 8));

    const auto event = std::find_if(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& trace) {
        return trace.type == kage::sync::SyncTraceEventType::ClockSkew &&
            trace.data.find("stage=clock_requested_prefill") != std::string::npos &&
            trace.data.find("observed_downstream=") != std::string::npos &&
            trace.data.find("prediction_target=") != std::string::npos &&
            trace.data.find("prefill_input=") != std::string::npos;
    });
    REQUIRE(event != client_events.end());
    REQUIRE(event->component_name == "Clock");
}

TEST_CASE("client input tracing records input components every input tick") {
    ecs::Registry registry;
    const ecs::Entity input_component =
        kage::sync::register_sync_component<kage_sync_tests::NetworkedPosition>(registry, "NetworkedPosition");
    kage::sync::configure_client(registry, 1);
    REQUIRE(kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(registry));

    const ecs::Entity owned = registry.create();
    REQUIRE(kage::sync::set_owner(registry, owned, 1));

    std::vector<kage::sync::SyncTraceEvent> events;
    kage::sync::SyncTracer tracer;
    tracer.set_frame_data_enabled(true);
    tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { events.push_back(event); }});

    kage::sync::ReplicationClient client;
    client.set_tracer(&tracer);
    REQUIRE(client.set_input(registry, kage_sync_tests::NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(registry, client.options().fixed_dt_seconds));

    const auto input_event = std::find_if(events.begin(), events.end(), [&](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent &&
            event.role == kage::sync::SyncTraceRole::Client &&
            event.local_entity == owned &&
            event.component == input_component &&
            event.frame == 1;
    });
    REQUIRE(input_event != events.end());
}

#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
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

    std::vector<kage::sync::BitBuffer> server_packets;
    std::vector<kage::sync::SyncTraceEvent> server_events;
    kage::sync::SyncTracer server_tracer;
    server_tracer.set_packet_logs_enabled(true);
    server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        server_packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    server.set_tracer(&server_tracer);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);
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
    std::vector<kage::sync::BitBuffer> ack_packets = client.drain_ack_packets();
    REQUIRE(ack_packets.size() == 1);
    REQUIRE(std::any_of(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PacketLog &&
            event.role == kage::sync::SyncTraceRole::Client &&
            event.data.find("message=client_ack") != std::string::npos &&
            event.data.find("acks=[1]") != std::string::npos;
    }));
    REQUIRE(server.process_packet(1, ack_packets[0]));
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

    std::vector<kage::sync::BitBuffer> server_packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        server_packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry);
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

    std::vector<kage::sync::BitBuffer> packets = client.drain_packets();
    REQUIRE(std::any_of(packets.begin(), packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_ack_message;
    }));
    REQUIRE(std::none_of(packets.begin(), packets.end(), [](kage::sync::BitBuffer packet) {
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

    std::vector<kage::sync::BitBuffer> packets;
    std::vector<kage::sync::SyncTraceEvent> server_events;
    kage::sync::SyncTracer server_tracer;
    server_tracer.set_packet_logs_enabled(true);
    server_tracer.set_frame_data_enabled(true);
    server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        packets.push_back(packet);
    };
    kage::sync::ReplicationServer server(server_options);
    server.set_tracer(&server_tracer);
    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    REQUIRE(kage::sync::emit_cue(server_registry, server_entity, 1, kage_sync_tests::TestCue{7}, 1.0f));
    server.tick(server_registry);
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

TEST_CASE("packet log tracing records client input baseline and server received input range") {
    ecs::Registry client_registry;
    kage::sync::register_sync_component<kage_sync_tests::NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(client_registry));
    const ecs::Entity client_owned = client_registry.create();
    REQUIRE(kage::sync::set_owner(client_registry, client_owned, 1));

    std::vector<kage::sync::SyncTraceEvent> client_events;
    kage::sync::SyncTracer client_tracer;
    client_tracer.set_packet_logs_enabled(true);
    client_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { client_events.push_back(event); }});
    kage::sync::ReplicationClient client;
    client.set_tracer(&client_tracer);
    REQUIRE(client.set_input(client_registry, kage_sync_tests::NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    std::vector<kage::sync::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != input_packets.end());
    REQUIRE(std::any_of(client_events.begin(), client_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PacketLog &&
            event.data.find("message=client_input") != std::string::npos &&
            event.data.find("input_frames=1-1") != std::string::npos &&
            event.data.find("baseline=0") != std::string::npos;
    }));

    ecs::Registry server_registry;
    kage::sync::register_sync_component<kage_sync_tests::NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(server_registry));
    const ecs::Entity server_owned = server_registry.create();
    REQUIRE(kage::sync::set_owner(server_registry, server_owned, 1));

    std::vector<kage::sync::SyncTraceEvent> server_events;
    kage::sync::SyncTracer server_tracer;
    server_tracer.set_packet_logs_enabled(true);
    server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [](kage::sync::ClientId, const kage::sync::BitBuffer&) {};
    kage::sync::ReplicationServer server(server_options);
    server.set_tracer(&server_tracer);
    REQUIRE(server.add_client(1));
    REQUIRE(server.process_packet(server_registry, 1, *input_packet));
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::PacketLog &&
            event.role == kage::sync::SyncTraceRole::Server &&
            event.data.find("message=client_input") != std::string::npos &&
            event.data.find("input_frames=1-1") != std::string::npos &&
            event.data.find("baseline=0") != std::string::npos;
    }));
}
#endif

TEST_CASE("server input tracing records input components and stale input starvation") {
    ecs::Registry server_registry;
    const ecs::Entity server_input_component =
        kage::sync::register_sync_component<kage_sync_tests::NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(server_registry));

    const ecs::Entity owned = server_registry.create();
    REQUIRE(kage::sync::set_owner(server_registry, owned, 1));

    std::vector<kage::sync::SyncTraceEvent> server_events;
    kage::sync::SyncTracer server_tracer;
    server_tracer.set_frame_data_enabled(true);
    server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});

    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [](kage::sync::ClientId, const kage::sync::BitBuffer&) {};
    kage::sync::ReplicationServer server(server_options);
    server.set_tracer(&server_tracer);
    REQUIRE(server.add_client(1));

    ecs::Registry client_registry;
    kage::sync::register_sync_component<kage_sync_tests::NetworkedPosition>(client_registry, "NetworkedPosition");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(client_registry));

    kage::sync::ReplicationClient client;
    REQUIRE(client.set_input(client_registry, kage_sync_tests::NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(client_registry, client.options().fixed_dt_seconds));
    std::vector<kage::sync::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != input_packets.end());

    REQUIRE(server.process_packet(server_registry, 1, *input_packet));
    server.tick(server_registry);
    server.tick(server_registry);
    server_tracer.set_frame_data_enabled(false);
    server.tick(server_registry);

    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent &&
            event.role == kage::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.frame == 1 &&
            event.data.find("x=50,y=60") != std::string::npos;
    }));
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent &&
            event.role == kage::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.frame == 2 &&
            event.data.find("x=50,y=60") != std::string::npos;
    }));
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent &&
            event.role == kage::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.frame == 3 &&
            event.data.empty();
    }));
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::InputStarved &&
            event.role == kage::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.frame == 2 &&
            event.data == "input_frame=1";
    }));
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::InputStarved &&
            event.role == kage::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.frame == 3 &&
            event.data == "input_frame=1";
    }));
}

TEST_CASE("server input tracing records starvation before any input arrives") {
    ecs::Registry server_registry;
    const ecs::Entity server_input_component =
        kage::sync::register_sync_component<kage_sync_tests::NetworkedPosition>(server_registry, "NetworkedPosition");
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(server_registry));

    const ecs::Entity owned = server_registry.create();
    REQUIRE(kage::sync::set_owner(server_registry, owned, 1));

    std::vector<kage::sync::SyncTraceEvent> server_events;
    kage::sync::SyncTracer server_tracer;
    server_tracer.set_frame_data_enabled(true);
    server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});

    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [](kage::sync::ClientId, const kage::sync::BitBuffer&) {};
    kage::sync::ReplicationServer server(server_options);
    server.set_tracer(&server_tracer);
    REQUIRE(server.add_client(1));

    server.tick(server_registry);

    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::InputStarved &&
            event.role == kage::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.component_name == "NetworkedPosition" &&
            event.frame == 1 &&
            event.data == "input_frame=0";
    }));
}

TEST_CASE("token client bootstraps input packets that server traces after first update") {
    ecs::Registry server_registry;
    const kage::sync::SyncArchetypeId server_archetype = define_networked_archetype(server_registry);
    const ecs::Entity server_input_component =
        server_registry.component<kage_sync_tests::NetworkedPosition>();
    kage::sync::configure_server(server_registry);
    REQUIRE(kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(server_registry));

    const ecs::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<kage_sync_tests::NetworkedPosition>(
                owned,
                kage_sync_tests::NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(kage::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    std::vector<kage::sync::BitBuffer> server_packets;
    std::vector<kage::sync::SyncTraceEvent> server_events;
    kage::sync::SyncTracer server_tracer;
    server_tracer.set_frame_data_enabled(true);
    server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});

    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
        server_packets.push_back(packet);
    };
    server_options.connect_handler = [](const std::string&, kage::sync::ClientId&, std::string&) {
        return true;
    };
    kage::sync::ReplicationServer server(server_options);
    server.set_tracer(&server_tracer);

    ecs::Registry client_registry;
    define_networked_archetype(client_registry);
    kage::sync::register_sync_component<kage::sync::NetworkOwner>(client_registry, "kage.sync.NetworkOwner");
    kage::sync::configure_client(client_registry, 1);
    REQUIRE(kage::sync::set_client_input_component<kage_sync_tests::NetworkedPosition>(client_registry));

    kage::sync::ReplicationClientOptions client_options;
    client_options.connect_token = "token";
    kage::sync::ReplicationClient client(client_options);
    REQUIRE(client.set_input(client_registry, kage_sync_tests::NetworkedPosition{5.0f, 6.0f}));

    std::vector<kage::sync::BitBuffer> client_packets = client.drain_packets();
    REQUIRE(client_packets.size() == 1);
    REQUIRE(server.process_packet(server_registry, 1, client_packets[0]));
    REQUIRE_FALSE(server_packets.empty());
    REQUIRE(client.receive(client_registry, server_packets.back()));
    server_packets.clear();

    client_packets = client.drain_packets();
    REQUIRE(std::any_of(client_packets.begin(), client_packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_connect_ack_message;
    }));
    for (const kage::sync::BitBuffer& packet : client_packets) {
        REQUIRE(server.process_packet(server_registry, 1, packet));
    }

    server.tick(server_registry);
    REQUIRE_FALSE(server_packets.empty());
    for (const kage::sync::BitBuffer& packet : server_packets) {
        (void)client.receive(client_registry, packet);
    }
    server_packets.clear();

    client_packets = client.drain_packets();
    const auto input_packet = std::find_if(client_packets.begin(), client_packets.end(), [](kage::sync::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != client_packets.end());
    REQUIRE(server.process_packet(server_registry, 1, *input_packet));

    server.tick(server_registry);
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const kage::sync::SyncTraceEvent& event) {
        return event.type == kage::sync::SyncTraceEventType::FrameComponent &&
            event.role == kage::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.component_name == "NetworkedPosition" &&
            event.data.find("x=50,y=60") != std::string::npos;
    }));
}

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

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
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

    std::vector<kage::sync::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const kage::sync::BitBuffer& packet) {
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
