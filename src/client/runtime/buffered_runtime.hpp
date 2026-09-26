#pragma once

#include "client/state.hpp"
#include "client/store/frame_ring_store.hpp"

#include "ashiato/ashiato.hpp"
#include "ashiato/sync/client_clock.hpp"

#include <cstddef>
#include <cstdint>

namespace ashiato::sync {

class ReplicationClient;

namespace client_detail {

enum class MissingBufferedFramePolicy : std::uint8_t {
    FailApply,
    AllowMissing,
};

class ClientBufferedRuntime {
public:
    explicit ClientBufferedRuntime(std::size_t frame_capacity = 64);

    ClientFrameRingStore& frames() noexcept {
        return buffered_frames_;
    }

    const ClientFrameRingStore& frames() const noexcept {
        return buffered_frames_;
    }

    bool has_applied_frame() const noexcept {
        return has_applied_buffered_frame_;
    }

    SyncFrame last_applied_frame() const noexcept {
        return last_applied_buffered_frame_;
    }

    // A record was written into the ring for `frame`. Before the first frame is applied, the earliest such frame is
    // where playback starts if the clock has been re-anchored past it (`frames_owed`).
    void note_written(SyncFrame frame) noexcept {
        if (!has_applied_buffered_frame_ && (!has_unapplied_write_ || frame < earliest_unapplied_write_)) {
            earliest_unapplied_write_ = frame;
            has_unapplied_write_ = true;
        }
    }

    void reset_entity(std::uint32_t entity_index) noexcept;
    void ensure_entity(std::uint32_t entity_index);
    void clear_entity(std::uint32_t entity_index) noexcept;

    // The frames playback owes when the clock stands at `buffered_frame`: `advanced` as the clock gave it, unless the
    // clock was re-anchored forward past frames never applied (a time-sync re-estimate), in which case playback resumes
    // from the frame after the last one applied -- or, before any frame has been applied, from the earliest frame a
    // record was written for -- as far back as the ring still holds.
    ReplicationClientClock::FrameRange frames_owed(
        const ReplicationClientClock::FrameRange& advanced,
        SyncFrame buffered_frame) const noexcept;
    bool apply_frames(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const ReplicationClientClock::FrameRange& frames);
    bool apply_frame(
        ReplicationClient& client,
        ashiato::Registry& registry,
        SyncFrame buffered_frame,
        MissingBufferedFramePolicy missing_frame_policy = MissingBufferedFramePolicy::FailApply);

private:
    ClientFrameRingStore buffered_frames_;
    SyncFrame last_applied_buffered_frame_ = 0;
    bool has_applied_buffered_frame_ = false;
    SyncFrame earliest_unapplied_write_ = 0;
    bool has_unapplied_write_ = false;
};

}  // namespace client_detail
}  // namespace ashiato::sync
