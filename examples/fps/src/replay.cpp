#include "replay.hpp"

#include "game/components.hpp"
#include "game/cues.hpp"
#include "game/schema.hpp"
#include "game/sync_traits.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <typeindex>
#include <utility>

namespace fps {
namespace {

template <typename T>
void push_registered_component(const ecs::Registry& registry, std::vector<ecs::Entity>& components) {
    const ecs::Entity component = registry.component<T>();
    if (std::find(components.begin(), components.end(), component) == components.end()) {
        components.push_back(component);
    }
}

void push_replay_component(const ecs::Registry& registry, std::vector<ecs::Entity>& components, ecs::Entity component, const char* source) {
    if (!component || registry.component_info(component) == nullptr) {
        throw std::logic_error(std::string("FPS replay snapshot references unregistered component from ") + source);
    }
    if (std::find(components.begin(), components.end(), component) == components.end()) {
        components.push_back(component);
    }
}

ecs::SnapshotComponentOptions build_replay_snapshot_options(const ecs::Registry& registry) {
    ecs::SnapshotComponentOptions options;
    std::vector<ecs::Entity> components;
    components.reserve(16);

    push_registered_component<kage::sync::Replicated>(registry, components);
    push_registered_component<kage::sync::NetworkOwner>(registry, components);
    push_registered_component<kage::sync::NoSimulate>(registry, components);

    const kage::sync::SyncSettings& settings = registry.get<kage::sync::SyncSettings>();
    if (settings.input_component) {
        push_replay_component(registry, components, settings.input_component, "sync input component");
    }
    for (const kage::sync::SyncArchetype& archetype : settings.archetypes) {
        for (const kage::sync::SyncTagReplication& tag : archetype.tags) {
            push_replay_component(registry, components, tag.tag, "sync archetype tag");
        }
        for (const kage::sync::ComponentReplication& component : archetype.components) {
            push_replay_component(registry, components, component.component, "sync archetype component");
        }
    }

    options.include_components = std::move(components);
    return options;
}

void write_u32(std::ostream& out, std::uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_u64(std::ostream& out, std::uint64_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_f32(std::ostream& out, float value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_u8(std::ostream& out, std::uint8_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

std::uint32_t read_u32(std::istream& in) {
    std::uint32_t value = 0;
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

std::uint64_t read_u64(std::istream& in) {
    std::uint64_t value = 0;
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

float read_f32(std::istream& in) {
    float value = 0.0f;
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

std::uint8_t read_u8(std::istream& in) {
    std::uint8_t value = 0;
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

constexpr std::uint32_t replay_frame_magic = 0x52535046U;
constexpr std::uint32_t replay_frame_version = 2U;
constexpr std::uint8_t replay_done_message = 200U;
constexpr std::uint8_t replay_target_message = 201U;
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

struct ReplayCueRecord {
    ecs::Entity owner;
    kage::sync::SyncCueTypeId type = 0;
    float relevance_seconds = 0.0f;
    bool only_replicate_to_owner = false;
    std::string payload;
};

struct ReplayFrameData {
    std::string snapshot_payload;
    std::vector<ReplayCueRecord> cues;
};

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

void write_string_payload(std::ostream& out, const std::string& payload) {
    write_u32(out, static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty()) {
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }
}

std::string read_string_payload(std::istream& in) {
    const std::uint32_t size = read_u32(in);
    std::string payload(size, '\0');
    if (!payload.empty()) {
        in.read(&payload[0], static_cast<std::streamsize>(payload.size()));
    }
    if (!in) {
        throw std::runtime_error("failed to read FPS replay cue payload");
    }
    return payload;
}

std::uint64_t cue_entity_payload(const kage::sync::QueuedSyncCue& cue) {
    if (cue.value == nullptr) {
        return 0U;
    }
    const auto player_hit = std::static_pointer_cast<PlayerHitCue>(cue.value);
    return player_hit != nullptr ? player_hit->shooter.entity.value : 0U;
}

std::uint64_t hit_confirm_entity_payload(const kage::sync::QueuedSyncCue& cue) {
    if (cue.value == nullptr) {
        return 0U;
    }
    const auto hit_confirm = std::static_pointer_cast<HitConfirmCue>(cue.value);
    return hit_confirm != nullptr ? hit_confirm->victim.entity.value : 0U;
}

std::uint64_t death_entity_payload(const kage::sync::QueuedSyncCue& cue) {
    if (cue.value == nullptr) {
        return 0U;
    }
    const auto player_death = std::static_pointer_cast<PlayerDeathCue>(cue.value);
    return player_death != nullptr ? player_death->shooter.entity.value : 0U;
}

bool make_replay_cue_payload(
    const ecs::Registry& registry,
    const kage::sync::QueuedSyncCue& cue,
    std::string& payload) {
    const kage::sync::SyncSettings& settings = registry.get<kage::sync::SyncSettings>();
    auto cue_type = [&](const std::type_info& type) {
        const auto found = settings.cue_type_ids.find(std::type_index(type));
        return found != settings.cue_type_ids.end() ? found->second : kage::sync::SyncCueTypeId{};
    };
    if (cue.type == cue_type(typeid(ShotCue)) || cue.type == cue_type(typeid(SurfaceHitCue))) {
        if (cue.payload.byte_size() != 0U) {
            payload.assign(
                reinterpret_cast<const char*>(cue.payload.data()),
                reinterpret_cast<const char*>(cue.payload.data()) + cue.payload.byte_size());
        } else {
            payload.clear();
        }
        return true;
    }
    std::ostringstream out;
    if (cue.type == cue_type(typeid(PlayerHitCue))) {
        write_u64(out, cue_entity_payload(cue));
        payload = out.str();
        return true;
    }
    if (cue.type == cue_type(typeid(HitConfirmCue))) {
        write_u64(out, hit_confirm_entity_payload(cue));
        payload = out.str();
        return true;
    }
    if (cue.type == cue_type(typeid(PlayerDeathCue))) {
        write_u64(out, death_entity_payload(cue));
        payload = out.str();
        return true;
    }
    return false;
}

std::vector<ReplayCueRecord> capture_replay_cues(
    const ecs::Registry& registry,
    kage::sync::QueuedSyncCueView cues) {
    std::vector<ReplayCueRecord> records;
    records.reserve(cues.size);
    for (const kage::sync::QueuedSyncCue& cue : cues) {
        if (!cue.entity || cue.type >= registry.get<kage::sync::SyncSettings>().cue_ops.size()) {
            continue;
        }
        std::string payload;
        if (!make_replay_cue_payload(registry, cue, payload)) {
            continue;
        }
        records.push_back(ReplayCueRecord{
            cue.entity,
            cue.type,
            cue.relevance_seconds,
            cue.only_replicate_to_owner,
            std::move(payload)});
    }
    return records;
}

void write_replay_cues(std::ostream& out, const std::vector<ReplayCueRecord>& cues) {
    for (const ReplayCueRecord& cue : cues) {
        write_u64(out, cue.owner.value);
        write_u32(out, cue.type);
        write_f32(out, cue.relevance_seconds);
        write_u8(out, cue.only_replicate_to_owner ? 1U : 0U);
        write_string_payload(out, cue.payload);
    }
}

ReplayFrameData read_frame_data(const std::string& frame_path, const FpsReplayRecorder::FrameEntry& entry) {
    std::ifstream input(frame_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open FPS replay frame file for reading");
    }
    input.seekg(static_cast<std::streamoff>(entry.offset));
    const std::uint32_t magic = read_u32(input);
    const std::uint32_t version = read_u32(input);
    const std::uint32_t kind = read_u32(input);
    const std::uint32_t frame = read_u32(input);
    const std::uint64_t payload_size = read_u64(input);
    const std::uint32_t cue_count = read_u32(input);
    if (!input || magic != replay_frame_magic || version != replay_frame_version ||
        kind != entry.kind || frame != entry.frame || payload_size != entry.payload_size ||
        cue_count != entry.cue_count) {
        throw std::runtime_error("invalid FPS replay frame header");
    }
    ReplayFrameData data;
    data.snapshot_payload.resize(static_cast<std::size_t>(entry.payload_size), '\0');
    if (!data.snapshot_payload.empty()) {
        input.read(&data.snapshot_payload[0], static_cast<std::streamsize>(data.snapshot_payload.size()));
    }
    if (!input) {
        throw std::runtime_error("failed to read FPS replay frame payload");
    }
    data.cues.reserve(cue_count);
    for (std::uint32_t index = 0; index < cue_count; ++index) {
        ReplayCueRecord cue;
        cue.owner = ecs::Entity{read_u64(input)};
        cue.type = static_cast<kage::sync::SyncCueTypeId>(read_u32(input));
        cue.relevance_seconds = read_f32(input);
        cue.only_replicate_to_owner = read_u8(input) != 0U;
        cue.payload = read_string_payload(input);
        data.cues.push_back(std::move(cue));
    }
    return data;
}

void mark_restored_replicated_dirty(ecs::Registry& registry) {
    struct RestoredReplicatedEntity {
        ecs::Entity entity;
        kage::sync::SyncArchetypeId archetype;
    };

    std::vector<RestoredReplicatedEntity> replicated_entities;
    registry.view<const kage::sync::Replicated>().each([&replicated_entities](ecs::Entity entity, const kage::sync::Replicated& replicated) {
        replicated_entities.push_back(RestoredReplicatedEntity{entity, replicated.archetype});
    });

    const kage::sync::SyncSettings& settings = registry.get<kage::sync::SyncSettings>();
    for (const RestoredReplicatedEntity& replicated : replicated_entities) {
        const ecs::Entity entity = replicated.entity;
        if (registry.contains<kage::sync::Replicated>(entity)) {
            (void)registry.write<kage::sync::Replicated>(entity);
        }
        if (replicated.archetype.value >= settings.archetypes.size()) {
            continue;
        }
        const kage::sync::SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
        for (const kage::sync::ComponentReplication& component : archetype.components) {
            const ecs::ComponentInfo* info = registry.component_info(component.component);
            if (info == nullptr || info->tag || registry.get(entity, component.component) == nullptr) {
                continue;
            }
            (void)registry.write(entity, component.component);
        }
    }
}

void emit_replay_cues(ecs::Registry& registry, const std::vector<ReplayCueRecord>& cues) {
    const kage::sync::SyncSettings& settings = registry.get<kage::sync::SyncSettings>();
    const auto shot_type = settings.cue_type_ids.at(std::type_index(typeid(ShotCue)));
    const auto surface_type = settings.cue_type_ids.at(std::type_index(typeid(SurfaceHitCue)));
    const auto player_hit_type = settings.cue_type_ids.at(std::type_index(typeid(PlayerHitCue)));
    const auto hit_confirm_type = settings.cue_type_ids.at(std::type_index(typeid(HitConfirmCue)));
    const auto player_death_type = settings.cue_type_ids.at(std::type_index(typeid(PlayerDeathCue)));
    for (const ReplayCueRecord& cue : cues) {
        if (!cue.owner || !registry.alive(cue.owner)) {
            continue;
        }
        if (cue.type == shot_type) {
            (void)kage::sync::emit_cue(
                registry,
                cue.owner,
                ShotCue{},
                cue.relevance_seconds,
                cue.only_replicate_to_owner);
        } else if (cue.type == surface_type) {
            SurfaceHitCue value;
            ecs::BitBuffer payload;
            payload.push_bytes(cue.payload.data(), cue.payload.size());
            if (kage::sync::SyncCueTraits<SurfaceHitCue>::deserialize(payload, value)) {
                (void)kage::sync::emit_cue(
                    registry,
                    cue.owner,
                    value,
                    cue.relevance_seconds,
                    cue.only_replicate_to_owner);
            }
        } else if (cue.type == player_hit_type) {
            std::istringstream in(cue.payload);
            const ecs::Entity shooter{read_u64(in)};
            (void)kage::sync::emit_cue(
                registry,
                cue.owner,
                PlayerHitCue{kage::sync::EntityReference{shooter}},
                cue.relevance_seconds,
                cue.only_replicate_to_owner);
        } else if (cue.type == hit_confirm_type) {
            std::istringstream in(cue.payload);
            const ecs::Entity victim{read_u64(in)};
            (void)kage::sync::emit_cue(
                registry,
                cue.owner,
                HitConfirmCue{kage::sync::EntityReference{victim}},
                cue.relevance_seconds,
                cue.only_replicate_to_owner);
        } else if (cue.type == player_death_type) {
            std::istringstream in(cue.payload);
            const ecs::Entity shooter{read_u64(in)};
            (void)kage::sync::emit_cue(
                registry,
                cue.owner,
                PlayerDeathCue{kage::sync::EntityReference{shooter}},
                cue.relevance_seconds,
                cue.only_replicate_to_owner);
        }
    }
}

}  // namespace

struct FpsReplayServer::Session {
    kage::sync::SyncFrame end_frame = 0;
    SocketHandle socket = invalid_socket_handle;
    sockaddr_in address{};
    std::size_t next_entry = 0;
    std::uint64_t killer_entity_value = 0;
    std::uint64_t killer_player_id = 0;
    ecs::Registry registry;
    std::unique_ptr<kage::sync::ReplicationServer> server;
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
        std::uint64_t recorded_killer_entity_value,
        const ecs::BitBuffer& connect_packet)
        : end_frame(death_frame + replay_tail_frames),
          socket(socket),
          address(peer_address),
          killer_entity_value(recorded_killer_entity_value),
          killer_player_id(recorded_killer_entity_value) {
        const auto& entries = recorder.entries();
        const kage::sync::SyncFrame target = death_frame > replay_preroll_frames
            ? death_frame - replay_preroll_frames
            : 1U;
        std::size_t start = entries.size();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].kind == 1U &&
                entries[index].frame <= target) {
                start = index;
            }
        }
        if (start == entries.size()) {
            throw std::runtime_error("no full FPS replay frame is available before death");
        }

        ReplayFrameData frame_data = read_frame_data(recorder.frame_path(), entries[start]);
        std::stringstream payload(frame_data.snapshot_payload, std::ios::in | std::ios::binary);
        registry.restore_snapshot(ecs::Registry::Snapshot::read(payload));
        kage::sync::configure_server(registry);
        (void)define_schema(registry);
        const ecs::Entity killer_entity{killer_entity_value};
        if (const FpsUniquePlayerId* unique = registry.try_get<FpsUniquePlayerId>(killer_entity)) {
            killer_player_id = unique->value;
        }
        mark_restored_replicated_dirty(registry);

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
        (void)server->process_packet(registry, peer, connect_packet);
        next_entry = start;
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
        if (next_entry >= recorder.entries().size()) {
            done = true;
            return;
        }

        const FpsReplayRecorder::FrameEntry& entry = recorder.entries()[next_entry];
        if (entry.frame > end_frame) {
            done = true;
            return;
        }

        ReplayFrameData frame_data = read_frame_data(recorder.frame_path(), entry);
        std::stringstream payload(frame_data.snapshot_payload, std::ios::in | std::ios::binary);
        if (entry.kind == 1U) {
            registry.restore_snapshot(ecs::Registry::Snapshot::read(payload));
            kage::sync::configure_server(registry);
            (void)define_schema(registry);
        } else {
            registry.restore_delta_snapshot(ecs::Registry::DeltaSnapshot::read(payload));
        }
        mark_restored_replicated_dirty(registry);
        emit_replay_cues(registry, frame_data.cues);
        server->advance_frame_without_simulating(registry);
        ++next_entry;
    }
};

FpsReplayRecorder::FpsReplayRecorder(const ecs::Registry& registry, const std::string& directory) {
    kage::sync::SnapshotWriterOptions options;
    options.component_options = build_replay_snapshot_options(registry);
    options.full_snapshot_interval_frames = 60;
    options.write = [this](const kage::sync::SnapshotWriterFrame& frame) {
        if (frame.registry == nullptr) {
            return;
        }
        write_frame(
            *frame.registry,
            frame.kind == kage::sync::SnapshotWriterFrameKind::Full ? FrameKind::Full : FrameKind::Delta,
            frame.frame,
            frame.payload,
            frame.cues);
    };
    writer_ = kage::sync::SnapshotWriter(std::move(options));

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

void FpsReplayRecorder::write_frame(
    const ecs::Registry& registry,
    FrameKind kind,
    kage::sync::SyncFrame frame,
    const std::string& payload,
    kage::sync::QueuedSyncCueView cues) {
    std::vector<ReplayCueRecord> cue_records = capture_replay_cues(registry, cues);
    const std::uint64_t frame_offset = static_cast<std::uint64_t>(frames_.tellp());
    write_u32(frames_, replay_frame_magic);
    write_u32(frames_, replay_frame_version);
    write_u32(frames_, static_cast<std::uint32_t>(kind));
    write_u32(frames_, frame);
    write_u64(frames_, static_cast<std::uint64_t>(payload.size()));
    write_u32(frames_, static_cast<std::uint32_t>(cue_records.size()));
    if (!payload.empty()) {
        frames_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }
    write_replay_cues(frames_, cue_records);
    frames_.flush();
    if (!frames_) {
        throw std::runtime_error("failed to write FPS replay frame");
    }
    entries_.push_back(FrameEntry{
        frame_offset,
        static_cast<std::uint64_t>(payload.size()),
        static_cast<std::uint32_t>(cue_records.size()),
        frame,
        static_cast<std::uint32_t>(kind)});
}

FpsReplayServer::FpsReplayServer(const FpsReplayRecorder& recorder, std::uint16_t port)
    : recorder_(&recorder),
      socket_(make_udp_socket(port)) {
    std::cout << "kage_sync_fps_example replay listening on UDP " << port << '\n';
}

FpsReplayServer::~FpsReplayServer() {
    close_socket(socket_);
}

void FpsReplayServer::record_death(
    kage::sync::ClientId client,
    kage::sync::SyncFrame frame,
    ecs::Entity killer_entity) {
    if (client == kage::sync::invalid_client_id) {
        return;
    }
    deaths_[client] = DeathInfo{frame, killer_entity.value};
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
            found->second.killer_entity_value,
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
    options.interpolation_buffer_frames = 2;
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
