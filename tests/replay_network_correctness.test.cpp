#include "ashiato/sync/sync.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace replay_network_tests {

struct ReplayVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ReplayTransform {
    ReplayVec3 position;
};

struct ReplayInput {
    float move_x = 0.0f;
};

struct ReplayDeathState {
    std::uint8_t dead = 0;
};

struct ShootingCue {
    std::int32_t id = 0;
};

struct PlayedShootingCue {
    std::int32_t id = 0;
    ashiato::sync::SyncFrame frame = 0;
};

struct ReplayCuePlayback {
    std::vector<PlayedShootingCue> played;
};

struct EntitySnapshot {
    ReplayTransform transform;
    ReplayDeathState death;
};

struct FrameSnapshot {
    EntitySnapshot killer;
    EntitySnapshot victim;
};

struct ExpectedCue {
    std::int32_t id = 0;
    ashiato::sync::SyncFrame frame = 0;
};

constexpr double fixed_dt = 1.0 / 60.0;
constexpr ashiato::sync::ClientId replay_client_id = 7;
constexpr ashiato::sync::SyncFrame death_frame = 48;
constexpr ashiato::sync::SyncFrame write_interval = 4;
constexpr ashiato::sync::SyncFrame full_frame_interval = 16;
constexpr ashiato::sync::SyncFrame preroll_frames = 24;
constexpr ashiato::sync::SyncFrame tail_frames = 32;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle invalid_socket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle invalid_socket = -1;
#endif

class SocketRuntime {
public:
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif
    }

    ~SocketRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

void close_socket(SocketHandle socket) {
    if (socket == invalid_socket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

void set_nonblocking(SocketHandle socket) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
#endif
}

SocketHandle make_udp_socket(std::uint16_t port) {
    SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket) {
        throw std::runtime_error("failed to create UDP socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(socket);
        throw std::runtime_error("failed to bind UDP socket");
    }
    set_nonblocking(socket);
    return socket;
}

std::uint16_t bound_port(SocketHandle socket) {
    sockaddr_in address{};
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        throw std::runtime_error("getsockname failed");
    }
    return ntohs(address.sin_port);
}

sockaddr_in loopback_address(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

ashiato::sync::PeerId peer_id(const sockaddr_in& address) {
    return (static_cast<ashiato::sync::PeerId>(ntohs(address.sin_port)) << 32U) |
        static_cast<ashiato::sync::PeerId>(ntohl(address.sin_addr.s_addr));
}

void send_packet(SocketHandle socket, const sockaddr_in& target, const ashiato::BitBuffer& packet) {
    const auto* data = reinterpret_cast<const char*>(packet.data());
    (void)sendto(
        socket,
        data,
        static_cast<int>(packet.byte_size()),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target));
}

bool receive_packet(SocketHandle socket, ashiato::BitBuffer& packet, sockaddr_in* sender = nullptr) {
    std::array<char, 2048> bytes{};
    sockaddr_in source{};
#ifdef _WIN32
    int source_size = sizeof(source);
#else
    socklen_t source_size = sizeof(source);
#endif
    const int received = recvfrom(
        socket,
        bytes.data(),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<sockaddr*>(&source),
        &source_size);
    if (received <= 0) {
        return false;
    }
    packet.clear();
    packet.write_bytes(bytes.data(), static_cast<std::size_t>(received));
    if (sender != nullptr) {
        *sender = source;
    }
    return true;
}

std::uint8_t packet_message(ashiato::BitBuffer packet) {
    return packet.remaining_bits() >= ashiato::sync::protocol::message_bits
        ? static_cast<std::uint8_t>(packet.read_bits(ashiato::sync::protocol::message_bits))
        : std::numeric_limits<std::uint8_t>::max();
}

ashiato::sync::SyncFrame update_packet_frame(ashiato::BitBuffer packet) {
    if (packet_message(packet) != ashiato::sync::protocol::server_update_message) {
        return 0U;
    }
    (void)packet.read_bits(ashiato::sync::protocol::message_bits);
    return static_cast<ashiato::sync::SyncFrame>(packet.read_bits(32U));
}

