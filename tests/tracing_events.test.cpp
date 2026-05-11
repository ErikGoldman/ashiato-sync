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

namespace {

struct AnonymousCueNameProbe {};

}  // namespace

TEST_CASE("sync cue default type names omit anonymous namespace qualifiers") {
    REQUIRE(ashiato::sync::detail::default_type_name<AnonymousCueNameProbe>() == "AnonymousCueNameProbe");
}

TEST_CASE("sync tracing records server send client receive and apply events") {
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
    std::vector<ashiato::sync::SyncTraceEvent> server_events;
    ashiato::sync::SyncTracer server_tracer;
    server_tracer.set_frame_data_enabled(true);
    server_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
    server.set_tracer(&server_tracer);

    REQUIRE(server.add_client(1));
    REQUIRE(start_sync(server_registry, server_entity, server_archetype));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(packets.size() == 1);
    REQUIRE(has_event(server_events, ashiato::sync::SyncTraceEventType::ClientConnected));
    REQUIRE(has_event(server_events, ashiato::sync::SyncTraceEventType::EntityStartedSyncing));
    REQUIRE(has_event(server_events, ashiato::sync::SyncTraceEventType::ComponentSent));
    REQUIRE(has_event(server_events, ashiato::sync::SyncTraceEventType::FrameComponent));

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
    REQUIRE(has_event(client_events, ashiato::sync::SyncTraceEventType::EntityReceived));
    REQUIRE(has_event(client_events, ashiato::sync::SyncTraceEventType::ComponentReceived));
    REQUIRE_FALSE(has_event(client_events, ashiato::sync::SyncTraceEventType::ComponentApplied));
}

TEST_CASE("cue tracing records lifecycle events and uses frame data gating") {
    auto run_case = [](bool frame_data_enabled) {
        ashiato::Registry server_registry;
        const ashiato::sync::SyncArchetypeId server_archetype =
            ashiato_sync_tests::define_position_archetype(server_registry);
        const ashiato::sync::SyncCueTypeId cue_type =
            ashiato::sync::register_sync_cue<ashiato_sync_tests::TestCue>(server_registry);
        const ashiato::Entity server_entity = server_registry.create();
        REQUIRE(server_registry.add<ashiato_sync_tests::Position>(
            server_entity,
            ashiato_sync_tests::Position{1.0f, 2.0f}) != nullptr);

        std::vector<ashiato::BitBuffer> packets;
        ashiato::sync::ReplicationServerOptions server_options;
        server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
            packets.push_back(packet);
        };
        ashiato::sync::ReplicationServer server(server_registry, server_options);
        std::vector<ashiato::sync::SyncTraceEvent> server_events;
        ashiato::sync::SyncTracer server_tracer;
        server_tracer.set_frame_data_enabled(frame_data_enabled);
        server_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
            [&](const ashiato::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
        server.set_tracer(&server_tracer);

        REQUIRE(server.add_client(1));
        REQUIRE(start_sync(server_registry, server_entity, server_archetype));
        REQUIRE(ashiato_sync_tests::emit_test_cue(server_registry, server_entity, 1, ashiato_sync_tests::TestCue{7}, 1.0f));
        server.tick(server_registry, server.options().fixed_dt_seconds);
        REQUIRE(packets.size() == 1);
        REQUIRE(has_cue_event(server_events, ashiato::sync::SyncTraceEventType::CueEmitted, cue_type));
        REQUIRE(has_cue_event(server_events, ashiato::sync::SyncTraceEventType::CueSent, cue_type));

        ashiato::Registry client_registry;
        const ashiato::sync::SyncArchetypeId client_archetype =
            ashiato_sync_tests::define_position_archetype(client_registry);
        REQUIRE(client_archetype == server_archetype);
        client_registry.register_component<ashiato_sync_tests::CuePlayback>("CuePlayback");
        REQUIRE(ashiato::sync::register_sync_cue<ashiato_sync_tests::TestCue>(client_registry) == cue_type);
        ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
        std::vector<ashiato::sync::SyncTraceEvent> client_events;
        ashiato::sync::SyncTracer client_tracer;
        client_tracer.set_frame_data_enabled(frame_data_enabled);
        client_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
            [&](const ashiato::sync::SyncTraceEvent& event) { client_events.push_back(event); }});
        ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
        client.set_tracer(&client_tracer);

        REQUIRE(client.receive(client_registry, packets[0]));
        REQUIRE(has_cue_event(client_events, ashiato::sync::SyncTraceEventType::CueReceived, cue_type));
        REQUIRE(has_cue_event(client_events, ashiato::sync::SyncTraceEventType::CuePlayed, cue_type));
        REQUIRE_FALSE(has_cue_event(client_events, ashiato::sync::SyncTraceEventType::CueConfirmed, cue_type));
        return std::make_pair(std::move(server_events), std::move(client_events));
    };

    auto without_data = run_case(false);
    REQUIRE_FALSE(has_cue_event_data(without_data.first, ashiato::sync::SyncTraceEventType::CueEmitted, "id=7"));
    REQUIRE_FALSE(has_cue_event_data(without_data.first, ashiato::sync::SyncTraceEventType::CueSent, "id=7"));
    REQUIRE_FALSE(has_cue_event_data(without_data.second, ashiato::sync::SyncTraceEventType::CueReceived, "id=7"));
    REQUIRE_FALSE(has_cue_event_data(without_data.second, ashiato::sync::SyncTraceEventType::CuePlayed, "id=7"));

    auto with_data = run_case(true);
