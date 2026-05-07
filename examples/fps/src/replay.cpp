#include "replay.hpp"

#include "game/components.hpp"
#include "game/cues.hpp"
#include "game/schema.hpp"
#include "game/sync_traits.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <utility>

namespace fps {
namespace {

void write_u32(std::ostream& out, std::uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_u64(std::ostream& out, std::uint64_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

constexpr std::uint32_t replay_frame_magic = 0x52535046U;
constexpr std::uint32_t replay_frame_version = 3U;
constexpr std::uint8_t replay_done_message = 200U;
constexpr std::uint8_t replay_target_message = 201U;
constexpr kage::sync::SyncFrame replay_sample_stride_frames = 4U;
constexpr kage::sync::SyncFrame replay_interpolation_buffer_frames = replay_sample_stride_frames * 2U;
constexpr kage::sync::SyncFrame replay_frames_for_seconds(float seconds) {
    return static_cast<kage::sync::SyncFrame>(seconds / fixed_dt + 0.5f);
}

constexpr float replay_transfer_budget_seconds = 0.5f;
constexpr kage::sync::SyncFrame replay_preroll_frames = replay_frames_for_seconds(3.0f);
constexpr kage::sync::SyncFrame replay_total_frames =
    replay_frames_for_seconds(respawn_seconds - replay_transfer_budget_seconds);
constexpr kage::sync::SyncFrame replay_tail_frames =
    replay_total_frames > replay_preroll_frames ? replay_total_frames - replay_preroll_frames : 0U;
constexpr double replay_done_resend_seconds = 1.0;
constexpr double replay_session_timeout_seconds = 8.0;
constexpr float replay_client_timeout_seconds = 8.0f;
constexpr float replay_done_client_drain_seconds = 0.35f;

std::string replay_token(kage::sync::ClientId client) {
    return "fps-replay:" + std::to_string(static_cast<unsigned long long>(client));
}

bool parse_replay_token(const std::string& token, kage::sync::ClientId& out) {
    const std::string prefix = "fps-replay:";
    if (token.compare(0U, prefix.size(), prefix) != 0) {
        return false;
    }
    try {
        out = static_cast<kage::sync::ClientId>(std::stoull(token.substr(prefix.size())));
        return true;
    } catch (...) {
        return false;
    }
}

void send_replay_done(SocketHandle socket, const sockaddr_in& target) {
    ecs::BitBuffer packet;
    packet.push_bits(replay_done_message, 8U);
    send_packet(socket, target, packet);
}

void send_replay_target(SocketHandle socket, const sockaddr_in& target, std::uint64_t player_id) {
    ecs::BitBuffer packet;
    packet.push_bits(replay_target_message, 8U);
    packet.push_unsigned_bits(player_id, 64U);
    send_packet(socket, target, packet);
}

bool read_replay_target(ecs::BitBuffer packet, std::uint64_t& player_id) {
    if (packet.remaining_bits() < 8U + 64U ||
        static_cast<std::uint8_t>(packet.read_bits(8U)) != replay_target_message) {
        return false;
    }
    player_id = packet.read_unsigned_bits(64U);
    return true;
}

}  // namespace

struct FpsReplayServer::Session {
    SocketHandle socket = invalid_socket_handle;
    sockaddr_in address{};
    std::uint64_t killer_player_id = 0;
    ecs::Registry registry;
    std::unique_ptr<kage::sync::ReplicationServer> server;
    kage::sync::ReplicationReplayStreamSession replay_session;
    bool ready = false;
    bool done = false;
    double lifetime_seconds = 0.0;
    double done_notify_seconds = 0.0;

    Session(
        const FpsReplayRecorder& recorder,
        SocketHandle socket,
        kage::sync::ClientId peer,
        const sockaddr_in& peer_address,
        kage::sync::ClientId victim_client,
        kage::sync::SyncFrame death_frame,
        std::uint64_t recorded_killer_player_id,
        const ecs::BitBuffer& connect_packet)
        : socket(socket),
          address(peer_address),
          killer_player_id(recorded_killer_player_id) {
        kage::sync::configure_server(registry);
        (void)define_schema(registry);

        kage::sync::ReplicationServerOptions options;
        options.bandwidth_limit_bytes_per_tick = 64 * 1024;
        options.mtu_bytes = 1200;
        options.fixed_dt_seconds = fixed_dt;
        options.connect_handler = [victim_client](const std::string& token, kage::sync::ClientId& accepted, std::string&) {
            kage::sync::ClientId parsed = kage::sync::invalid_client_id;
            if (!parse_replay_token(token, parsed) || parsed != victim_client) {
                return false;
            }
            accepted = victim_client;
            return true;
        };
        options.transport = [socket, peer_address](kage::sync::ClientId, const ecs::BitBuffer& packet) {
            send_packet(socket, peer_address, packet);
        };
        server = std::make_unique<kage::sync::ReplicationServer>(options);
        if (!recorder.streamer().begin_session(death_frame, registry, *server, replay_session)) {
            throw std::runtime_error("no full FPS replay frame is available before death");
        }
        (void)server->process_packet(registry, peer, connect_packet);
    }

    void receive(kage::sync::ClientId peer, ecs::BitBuffer packet) {
        ecs::BitBuffer copy = packet;
        std::uint8_t message = 0U;
        if (copy.remaining_bits() >= 8U) {
            message = static_cast<std::uint8_t>(copy.read_bits(8U));
        }
        const bool ack = message == kage::sync::protocol::client_connect_ack_message;
        const bool processed = server->process_packet(registry, peer, std::move(packet));
        if (processed && ack) {
            ready = true;
            send_replay_target(socket, address, killer_player_id);
        }
    }

    void tick(const FpsReplayRecorder& recorder) {
        lifetime_seconds += fixed_dt;
        if (done) {
            done_notify_seconds += fixed_dt;
            return;
        }
        if (lifetime_seconds >= replay_session_timeout_seconds) {
            done = true;
            return;
        }
        if (!ready) {
            return;
        }
        if (!recorder.streamer().tick_session(replay_session, registry, *server)) {
            done = true;
        }
    }
};

FpsReplayRecorder::FpsReplayRecorder(const std::string& directory)
    : streamer_(kage::sync::ReplicationReplayStreamerOptions{
          static_cast<std::size_t>(replay_total_frames + 120U),
          replay_preroll_frames,
          replay_tail_frames}),
      writer_(kage::sync::ReplicationReplayWriterOptions{
          60,
          [this](kage::sync::ReplicationReplayFrame frame) {
              write_frame(frame);
              streamer_.push_frame(std::move(frame));
          },
          replay_sample_stride_frames}) {
    std::filesystem::create_directories(directory);
    frame_path_ = (std::filesystem::path(directory) / "frames.bin").string();
    frames_.open(frame_path_, std::ios::binary | std::ios::trunc);
    if (!frames_) {
        throw std::runtime_error("failed to open FPS replay frame file");
    }
}

void FpsReplayRecorder::attach(kage::sync::ReplicationServer& server) {
    writer_.attach(server);
}

void FpsReplayRecorder::detach() {
    writer_.detach();
}

void FpsReplayRecorder::write_frame(const kage::sync::ReplicationReplayFrame& frame) {
    const std::uint64_t frame_offset = static_cast<std::uint64_t>(frames_.tellp());
    write_u32(frames_, replay_frame_magic);
    write_u32(frames_, replay_frame_version);
    write_u32(frames_, static_cast<std::uint32_t>(frame.kind));
    write_u32(frames_, frame.frame);
    write_u64(frames_, static_cast<std::uint64_t>(frame.payload.byte_size()));
    write_u64(frames_, static_cast<std::uint64_t>(frame.payload.bit_size()));
    if (frame.payload.byte_size() != 0U) {
        frames_.write(
            reinterpret_cast<const char*>(frame.payload.data()),
            static_cast<std::streamsize>(frame.payload.byte_size()));
    }
    frames_.flush();
    if (!frames_) {
        throw std::runtime_error("failed to write FPS replay frame");
    }
    entries_.push_back(FrameEntry{
        frame_offset,
        static_cast<std::uint64_t>(frame.payload.byte_size()),
        static_cast<std::uint64_t>(frame.payload.bit_size()),
        frame.frame,
        static_cast<std::uint32_t>(frame.kind)});
}

FpsReplayServer::FpsReplayServer(const FpsReplayRecorder& recorder, std::uint16_t port)
    : recorder_(&recorder),
      socket_(make_udp_socket(port)) {
    std::cout << "kage_sync_fps_example replay listening on UDP " << port << '\n';
}

FpsReplayServer::~FpsReplayServer() {
    detach();
    close_socket(socket_);
}

void FpsReplayServer::record_death(
    kage::sync::ClientId client,
    kage::sync::SyncFrame frame,
    std::uint64_t killer_player_id) {
    if (client == kage::sync::invalid_client_id) {
        return;
    }
    deaths_[client] = DeathInfo{frame, killer_player_id};
}

void FpsReplayServer::attach(kage::sync::ReplicationServer& server) {
    detach();
    death_subscription_ = server.subscribe_registry_dirty_frame_listener(*this);
}

void FpsReplayServer::detach() {
    death_subscription_.reset();
}

void FpsReplayServer::on_server_registry_dirty_frame(const kage::sync::ServerRegistryDirtyFrame& frame) {
    const kage::sync::SyncSettings& settings = frame.registry.get<kage::sync::SyncSettings>();
    const auto found_death_type = settings.cue_type_ids.find(std::type_index(typeid(PlayerDeathCue)));
    if (found_death_type == settings.cue_type_ids.end()) {
        return;
    }
    for (const kage::sync::QueuedSyncCue& cue : frame.cues) {
        if (cue.type != found_death_type->second || cue.value == nullptr) {
            continue;
        }
        const kage::sync::NetworkOwner* victim_owner = frame.registry.try_get<kage::sync::NetworkOwner>(cue.entity);
        if (victim_owner == nullptr) {
            continue;
        }
        const auto death = std::static_pointer_cast<PlayerDeathCue>(cue.value);
        if (death == nullptr) {
            continue;
        }
        const FpsUniquePlayerId* killer_id = frame.registry.try_get<FpsUniquePlayerId>(death->shooter.entity);
        record_death(
            victim_owner->client,
            frame.frame,
            killer_id != nullptr ? killer_id->value : death->shooter.entity.value);
    }
}

bool FpsReplayServer::begin_session(
    kage::sync::ClientId peer,
    const sockaddr_in& address,
    const ecs::BitBuffer& packet) {
    ecs::BitBuffer copy = packet;
    if (copy.remaining_bits() < 8U ||
        static_cast<std::uint8_t>(copy.read_bits(8U)) != kage::sync::protocol::client_connect_request_message) {
        return false;
    }
    std::string token;
    if (!kage::sync::protocol::read_string(copy, token)) {
        return false;
    }
    kage::sync::ClientId client = kage::sync::invalid_client_id;
    if (!parse_replay_token(token, client)) {
        return false;
    }
    const auto found = deaths_.find(client);
    if (found == deaths_.end()) {
        return false;
    }
    try {
        sessions_.push_back(std::make_unique<Session>(
            *recorder_,
            socket_,
            peer,
            address,
            client,
            found->second.frame,
            found->second.killer_player_id,
            packet));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void FpsReplayServer::tick(double dt_seconds) {
    ecs::BitBuffer packet;
    sockaddr_in sender{};
    while (receive_packet(socket_, packet, &sender)) {
        const kage::sync::ClientId peer = peer_id(sender);
        auto found = std::find_if(sessions_.begin(), sessions_.end(), [peer](const std::unique_ptr<Session>& session) {
            return peer_id(session->address) == peer;
        });
        if (found != sessions_.end()) {
            (*found)->receive(peer, std::move(packet));
        } else {
            (void)begin_session(peer, sender, packet);
        }
    }

    accumulator_seconds_ += dt_seconds;
    while (accumulator_seconds_ >= fixed_dt) {
        accumulator_seconds_ -= fixed_dt;
        for (auto& session : sessions_) {
            session->tick(*recorder_);
            if (session->done) {
                send_replay_done(socket_, session->address);
            }
        }
        sessions_.erase(
            std::remove_if(sessions_.begin(), sessions_.end(), [](const std::unique_ptr<Session>& session) {
                return session->done && session->done_notify_seconds >= replay_done_resend_seconds;
            }),
            sessions_.end());
    }
}

FpsDeathCamClient::~FpsDeathCamClient() {
    stop();
}

void FpsDeathCamClient::start(const std::string& host, std::uint16_t replay_port, kage::sync::ClientId client_id) {
    stop();
    socket_ = make_udp_socket(0);
    server_address_ = make_address(host, replay_port);
    registry_ = std::make_unique<ecs::Registry>();
    kage::sync::configure_client(*registry_, client_id);
    (void)define_schema(*registry_);

    kage::sync::ReplicationClientOptions options;
    options.connect_token = replay_token(client_id);
    options.default_entity_mode = kage::sync::ReplicationClientMode::BufferedInterpolation;
    options.fixed_dt_seconds = fixed_dt;
    options.interpolation_buffer_frames = replay_interpolation_buffer_frames;
    options.interpolation_buffer_capacity_frames = 64;
    options.auto_interpolation_buffer_frames = false;
    client_ = std::make_unique<kage::sync::ReplicationClient>(options);
    client_->set_packet_sender([this](const ecs::BitBuffer& packet) {
        send_packet(socket_, server_address_, packet);
    });
    active_seconds_ = 0.0f;
    replay_done_received_ = false;
    target_player_id_ = 0;
    replay_done_drain_seconds_ = 0.0f;
    active_ = true;
}

void FpsDeathCamClient::tick(float dt_seconds) {
    if (!active_) {
        return;
    }
    active_seconds_ += dt_seconds;
    ecs::BitBuffer packet;
    while (receive_packet(socket_, packet)) {
        if (is_replay_done_packet(packet)) {
            replay_done_received_ = true;
            continue;
        }
        std::uint64_t target_player_id = 0;
        if (read_replay_target(packet, target_player_id)) {
            target_player_id_ = target_player_id;
            continue;
        }
        client_->receive_packet(packet);
    }
    (void)client_->tick(*registry_, dt_seconds);
    if (replay_done_received_) {
        replay_done_drain_seconds_ += dt_seconds;
        if (replay_done_drain_seconds_ >= replay_done_client_drain_seconds) {
            stop();
            return;
        }
    }
    if (active_seconds_ >= replay_client_timeout_seconds) {
        stop();
    }
}

void FpsDeathCamClient::stop() {
    if (socket_ != invalid_socket_handle) {
        close_socket(socket_);
        socket_ = invalid_socket_handle;
    }
    client_.reset();
    registry_.reset();
    active_ = false;
    active_seconds_ = 0.0f;
    replay_done_received_ = false;
    target_player_id_ = 0;
    replay_done_drain_seconds_ = 0.0f;
}

bool is_replay_done_packet(ecs::BitBuffer packet) {
    return packet.remaining_bits() >= 8U &&
        static_cast<std::uint8_t>(packet.read_bits(8U)) == replay_done_message;
}

}  // namespace fps