ashiato::sync::SyncArchetypeId define_replay_schema(ashiato::Registry& registry) {
    const ashiato::Entity transform =
        ashiato::sync::register_sync_component<ReplayTransform>(registry, "ReplayTransform");
    const ashiato::Entity death =
        ashiato::sync::register_sync_component<ReplayDeathState>(registry, "ReplayDeathState");
    registry.register_component<ReplayInput>("ReplayInput");
    registry.register_component<ReplayCuePlayback>("ReplayCuePlayback");
    (void)ashiato::sync::register_sync_cue<ShootingCue>(registry);
    return ashiato::sync::define_archetype(
        registry,
        "ReplayActor",
        {
            {transform, ashiato::sync::ReplicationAudience::All},
            {death, ashiato::sync::ReplicationAudience::All},
        });
}

void register_replay_jobs(ashiato::Registry& registry) {
    registry.job<ReplayTransform, const ReplayInput>(0).single_thread().each(
        [](ashiato::Entity, ReplayTransform& transform, const ReplayInput& input) {
            transform.position.x += input.move_x;
        });
}

ashiato::Entity spawn_actor(
    ashiato::Registry& registry,
    ashiato::sync::SyncArchetypeId archetype,
    ReplayVec3 position,
    bool with_input) {
    const ashiato::Entity entity = registry.create();
    registry.add<ReplayTransform>(entity, ReplayTransform{position});
    registry.add<ReplayDeathState>(entity, ReplayDeathState{});
    if (with_input) {
        registry.add<ReplayInput>(entity, ReplayInput{});
    }
    registry.add<ashiato::sync::Replicated>(entity, ashiato::sync::Replicated{archetype});
    return entity;
}

FrameSnapshot snapshot_frame(ashiato::Registry& registry, ashiato::Entity killer, ashiato::Entity victim) {
    return FrameSnapshot{
        EntitySnapshot{registry.get<ReplayTransform>(killer), registry.get<ReplayDeathState>(killer)},
        EntitySnapshot{registry.get<ReplayTransform>(victim), registry.get<ReplayDeathState>(victim)},
    };
}

std::vector<ExpectedCue> expected_cues_in_window(
    const std::vector<ExpectedCue>& emitted,
    ashiato::sync::SyncFrame start_frame,
    ashiato::sync::SyncFrame end_frame) {
    std::vector<ExpectedCue> expected;
    for (ExpectedCue cue : emitted) {
        if (cue.frame >= start_frame && cue.frame <= end_frame) {
            expected.push_back(cue);
        }
    }
    return expected;
}

ashiato::Entity find_actor_by_z(ashiato::Registry& registry, float z) {
    ashiato::Entity found;
    registry.view<const ReplayTransform>().each([&](ashiato::Entity entity, const ReplayTransform& transform) {
        if (!found && transform.position.z == z) {
            found = entity;
        }
    });
    return found;
}

void require_snapshot_matches(
    ashiato::Registry& registry,
    ashiato::Entity killer,
    ashiato::Entity victim,
    const FrameSnapshot& expected) {
    REQUIRE(killer);
    REQUIRE(victim);
    const ReplayTransform& killer_transform = registry.get<ReplayTransform>(killer);
    const ReplayTransform& victim_transform = registry.get<ReplayTransform>(victim);
    const ReplayDeathState& killer_death = registry.get<ReplayDeathState>(killer);
    const ReplayDeathState& victim_death = registry.get<ReplayDeathState>(victim);

    REQUIRE(killer_transform.position.x == Catch::Approx(expected.killer.transform.position.x));
    REQUIRE(killer_transform.position.y == Catch::Approx(expected.killer.transform.position.y));
    REQUIRE(killer_transform.position.z == Catch::Approx(expected.killer.transform.position.z));
    REQUIRE(victim_transform.position.x == Catch::Approx(expected.victim.transform.position.x));
    REQUIRE(victim_transform.position.y == Catch::Approx(expected.victim.transform.position.y));
    REQUIRE(victim_transform.position.z == Catch::Approx(expected.victim.transform.position.z));
    REQUIRE(killer_death.dead == expected.killer.death.dead);
    REQUIRE(victim_death.dead == expected.victim.death.dead);
}

struct RecordedReplay {
    ashiato::sync::ReplicationReplayStreamer streamer;
    std::map<ashiato::sync::SyncFrame, FrameSnapshot> snapshots;
    std::vector<ExpectedCue> emitted_cues;
};