#ifdef ASHIATO_SYNC_TRACE_COMPONENT_DATA
    REQUIRE(has_cue_event_data(with_data.first, ashiato::sync::SyncTraceEventType::CueEmitted, "id=7"));
    REQUIRE(has_cue_event_data(with_data.first, ashiato::sync::SyncTraceEventType::CueSent, "id=7"));
    REQUIRE(has_cue_event_data(with_data.second, ashiato::sync::SyncTraceEventType::CueReceived, "id=7"));
    REQUIRE(has_cue_event_data(with_data.second, ashiato::sync::SyncTraceEventType::CuePlayed, "id=7"));
#else
    (void)with_data;
#endif
}

TEST_CASE("server transport traces job-emitted cues at authoritative snapshot frame") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype =
        ashiato_sync_tests::define_position_archetype(server_registry);
    const ashiato::sync::SyncCueTypeId cue_type =
        ashiato::sync::register_sync_cue<ashiato_sync_tests::TestCue>(server_registry);
    const ashiato::Entity server_entity = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::Position>(
        server_entity,
        ashiato_sync_tests::Position{1.0f, 2.0f}) != nullptr);

    bool fired = false;
    server_registry.job<
        ashiato_sync_tests::Position,
        ashiato::sync::SyncSettings,
        ashiato::sync::FrameInfo,
        ashiato::sync::CueDispatcher>(0).each(
        [&](ashiato::Entity entity,
            ashiato_sync_tests::Position&,
            ashiato::sync::SyncSettings& settings,
            ashiato::sync::FrameInfo& frame,
            ashiato::sync::CueDispatcher& cues) {
            if (entity == server_entity && !fired) {
                REQUIRE(cues.emit(settings, frame, entity, ashiato_sync_tests::TestCue{7}, 1.0f));
                fired = true;
            }
        });

    std::vector<ashiato::BitBuffer> packets;
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        packets.push_back(packet);
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    std::vector<ashiato::sync::SyncTraceEvent> server_events;
    ashiato::sync::SyncTracer server_tracer;
    server_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
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
        [cue_type](const ashiato::sync::SyncTraceEvent& event) {
            return event.type == ashiato::sync::SyncTraceEventType::CueEmitted && event.cue_type == cue_type;
        });
    REQUIRE(emitted != server_events.end());
    REQUIRE(emitted->frame == 1);

    const auto sent = std::find_if(
        server_events.begin(),
        server_events.end(),
        [cue_type](const ashiato::sync::SyncTraceEvent& event) {
            return event.type == ashiato::sync::SyncTraceEventType::CueSent && event.cue_type == cue_type;
        });
    REQUIRE(sent != server_events.end());
    REQUIRE(sent->frame == 1);
}

