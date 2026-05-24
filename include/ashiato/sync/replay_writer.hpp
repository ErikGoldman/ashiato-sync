#pragma once

#include "ashiato/sync/server_frame_consumer.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace ashiato::sync {

class ReplicationServer;

enum class ReplicationReplayFrameKind : std::uint32_t {
    Full = 1,
    Delta = 2
};

struct ReplicationReplayFrame {
    SyncFrame frame = 0;
    ReplicationReplayFrameKind kind = ReplicationReplayFrameKind::Full;
    ashiato::BitBuffer payload;
};

struct ReplicationReplayWriterOptions {
    SyncFrame full_frame_interval_frames = 60;
    std::function<void(ReplicationReplayFrame)> write;
    SyncFrame write_interval_frames = 1;
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    SyncTracer* serialization_tracer = nullptr;
#endif
};

class ReplicationReplayWriter final : public ServerRegistryDirtyFrameListener {
public:
    explicit ReplicationReplayWriter(ReplicationReplayWriterOptions options = {});
    ~ReplicationReplayWriter() override;

    ReplicationReplayWriter(const ReplicationReplayWriter&) = delete;
    ReplicationReplayWriter& operator=(const ReplicationReplayWriter&) = delete;
    ReplicationReplayWriter(ReplicationReplayWriter&&) noexcept;
    ReplicationReplayWriter& operator=(ReplicationReplayWriter&&) noexcept;

    void attach(ReplicationServer& server);
    void detach();
    bool attached() const noexcept;

    void on_server_registry_dirty_frame(const ServerRegistryDirtyFrame& frame) override;

private:
    struct State;

    ReplicationReplayWriterOptions options_;
    std::unique_ptr<State> state_;
    ReplicationServer* server_ = nullptr;
    ServerRegistryDirtyFrameSubscription subscription_;
};

}  // namespace ashiato::sync