RecordedReplay build_recorded_replay() {
    ashiato::Registry registry;
    const ashiato::sync::SyncArchetypeId archetype = define_replay_schema(registry);
    register_replay_jobs(registry);

    const ashiato::Entity killer = spawn_actor(registry, archetype, ReplayVec3{0.0f, 0.0f, 0.0f}, true);
    const ashiato::Entity victim = spawn_actor(registry, archetype, ReplayVec3{3.0f, 0.0f, 10.0f}, false);

    ashiato::sync::ReplicationServerOptions server_options;
    server_options.fixed_dt_seconds = fixed_dt;
    ashiato::sync::ReplicationServer server(registry, server_options);

    RecordedReplay recorded{
        ashiato::sync::ReplicationReplayStreamer({128, preroll_frames, tail_frames}),
        {},
        {},
    };

    ashiato::sync::ReplicationReplayWriter writer({
        full_frame_interval,
        [&](ashiato::sync::ReplicationReplayFrame frame) {
            recorded.snapshots.emplace(frame.frame, snapshot_frame(registry, killer, victim));
            recorded.streamer.push_frame(std::move(frame));
        },
        write_interval});
    writer.attach(server);

    constexpr ashiato::sync::SyncFrame total_frames = death_frame + tail_frames + 16U;
    for (ashiato::sync::SyncFrame frame = 1; frame <= total_frames; ++frame) {
        registry.write<ReplayInput>(killer).move_x = frame < death_frame ? 0.25f : -0.25f;
        if ((frame % 8U) == 0U) {
            const std::int32_t cue_id = static_cast<std::int32_t>(frame / 8U);
            REQUIRE(registry.write<ashiato::sync::CueDispatcher>().emit(
                registry.get<ashiato::sync::SyncSettings>(),
                frame,
                killer,
                ShootingCue{cue_id},
                10.0f));
            recorded.emitted_cues.push_back(ExpectedCue{cue_id, frame});
        }
        if (frame == death_frame) {
            registry.write<ReplayDeathState>(victim).dead = 1U;
        }
        REQUIRE(server.tick(registry, fixed_dt));
    }
    writer.detach();
    REQUIRE(recorded.streamer.frame_count() > 0U);
    return recorded;
}

struct ReplayNetworkHarness {
    explicit ReplayNetworkHarness(const ashiato::sync::ReplicationReplayStreamer& replay_streamer)
        : streamer(replay_streamer),
          socket(make_udp_socket(0)),
          port(bound_port(socket)) {}

    ~ReplayNetworkHarness() {
        close_socket(socket);
    }

    const ashiato::sync::ReplicationReplayStreamer& streamer;
    SocketHandle socket = invalid_socket;
    std::uint16_t port = 0;
    sockaddr_in client_address{};
    ashiato::Registry registry;
    std::unique_ptr<ashiato::sync::ReplicationServer> server;
    ashiato::sync::ReplicationReplayStreamSession session;
    bool started = false;
    bool ready = false;

    void receive_client_packets() {
        ashiato::BitBuffer packet;
        sockaddr_in sender{};
        while (receive_packet(socket, packet, &sender)) {
            if (!started) {
                begin(sender);
            }
            const bool ack = packet_message(packet) == ashiato::sync::protocol::client_connect_ack_message;
            REQUIRE(server != nullptr);
            REQUIRE(server->process_packet(registry, peer_id(sender), std::move(packet)));
            if (ack) {
                ready = true;
            }
        }
    }

    void tick() {
        if (ready && session.active) {
            (void)streamer.tick_session(session, registry, *server);
        }
    }

    void begin(const sockaddr_in& sender) {
        client_address = sender;
        (void)define_replay_schema(registry);

        ashiato::sync::ReplicationServerOptions options;
        options.fixed_dt_seconds = fixed_dt;
        options.bandwidth_limit_bytes_per_tick = 64 * 1024;
        options.connect_handler = [](const std::string& token, ashiato::sync::ClientId& accepted, std::string&) {
            if (token != "killcam") {
                return false;
            }
            accepted = replay_client_id;
            return true;
        };
        options.transport = [this](ashiato::sync::PeerId, const ashiato::BitBuffer& packet) {
            send_packet(socket, client_address, packet);
        };
        server = std::make_unique<ashiato::sync::ReplicationServer>(registry, options);
        REQUIRE(streamer.begin_session(death_frame, registry, *server, session));
        started = true;
    }
};

}  // namespace replay_network_tests