TEST_CASE("client tracing records clock skew timing decisions") {
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
    REQUIRE(ashiato::sync::set_client_input_component<ashiato_sync_tests::NetworkedPosition>(client_registry));

    std::vector<ashiato::sync::SyncTraceEvent> client_events;
    ashiato::sync::SyncTracer client_tracer;
    client_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { client_events.push_back(event); }});

    ashiato::sync::ReplicationClientOptions options;
    options.prediction.input_buffer_capacity_frames = 32;
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, options));
    client.set_tracer(&client_tracer);
    REQUIRE(client.set_input(client_registry, ashiato_sync_tests::NetworkedPosition{3.0f, 4.0f}));
    REQUIRE(receive_at_local_frame(client, client_registry, packets[0], 8));

    const auto event = std::find_if(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& trace) {
        return trace.type == ashiato::sync::SyncTraceEventType::ClockSkew &&
            trace.data.find("stage=clock_requested_prefill") != std::string::npos &&
            trace.data.find("observed_downstream=") != std::string::npos &&
            trace.data.find("prediction_target=") != std::string::npos &&
            trace.data.find("prefill_input=") != std::string::npos;
    });
    REQUIRE(event != client_events.end());
    REQUIRE(event->component_name == "Clock");
}

TEST_CASE("client input tracing records input components every input tick") {
    ashiato::Registry registry;
    const ashiato::Entity input_component =
        ashiato::sync::register_sync_component<ashiato_sync_tests::NetworkedPosition>(registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<ashiato_sync_tests::NetworkedPosition>(registry));

    const ashiato::Entity owned = registry.create();
    REQUIRE(ashiato::sync::set_owner(registry, owned, 1));

    std::vector<ashiato::sync::SyncTraceEvent> events;
    ashiato::sync::SyncTracer tracer;
    tracer.set_frame_data_enabled(true);
    tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { events.push_back(event); }});

    ashiato::sync::ReplicationClient client(registry, ashiato_sync_tests::make_test_client_options(registry, {}));
    client.set_tracer(&tracer);
    REQUIRE(client.set_input(registry, ashiato_sync_tests::NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(registry, client.fixed_dt_seconds()));

    const auto input_event = std::find_if(events.begin(), events.end(), [&](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent &&
            event.role == ashiato::sync::SyncTraceRole::Client &&
            event.local_entity == owned &&
            event.component == input_component &&
            event.frame == 1;
    });
    REQUIRE(input_event != events.end());
}

#ifdef ASHIATO_SYNC_TRACE_PACKET_LOGS

TEST_CASE("packet log tracing records client input baseline and server received input range") {
    ashiato::Registry client_registry;
    ashiato::sync::register_sync_component<ashiato_sync_tests::NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<ashiato_sync_tests::NetworkedPosition>(client_registry));
    const ashiato::Entity client_owned = client_registry.create();
    REQUIRE(ashiato::sync::set_owner(client_registry, client_owned, 1));

    std::vector<ashiato::sync::SyncTraceEvent> client_events;
    ashiato::sync::SyncTracer client_tracer;
    client_tracer.set_packet_logs_enabled(true);
    client_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { client_events.push_back(event); }});
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    client.set_tracer(&client_tracer);
    REQUIRE(client.set_input(client_registry, ashiato_sync_tests::NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    std::vector<ashiato::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != input_packets.end());
    REQUIRE(std::any_of(client_events.begin(), client_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::PacketLog &&
            event.data.find("message=client_input") != std::string::npos &&
            event.data.find("input_frames=1-1") != std::string::npos &&
            event.data.find("baseline=0") != std::string::npos;
    }));

    ashiato::Registry server_registry;
    ashiato::sync::register_sync_component<ashiato_sync_tests::NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<ashiato_sync_tests::NetworkedPosition>(server_registry));
    const ashiato::Entity server_owned = server_registry.create();
    REQUIRE(ashiato::sync::set_owner(server_registry, server_owned, 1));

    std::vector<ashiato::sync::SyncTraceEvent> server_events;
    ashiato::sync::SyncTracer server_tracer;
    server_tracer.set_packet_logs_enabled(true);
    server_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { server_events.push_back(event); }});
    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    server.set_tracer(&server_tracer);
    REQUIRE(server.add_client(1));
    REQUIRE(server.process_packet(server_registry, 1, *input_packet));
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::PacketLog &&
            event.role == ashiato::sync::SyncTraceRole::Server &&
            event.data.find("message=client_input") != std::string::npos &&
            event.data.find("input_frames=1-1") != std::string::npos &&
            event.data.find("baseline=0") != std::string::npos;
    }));
}
#endif

