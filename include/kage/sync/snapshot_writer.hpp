#pragma once

#include "kage/sync/server_frame_consumer.hpp"

#include <functional>
#include <memory>
#include <string>

namespace kage::sync {

class ReplicationServer;

enum class SnapshotWriterFrameKind : std::uint32_t {
    Full = 1,
    Delta = 2
};

struct SnapshotWriterFrame {
    const ecs::Registry* registry = nullptr;
    SyncFrame frame = 0;
    SnapshotWriterFrameKind kind = SnapshotWriterFrameKind::Full;
    std::string payload;
    QueuedSyncCueView cues;
};

struct SnapshotWriterOptions {
    ecs::SnapshotComponentOptions component_options;
    SyncFrame full_snapshot_interval_frames = 60;
    std::function<void(const SnapshotWriterFrame&)> write;
};

class SnapshotWriter : public ServerFrameConsumer {
public:
    explicit SnapshotWriter(SnapshotWriterOptions options = {});
    ~SnapshotWriter();

    SnapshotWriter(const SnapshotWriter&) = delete;
    SnapshotWriter& operator=(const SnapshotWriter&) = delete;
    SnapshotWriter(SnapshotWriter&&) noexcept;
    SnapshotWriter& operator=(SnapshotWriter&&) noexcept;

    void attach(ReplicationServer& server);
    void detach();
    bool attached() const noexcept;

    void accumulate_frame_delta(const ServerFrameDelta& frame) override;

private:
    void record(const ServerFrameDelta& frame);

    SnapshotWriterOptions options_;
    ServerFrameConsumerSubscription subscription_;
    std::unique_ptr<ecs::Registry::Snapshot> last_full_;
    std::unique_ptr<ecs::Registry::DeltaSnapshot> last_delta_;
};

}  // namespace kage::sync