namespace ashiato::sync {

template <>
struct SyncComponentTraits<replay_network_tests::ReplayTransform> {
    using Quantized = replay_network_tests::ReplayTransform;

    static Quantized quantize(const replay_network_tests::ReplayTransform& value) {
        return value;
    }

    static replay_network_tests::ReplayTransform dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.write_bytes(reinterpret_cast<const char*>(&current), sizeof(current));
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(out));
        return true;
    }
};

template <>
struct SyncComponentTraits<replay_network_tests::ReplayDeathState> {
    using Quantized = replay_network_tests::ReplayDeathState;

    static Quantized quantize(const replay_network_tests::ReplayDeathState& value) {
        return value;
    }

    static replay_network_tests::ReplayDeathState dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.write_bits(current.dead, 1U);
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
        out.dead = static_cast<std::uint8_t>(in.read_bits(1U));
        return true;
    }
};

template <>
struct SyncCueTraits<replay_network_tests::ShootingCue> {
    static void serialize(
        const replay_network_tests::ShootingCue& cue,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext& context) {
        ASHIATO_SERIALIZE_TRACE(out, cue.id, 16U, "id");
    }

    static bool deserialize(
        ashiato::BitBuffer& in,
        replay_network_tests::ShootingCue& out,
        ashiato::ComponentSerializationContext& context) {
        ASHIATO_SERIALIZATION_TRACE_SCOPE("id");
        out.id = static_cast<std::int32_t>(in.read_bits(16U));
        return true;
    }

    static bool play(
        ashiato::Registry& registry,
        ashiato::Entity owner,
        const replay_network_tests::ShootingCue& cue,
        float,
        SyncFrame frame) {
        if (!registry.contains<replay_network_tests::ReplayCuePlayback>(owner)) {
            registry.add<replay_network_tests::ReplayCuePlayback>(owner);
        }
        registry.write<replay_network_tests::ReplayCuePlayback>(owner)
            .played.push_back(replay_network_tests::PlayedShootingCue{cue.id, frame});
        return true;
    }

    static bool rollback(ashiato::Registry&, ashiato::Entity, const replay_network_tests::ShootingCue&) {
        return true;
    }

    static bool equals_cue(
        const replay_network_tests::ShootingCue& lhs,
        const replay_network_tests::ShootingCue& rhs) {
        return lhs.id == rhs.id;
    }
};

}  // namespace ashiato::sync