TEST_CASE("server input tracing records input components and stale input starvation") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_input_component =
        ashiato::sync::register_sync_component<ashiato_sync_tests::NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<ashiato_sync_tests::NetworkedPosition>(server_registry));

    const ashiato::Entity owned = server_registry.create();
    REQUIRE(ashiato::sync::set_owner(server_registry, owned, 1));

    std::vector<ashiato::sync::SyncTraceEvent> server_events;
    ashiato::sync::SyncTracer server_tracer;
    server_tracer.set_frame_data_enabled(true);
    server_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { server_events.push_back(event); }});

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    server.set_tracer(&server_tracer);
    REQUIRE(server.add_client(1));

    ashiato::Registry client_registry;
    ashiato::sync::register_sync_component<ashiato_sync_tests::NetworkedPosition>(client_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<ashiato_sync_tests::NetworkedPosition>(client_registry));

    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, {}));
    REQUIRE(client.set_input(client_registry, ashiato_sync_tests::NetworkedPosition{5.0f, 6.0f}));
    REQUIRE(client.tick(client_registry, client.fixed_dt_seconds()));
    std::vector<ashiato::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != input_packets.end());

    REQUIRE(server.process_packet(server_registry, 1, *input_packet));
    server.tick(server_registry, server.options().fixed_dt_seconds);
    server.tick(server_registry, server.options().fixed_dt_seconds);
    server_tracer.set_frame_data_enabled(false);
    server.tick(server_registry, server.options().fixed_dt_seconds);

    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent &&
            event.role == ashiato::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.frame == 1 &&
            event.data.find("x=50,y=60") != std::string::npos;
    }));
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent &&
            event.role == ashiato::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.frame == 2 &&
            event.data.find("x=50,y=60") != std::string::npos;
    }));
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent &&
            event.role == ashiato::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.frame == 3 &&
            event.data.empty();
    }));
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::InputStarved &&
            event.role == ashiato::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.frame == 2 &&
            event.data == "input_frame=1";
    }));
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::InputStarved &&
            event.role == ashiato::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.frame == 3 &&
            event.data == "input_frame=1";
    }));
}

TEST_CASE("server input tracing records starvation before any input arrives") {
    ashiato::Registry server_registry;
    const ashiato::Entity server_input_component =
        ashiato::sync::register_sync_component<ashiato_sync_tests::NetworkedPosition>(server_registry, "NetworkedPosition");
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<ashiato_sync_tests::NetworkedPosition>(server_registry));

    const ashiato::Entity owned = server_registry.create();
    REQUIRE(ashiato::sync::set_owner(server_registry, owned, 1));

    std::vector<ashiato::sync::SyncTraceEvent> server_events;
    ashiato::sync::SyncTracer server_tracer;
    server_tracer.set_frame_data_enabled(true);
    server_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { server_events.push_back(event); }});

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [](ashiato::sync::ClientId, const ashiato::BitBuffer&) {};
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    server.set_tracer(&server_tracer);
    REQUIRE(server.add_client(1));

    server.tick(server_registry, server.options().fixed_dt_seconds);

    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::InputStarved &&
            event.role == ashiato::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.component_name == "NetworkedPosition" &&
            event.frame == 1 &&
            event.data == "input_frame=0";
    }));
}

