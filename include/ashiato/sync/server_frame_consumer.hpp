#pragma once

#include "ashiato/sync/types.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ashiato::sync {

class ReplicationServer;

struct ServerDestroyedReplicatedSlot {
    std::uint32_t slot = 0;
    ashiato::Entity entity;
};

struct ServerDestroyedReplicatedSlotView {
    const ServerDestroyedReplicatedSlot* data = nullptr;
    std::size_t size = 0;

    const ServerDestroyedReplicatedSlot* begin() const noexcept { return data; }
    const ServerDestroyedReplicatedSlot* end() const noexcept { return data == nullptr ? nullptr : data + size; }
    bool empty() const noexcept { return size == 0U; }
};

struct ServerRegistryDirtyFrame {
    ReplicationServer& server;
    ashiato::Registry& registry;
    ashiato::Registry::DirtyView dirty;
    SyncFrame frame = 0;
    QueuedSyncCueView cues;
    ServerDestroyedReplicatedSlotView destroyed_slots;
};

class ServerRegistryDirtyFrameListener {
public:
    virtual ~ServerRegistryDirtyFrameListener() = default;
    virtual void on_server_registry_dirty_frame(const ServerRegistryDirtyFrame& frame) = 0;
};

class ServerRegistryDirtyFrameSubscription {
public:
    ServerRegistryDirtyFrameSubscription() = default;
    ServerRegistryDirtyFrameSubscription(const ServerRegistryDirtyFrameSubscription&) = delete;
    ServerRegistryDirtyFrameSubscription& operator=(const ServerRegistryDirtyFrameSubscription&) = delete;
    ServerRegistryDirtyFrameSubscription(ServerRegistryDirtyFrameSubscription&& other) noexcept;
    ServerRegistryDirtyFrameSubscription& operator=(ServerRegistryDirtyFrameSubscription&& other) noexcept;
    ~ServerRegistryDirtyFrameSubscription();

    void reset();
    bool active() const noexcept;

private:
    friend class ReplicationServer;

    struct Entry {
        std::uint64_t id = 0;
        ServerRegistryDirtyFrameListener* listener = nullptr;
    };

    struct State {
        std::uint64_t next_id = 1;
        std::vector<Entry> listeners;
    };

    ServerRegistryDirtyFrameSubscription(const std::shared_ptr<State>& state, std::uint64_t id);

    std::weak_ptr<State> state_;
    std::uint64_t id_ = 0;
};

struct ServerFrameBatch {
    ReplicationServer& server;
    ashiato::Registry& registry;
    double dt_seconds = 0.0;
    std::uint32_t completed_frames = 0;
};

class ServerFrameBatchListener {
public:
    virtual ~ServerFrameBatchListener() = default;
    virtual void on_server_frame_batch_complete(const ServerFrameBatch& batch) = 0;
};

class ServerFrameBatchListenerSubscription {
public:
    ServerFrameBatchListenerSubscription() = default;
    ServerFrameBatchListenerSubscription(const ServerFrameBatchListenerSubscription&) = delete;
    ServerFrameBatchListenerSubscription& operator=(const ServerFrameBatchListenerSubscription&) = delete;
    ServerFrameBatchListenerSubscription(ServerFrameBatchListenerSubscription&& other) noexcept;
    ServerFrameBatchListenerSubscription& operator=(ServerFrameBatchListenerSubscription&& other) noexcept;
    ~ServerFrameBatchListenerSubscription();

    void reset();
    bool active() const noexcept;

private:
    friend class ReplicationServer;

    struct Entry {
        std::uint64_t id = 0;
        ServerFrameBatchListener* listener = nullptr;
    };

    struct State {
        std::uint64_t next_id = 1;
        std::vector<Entry> listeners;
    };

    ServerFrameBatchListenerSubscription(const std::shared_ptr<State>& state, std::uint64_t id);

    std::weak_ptr<State> state_;
    std::uint64_t id_ = 0;
};

}  // namespace ashiato::sync