TEST_CASE("network replay smoke streams correct movement and cue frames") {
    using namespace replay_network_tests;

    SocketRuntime socket_runtime;
    try {
        SocketHandle probe_socket = make_udp_socket(0);
        close_socket(probe_socket);
    } catch (const std::runtime_error& ex) {
        SKIP(ex.what());
    }

    RecordedReplay recorded = build_recorded_replay();
    ReplayNetworkHarness harness(recorded.streamer);

    SocketHandle client_socket = make_udp_socket(0);
    const sockaddr_in replay_server_address = loopback_address(harness.port);

    ashiato::Registry client_registry;
    (void)define_replay_schema(client_registry);

    ashiato::sync::ReplicationClientOptions client_options;
    client_options.session.local_client = replay_client_id;
    client_options.session.connect_token = "killcam";
    client_options.entities.default_mode = ashiato::sync::ReplicationClientMode::BufferedInterpolation;
    client_options.clock.fixed_dt_seconds = fixed_dt;
    client_options.buffered.auto_buffered_frame_lag = false;
    client_options.buffered.buffered_frame_lag = 2;
    ashiato::sync::ReplicationClient client(client_registry, client_options);
    client.set_packet_sender([client_socket, replay_server_address](const ashiato::BitBuffer& packet) {
        send_packet(client_socket, replay_server_address, packet);
    });

    ashiato::Entity local_killer;
    ashiato::Entity local_victim;
    std::vector<ashiato::sync::SyncFrame> received_update_frames;
    std::vector<ashiato::sync::SyncFrame> checked_sample_frames;
    ashiato::sync::SyncFrame last_checked_applied_frame = 0;
    bool saw_rightward_sample = false;
    bool saw_leftward_sample = false;
    float previous_killer_x = 0.0f;
    bool have_previous_killer_x = false;
    ashiato::sync::SyncFrame observed_dead_frame = 0;

    for (int tick = 0; tick < 420; ++tick) {
        REQUIRE(client.tick(client_registry, fixed_dt));
        harness.receive_client_packets();
        harness.tick();

        ashiato::BitBuffer packet;
        while (receive_packet(client_socket, packet)) {
            const ashiato::sync::SyncFrame update_frame = update_packet_frame(packet);
            if (update_frame != 0U) {
                received_update_frames.push_back(update_frame);
            }
            client.receive_packet(std::move(packet));
        }

        REQUIRE(client.tick(client_registry, 0.0));
        if (!local_killer) {
            local_killer = find_actor_by_z(client_registry, 0.0f);
        }
        if (!local_victim) {
            local_victim = find_actor_by_z(client_registry, 10.0f);
        }

        if (client.has_applied_buffered_frame()) {
            const ashiato::sync::SyncFrame applied = client.last_applied_buffered_frame();
            const auto expected = recorded.snapshots.find(applied);
            if (expected != recorded.snapshots.end() && applied != last_checked_applied_frame) {
                if (!local_killer || !local_victim) {
                    continue;
                }
                require_snapshot_matches(client_registry, local_killer, local_victim, expected->second);
                checked_sample_frames.push_back(applied);
                last_checked_applied_frame = applied;

                const float killer_x = client_registry.get<ReplayTransform>(local_killer).position.x;
                if (have_previous_killer_x) {
                    if (applied < death_frame && killer_x > previous_killer_x) {
                        saw_rightward_sample = true;
                    }
                    if (applied > death_frame && killer_x < previous_killer_x) {
                        saw_leftward_sample = true;
                    }
                }
                previous_killer_x = killer_x;
                have_previous_killer_x = true;

                if (client_registry.get<ReplayDeathState>(local_victim).dead != 0U && observed_dead_frame == 0U) {
                    observed_dead_frame = applied;
                }
            }
        }

        if (harness.started && !harness.session.active && local_killer &&
            client_registry.contains<ReplayCuePlayback>(local_killer)) {
            const std::vector<ExpectedCue> expected_cues =
                expected_cues_in_window(
                    recorded.emitted_cues,
                    checked_sample_frames.empty() ? 0U : checked_sample_frames.front(),
                    checked_sample_frames.empty() ? 0U : checked_sample_frames.back());
            const ReplayCuePlayback& playback = client_registry.get<ReplayCuePlayback>(local_killer);
            if (checked_sample_frames.size() >= recorded.snapshots.size() / 2U &&
                playback.played.size() >= expected_cues.size()) {
                break;
            }
        }
    }

    close_socket(client_socket);

    REQUIRE(harness.started);
    REQUIRE(harness.ready);
    REQUIRE(client.connection_state() == ashiato::sync::ReplicationClientConnectionState::Ready);
    REQUIRE_FALSE(received_update_frames.empty());
    REQUIRE(std::is_sorted(received_update_frames.begin(), received_update_frames.end()));
    REQUIRE(local_killer);
    REQUIRE(local_victim);
    REQUIRE_FALSE(checked_sample_frames.empty());
    REQUIRE(saw_rightward_sample);
    REQUIRE(saw_leftward_sample);
    REQUIRE(observed_dead_frame >= death_frame);

    const ashiato::sync::SyncFrame session_start = checked_sample_frames.front();
    const std::vector<ExpectedCue> expected_cues =
        expected_cues_in_window(recorded.emitted_cues, session_start, checked_sample_frames.back());
    REQUIRE(client_registry.contains<ReplayCuePlayback>(local_killer));
    const ReplayCuePlayback& playback = client_registry.get<ReplayCuePlayback>(local_killer);
    REQUIRE(playback.played.size() == expected_cues.size());
    for (std::size_t index = 0; index < expected_cues.size(); ++index) {
        REQUIRE(playback.played[index].id == expected_cues[index].id);
        REQUIRE(playback.played[index].frame == expected_cues[index].frame);
    }
}