TEST_CASE("token client bootstraps input packets that server traces after first update") {
    ashiato::Registry server_registry;
    const ashiato::sync::SyncArchetypeId server_archetype = define_networked_archetype(server_registry);
    const ashiato::Entity server_input_component =
        server_registry.component<ashiato_sync_tests::NetworkedPosition>();
    ashiato_sync_tests::configure_test_server_registry(server_registry);
    REQUIRE(ashiato::sync::set_client_input_component<ashiato_sync_tests::NetworkedPosition>(server_registry));

    const ashiato::Entity owned = server_registry.create();
    REQUIRE(server_registry.add<ashiato_sync_tests::NetworkedPosition>(
                owned,
                ashiato_sync_tests::NetworkedPosition{1.0f, 2.0f}) != nullptr);
    REQUIRE(ashiato::sync::set_owner(server_registry, owned, 1));
    REQUIRE(start_sync(server_registry, owned, server_archetype));

    std::vector<ashiato::BitBuffer> server_packets;
    std::vector<ashiato::sync::SyncTraceEvent> server_events;
    ashiato::sync::SyncTracer server_tracer;
    server_tracer.set_frame_data_enabled(true);
    server_tracer.set_callbacks(ashiato::sync::SyncTraceCallbacks{
        [&](const ashiato::sync::SyncTraceEvent& event) { server_events.push_back(event); }});

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](ashiato::sync::ClientId, const ashiato::BitBuffer& packet) {
        server_packets.push_back(packet);
    };
    server_options.connect_handler = [](const std::string&, ashiato::sync::ClientId&, std::string&) {
        return true;
    };
    ashiato::sync::ReplicationServer server(server_registry, server_options);
    server.set_tracer(&server_tracer);

    ashiato::Registry client_registry;
    define_networked_archetype(client_registry);
    ashiato::sync::register_sync_component<ashiato::sync::NetworkOwner>(client_registry, "ashiato.sync.NetworkOwner");
    ashiato_sync_tests::configure_test_client_registry(client_registry, 1);
    REQUIRE(ashiato::sync::set_client_input_component<ashiato_sync_tests::NetworkedPosition>(client_registry));

    ashiato::sync::ReplicationClientOptions client_options;
    client_options.session.connect_token = "token";
    ashiato::sync::ReplicationClient client(client_registry, ashiato_sync_tests::make_test_client_options(client_registry, client_options));
    REQUIRE(client.set_input(client_registry, ashiato_sync_tests::NetworkedPosition{5.0f, 6.0f}));

    std::vector<ashiato::BitBuffer> client_packets = client.drain_packets();
    REQUIRE(client_packets.size() == 1);
    REQUIRE(server.process_packet(server_registry, 1, client_packets[0]));
    REQUIRE_FALSE(server_packets.empty());
    REQUIRE(client.receive(client_registry, server_packets.back()));
    server_packets.clear();

    client_packets = client.drain_packets();
    REQUIRE(std::any_of(client_packets.begin(), client_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_connect_ack_message;
    }));
    for (const ashiato::BitBuffer& packet : client_packets) {
        REQUIRE(server.process_packet(server_registry, 1, packet));
    }

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE_FALSE(server_packets.empty());
    for (const ashiato::BitBuffer& packet : server_packets) {
        (void)client.receive(client_registry, packet);
    }
    server_packets.clear();

    client_packets = client.drain_packets();
    const auto input_packet = std::find_if(client_packets.begin(), client_packets.end(), [](ashiato::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == ashiato::sync::protocol::client_input_message;
    });
    REQUIRE(input_packet != client_packets.end());
    REQUIRE(server.process_packet(server_registry, 1, *input_packet));

    server.tick(server_registry, server.options().fixed_dt_seconds);
    REQUIRE(std::any_of(server_events.begin(), server_events.end(), [&](const ashiato::sync::SyncTraceEvent& event) {
        return event.type == ashiato::sync::SyncTraceEventType::FrameComponent &&
            event.role == ashiato::sync::SyncTraceRole::Server &&
            event.server_entity == owned &&
            event.component == server_input_component &&
            event.component_name == "NetworkedPosition" &&
            event.data.find("x=50,y=60") != std::string::npos;
    }));
}

#endif
