#pragma once

#include "kage/sync/sync.hpp"
#include "net.hpp"

#include <cstdint>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

namespace fps {

class FpsReplayRecorder {
public:
    struct FrameEntry {
        std::uint64_t offset = 0;
        std::uint64_t payload_size = 0;
        std::uint64_t payload_bits = 0;
        kage::sync::SyncFrame frame = 0;
        std::uint32_t kind = 0;
    };

    explicit FpsReplayRecorder(const std::string& directory);

    FpsReplayRecorder(const FpsReplayRecorder&) = delete;
    FpsReplayRecorder& operator=(const FpsReplayRecorder&) = delete;

    void attach(kage::sync::ReplicationServer& server);
    void detach();
    const std::string& frame_path() const noexcept { return frame_path_; }
    const std::vector<FrameEntry>& entries() const noexcept { return entries_; }
    const kage::sync::ReplicationReplayStreamer& streamer() const noexcept { return streamer_; }

private:
    std::string frame_path_;
    std::ofstream frames_;
    std::vector<FrameEntry> entries_;
    kage::sync::ReplicationReplayStreamer streamer_;
    kage::sync::ReplicationReplayWriter writer_;

    void write_frame(const kage::sync::ReplicationReplayFrame& frame);
};

class FpsReplayServer : private kage::sync::ServerRegistryDirtyFrameListener {
public:
    FpsReplayServer(const FpsReplayRecorder& recorder, std::uint16_t port);
    ~FpsReplayServer();

    FpsReplayServer(const FpsReplayServer&) = delete;
    FpsReplayServer& operator=(const FpsReplayServer&) = delete;

    void record_death(kage::sync::ClientId client, kage::sync::SyncFrame frame, std::uint64_t killer_player_id);
    void attach(kage::sync::ReplicationServer& server);
    void detach();
    void tick(double dt_seconds);

private:
    struct DeathInfo {
        kage::sync::SyncFrame frame = 0;
        std::uint64_t killer_player_id = 0;
    };

    struct Session;

    const FpsReplayRecorder* recorder_ = nullptr;
    kage::sync::ReplicationServer* live_server_ = nullptr;
    SocketHandle socket_ = invalid_socket_handle;
    kage::sync::ServerRegistryDirtyFrameSubscription death_subscription_;
    double accumulator_seconds_ = 0.0;
    std::unordered_map<kage::sync::ClientId, DeathInfo> deaths_;
    std::vector<std::unique_ptr<Session>> sessions_;

    bool begin_session(kage::sync::ClientId peer, const sockaddr_in& address, const ecs::BitBuffer& packet);
    void on_server_registry_dirty_frame(const kage::sync::ServerRegistryDirtyFrame& frame) override;
};

class FpsDeathCamClient {
public:
    FpsDeathCamClient() = default;
    ~FpsDeathCamClient();

    FpsDeathCamClient(const FpsDeathCamClient&) = delete;
    FpsDeathCamClient& operator=(const FpsDeathCamClient&) = delete;

    bool active() const noexcept { return active_; }
    void start(const std::string& host, std::uint16_t replay_port, kage::sync::ClientId client_id);
    void tick(float dt_seconds);
    void stop();
    std::uint64_t target_player_id() const noexcept { return target_player_id_; }

    ecs::Registry& registry() { return *registry_; }
    kage::sync::ReplicationClient& client() { return *client_; }

private:
    bool active_ = false;
    float active_seconds_ = 0.0f;
    bool replay_done_received_ = false;
    std::uint64_t target_player_id_ = 0;
    float replay_done_drain_seconds_ = 0.0f;
    SocketHandle socket_ = invalid_socket_handle;
    sockaddr_in server_address_{};
    std::unique_ptr<ecs::Registry> registry_;
    std::unique_ptr<kage::sync::ReplicationClient> client_;
};

bool is_replay_done_packet(ecs::BitBuffer packet);

}  // namespace fps
