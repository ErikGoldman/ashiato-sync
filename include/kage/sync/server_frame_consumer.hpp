#pragma once

#include "kage/sync/types.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace kage::sync {

class ReplicationServer;

struct ServerFrameDelta {
    ReplicationServer& server;
    ecs::Registry& registry;
    SyncFrame frame = 0;
    ecs::Registry::DirtyView dirty;
    QueuedSyncCueView cues;
};

struct ServerFrameFlushContext {
    ReplicationServer& server;
    ecs::Registry& registry;
    SyncFrame frame = 0;
    double dt_seconds = 0.0;
    std::uint32_t completed_frames = 0;
};

class ServerFrameConsumer {
public:
    virtual ~ServerFrameConsumer() = default;
    virtual void accumulate_frame_delta(const ServerFrameDelta& frame) = 0;
    virtual void flush(const ServerFrameFlushContext& context) {
        (void)context;
    }
};

class ServerFrameConsumerSubscription {
public:
    ServerFrameConsumerSubscription() = default;
    ServerFrameConsumerSubscription(const ServerFrameConsumerSubscription&) = delete;
    ServerFrameConsumerSubscription& operator=(const ServerFrameConsumerSubscription&) = delete;
    ServerFrameConsumerSubscription(ServerFrameConsumerSubscription&& other) noexcept;
    ServerFrameConsumerSubscription& operator=(ServerFrameConsumerSubscription&& other) noexcept;
    ~ServerFrameConsumerSubscription();

    void reset();
    bool active() const noexcept;

private:
    friend class ReplicationServer;

    struct Entry {
        std::uint64_t id = 0;
        ServerFrameConsumer* consumer = nullptr;
    };

    struct State {
        std::uint64_t next_id = 1;
        std::vector<Entry> consumers;
    };

    ServerFrameConsumerSubscription(std::shared_ptr<State> state, std::uint64_t id);

    std::weak_ptr<State> state_;
    std::uint64_t id_ = 0;
};

}  // namespace kage::sync
