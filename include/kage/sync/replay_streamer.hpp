#pragma once

#include "kage/sync/replay_writer.hpp"
#include "kage/sync/server.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace kage::sync {

struct ReplicationReplayStreamSessionAccess;

struct ReplicationReplayStreamerOptions {
    std::size_t max_frames = 600;
    SyncFrame preroll_frames = 180;
    SyncFrame tail_frames = 120;
};

struct ReplicationReplayStreamSession {
    std::size_t next_frame_index = 0;
    SyncFrame playback_frame = 0;
    SyncFrame end_frame = 0;
    bool active = false;

private:
    friend struct ReplicationReplayStreamSessionAccess;

    std::unordered_map<std::uint32_t, ecs::Entity> entities_;
    std::vector<ReplicationReplayFrame> frames_;
    std::vector<std::uint8_t> scratch_;
};

class ReplicationReplayStreamer final {
public:
    explicit ReplicationReplayStreamer(ReplicationReplayStreamerOptions options = {});

    const ReplicationReplayStreamerOptions& options() const noexcept { return options_; }
    std::size_t frame_count() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0U; }
    void clear();

    void push_frame(const ReplicationReplayFrame& frame);
    void push_frame(ReplicationReplayFrame&& frame);

    bool begin_session(
        SyncFrame focus_frame,
        ecs::Registry& registry,
        ReplicationServer& server,
        ReplicationReplayStreamSession& session) const;
    bool tick_session(
        ReplicationReplayStreamSession& session,
        ecs::Registry& registry,
        ReplicationServer& server) const;

    bool apply_frame(
        const ReplicationReplayFrame& frame,
        ecs::Registry& registry,
        ReplicationReplayStreamSession& session) const;

private:
    ReplicationReplayStreamerOptions options_;
    std::vector<ReplicationReplayFrame> frames_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;

    const ReplicationReplayFrame& frame_at(std::size_t index) const;
    void append_available_frames(ReplicationReplayStreamSession& session) const;
    std::size_t find_start_frame(SyncFrame focus_frame) const;
    bool apply_frame(
        const ReplicationReplayFrame& frame,
        ecs::Registry& registry,
        ReplicationReplayStreamSession& session,
        bool include_cues) const;
    bool apply_cues(
        ecs::Registry& registry,
        const SyncSettings& settings,
        ecs::BitBuffer& payload,
        EntityReferenceContext& references,
        ReplicationReplayStreamSession& session) const;
};

}  // namespace kage::sync
