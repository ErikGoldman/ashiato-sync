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

namespace {

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

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
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

        std::vector<ecs::BitBuffer> packets;
        kage::sync::ReplicationServerOptions server_options;
        server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
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
    server_registry.job<kage_sync_tests::Position, kage::sync::SyncSettings>(0).each(
        [&](ecs::Entity entity, kage_sync_tests::Position&, kage::sync::SyncSettings& settings) {
            if (entity == server_entity && !fired) {
                REQUIRE(kage::sync::emit_cue(settings, entity, kage_sync_tests::TestCue{7}, 1.0f));
                fired = true;
            }
        });

    std::vector<ecs::BitBuffer> packets;
    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
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
    std::vector<ecs::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](ecs::BitBuffer packet) {
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
    server_options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
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
    server_options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
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
    std::vector<ecs::BitBuffer> input_packets = client.drain_packets();
    auto input_packet = std::find_if(input_packets.begin(), input_packets.end(), [](ecs::BitBuffer packet) {
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
    server_options.transport = [](kage::sync::ClientId, const ecs::BitBuffer&) {};
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

    std::vector<ecs::BitBuffer> server_packets;
    std::vector<kage::sync::SyncTraceEvent> server_events;
    kage::sync::SyncTracer server_tracer;
    server_tracer.set_frame_data_enabled(true);
    server_tracer.set_callbacks(kage::sync::SyncTraceCallbacks{
        [&](const kage::sync::SyncTraceEvent& event) { server_events.push_back(event); }});

    kage::sync::ReplicationServerOptions server_options;
    server_options.transport = [&](kage::sync::ClientId, const ecs::BitBuffer& packet) {
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

    std::vector<ecs::BitBuffer> client_packets = client.drain_packets();
    REQUIRE(client_packets.size() == 1);
    REQUIRE(server.process_packet(server_registry, 1, client_packets[0]));
    REQUIRE_FALSE(server_packets.empty());
    REQUIRE(client.receive(client_registry, server_packets.back()));
    server_packets.clear();

    client_packets = client.drain_packets();
    REQUIRE(std::any_of(client_packets.begin(), client_packets.end(), [](ecs::BitBuffer packet) {
        return static_cast<std::uint8_t>(packet.read_bits(8U)) == kage::sync::protocol::client_connect_ack_message;
    }));
    for (const ecs::BitBuffer& packet : client_packets) {
        REQUIRE(server.process_packet(server_registry, 1, packet));
    }

    server.tick(server_registry);
    REQUIRE_FALSE(server_packets.empty());
    for (const ecs::BitBuffer& packet : server_packets) {
        (void)client.receive(client_registry, packet);
    }
    server_packets.clear();

    client_packets = client.drain_packets();
    const auto input_packet = std::find_if(client_packets.begin(), client_packets.end(), [](ecs::BitBuffer packet) {
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

#endif
