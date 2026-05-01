#pragma once

#include "kage/sync/types.hpp"

#ifdef KAGE_SYNC_ENABLE_TRACING

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kage::sync {

enum class SyncTraceRole : std::uint8_t {
    Server = 1,
    Client = 2
};

enum class SyncTraceEventType : std::uint8_t {
    ClientConnected = 1,
    ClientDisconnected = 2,
    EntityStartedSyncing = 3,
    EntityReceived = 4,
    ComponentSent = 5,
    ComponentReceived = 6,
    ComponentApplied = 7,
    ComponentRemoved = 8,
    TagSent = 9,
    TagApplied = 10,
    ModeChanged = 11,
    BufferedStarved = 12,
    PredictionRollbackConflict = 13,
    FrameComponent = 14,
    CueInvoked = 15,
    CueRolledBack = 16,
    CueReceived = 17,
    TagReceived = 18,
    EntityDestroyed = 19,
    ResimulatedFrameComponent = 20,
    ComponentName = 21,
    RollbackReason = 22
};

struct SyncTraceEvent {
    SyncTraceEventType type = SyncTraceEventType::ClientConnected;
    SyncTraceRole role = SyncTraceRole::Server;
    ClientId client = invalid_client_id;
    SyncFrame frame = 0;
    ecs::Entity server_entity{};
    ecs::Entity local_entity{};
    ClientEntityNetworkId client_network_id = invalid_client_entity_network_id;
    std::uint32_t wire_network_id = 0;
    std::uint32_t network_version = 0;
    SyncArchetypeId archetype = invalid_sync_archetype_id;
    ecs::Entity component{};
    ecs::Entity tag{};
    SyncCueTypeId cue_type = 0;
    ReplicationClientMode previous_mode = ReplicationClientMode::Snap;
    ReplicationClientMode mode = ReplicationClientMode::Snap;
    bool remove = false;
    std::string data;
    std::string component_name;
};

struct SyncTraceCallbacks {
    std::function<void(const SyncTraceEvent&)> on_event;
    std::function<void(const SyncTraceEvent&)> on_client_connected;
    std::function<void(const SyncTraceEvent&)> on_client_disconnected;
    std::function<void(const SyncTraceEvent&)> on_entity_started_syncing;
    std::function<void(const SyncTraceEvent&)> on_entity_received;
    std::function<void(const SyncTraceEvent&)> on_component_sent;
    std::function<void(const SyncTraceEvent&)> on_component_received;
    std::function<void(const SyncTraceEvent&)> on_component_applied;
    std::function<void(const SyncTraceEvent&)> on_component_removed;
    std::function<void(const SyncTraceEvent&)> on_tag_sent;
    std::function<void(const SyncTraceEvent&)> on_tag_received;
    std::function<void(const SyncTraceEvent&)> on_tag_applied;
    std::function<void(const SyncTraceEvent&)> on_mode_changed;
    std::function<void(const SyncTraceEvent&)> on_buffered_starved;
    std::function<void(const SyncTraceEvent&)> on_prediction_rollback_conflict;
    std::function<void(const SyncTraceEvent&)> on_frame_component;
    std::function<void(const SyncTraceEvent&)> on_cue_invoked;
    std::function<void(const SyncTraceEvent&)> on_cue_rolled_back;
    std::function<void(const SyncTraceEvent&)> on_cue_received;
    std::function<void(const SyncTraceEvent&)> on_entity_destroyed;
    std::function<void(const SyncTraceEvent&)> on_resimulated_frame_component;
    std::function<void(const SyncTraceEvent&)> on_component_name;
    std::function<void(const SyncTraceEvent&)> on_rollback_reason;
};

class SyncTracer {
public:
    explicit SyncTracer(SyncTraceCallbacks callbacks = {});

    bool enabled() const noexcept { return enabled_; }
    bool frame_data_enabled() const noexcept { return frame_data_enabled_; }
    bool cue_data_enabled() const noexcept { return cue_data_enabled_; }

    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    void set_frame_data_enabled(bool enabled) noexcept { frame_data_enabled_ = enabled; }
    void set_cue_data_enabled(bool enabled) noexcept { cue_data_enabled_ = enabled; }
    void set_callbacks(SyncTraceCallbacks callbacks);
    const SyncTraceCallbacks& callbacks() const noexcept { return callbacks_; }

    void trace(const SyncTraceEvent& event) const;

private:
    bool enabled_ = true;
    bool frame_data_enabled_ = false;
    bool cue_data_enabled_ = false;
    SyncTraceCallbacks callbacks_;
};

struct BinarySyncTraceWriterOptions {
    std::string server_path;
    std::string client_path;
    std::size_t flush_threshold_bytes = 64 * 1024;
};

class BinarySyncTraceWriter {
public:
    explicit BinarySyncTraceWriter(BinarySyncTraceWriterOptions options);
    ~BinarySyncTraceWriter();

    BinarySyncTraceWriter(const BinarySyncTraceWriter&) = delete;
    BinarySyncTraceWriter& operator=(const BinarySyncTraceWriter&) = delete;

    SyncTracer& tracer() noexcept { return tracer_; }
    const SyncTracer& tracer() const noexcept { return tracer_; }

    void flush();
    void close();

private:
    struct Sink {
        std::ofstream file;
        std::vector<std::uint8_t> pending;
        std::vector<std::uint8_t> writing;
        std::unordered_set<std::uint64_t> named_components;
        std::mutex mutex;
        std::condition_variable cv;
        std::thread thread;
        bool flush_requested = false;
        bool closing = false;
    };

    static void append_event(std::vector<std::uint8_t>& out, const SyncTraceEvent& event);
    static void write_header(std::ofstream& file, SyncTraceRole role);
    void record(const SyncTraceEvent& event);
    void run_sink(Sink& sink);
    Sink& sink_for(SyncTraceRole role) noexcept;

    BinarySyncTraceWriterOptions options_;
    Sink server_;
    Sink client_;
    SyncTracer tracer_;
};

inline constexpr std::uint16_t ktrace_format_version = 5;
inline constexpr ClientId ktrace_server_source_id = invalid_client_id;

struct KTraceDirectoryWriterOptions {
    std::string directory;
    std::size_t queue_capacity_bytes = 4 * 1024 * 1024;
    std::size_t flush_threshold_bytes = 64 * 1024;
    bool truncate_existing = true;
};

struct KTraceRecord {
    SyncTraceEvent event;
    std::uint64_t timestamp_us = 0;
};

struct KTraceFile {
    std::string path;
    SyncTraceRole role = SyncTraceRole::Server;
    ClientId client = invalid_client_id;
    std::uint64_t recorded_unix_ns = 0;
    std::uint16_t version = 0;
    std::vector<KTraceRecord> records;
    std::unordered_map<std::uint64_t, std::string> component_names;
};

enum class KTraceCellState : std::uint16_t {
    SentToClient = 1U << 0U,
    ReceivedFromServer = 1U << 1U,
    Applied = 1U << 2U,
    LocalInterpolated = 1U << 3U,
    LocalPredicted = 1U << 4U,
    PredictedCorrect = 1U << 5U,
    Mispredicted = 1U << 6U,
    Starved = 1U << 7U,
    Removed = 1U << 8U,
    EntityDestroyed = 1U << 9U,
    Resimulated = 1U << 10U
};

struct KTraceFrameCell {
    SyncFrame frame = 0;
    std::uint16_t state_mask = 0;
    std::vector<std::uint32_t> event_indices;
};

struct KTraceComponentRow {
    ecs::Entity component{};
    std::vector<KTraceFrameCell> cells;
};

struct KTraceEntityBranch {
    SyncFrame from_frame = 0;
    ecs::Entity component{};
    std::vector<KTraceComponentRow> components;
};

struct KTraceEntityRow {
    ClientEntityNetworkId client_network_id = invalid_client_entity_network_id;
    std::uint32_t wire_network_id = 0;
    std::uint32_t network_version = 0;
    ecs::Entity server_entity{};
    ecs::Entity local_entity{};
    SyncArchetypeId archetype = invalid_sync_archetype_id;
    std::vector<KTraceComponentRow> components;
    std::vector<KTraceEntityBranch> rollback_branches;
};

struct KTraceSourceHistory {
    SyncTraceRole role = SyncTraceRole::Server;
    ClientId client = invalid_client_id;
    std::string path;
    std::uint64_t recorded_unix_ns = 0;
    std::vector<KTraceRecord> records;
    std::vector<KTraceEntityRow> entities;
    std::unordered_map<std::uint64_t, std::string> component_names;
};

struct SyncTraceHistory {
    std::vector<KTraceSourceHistory> sources;
};

class KTraceDirectoryWriter {
public:
    explicit KTraceDirectoryWriter(KTraceDirectoryWriterOptions options);
    ~KTraceDirectoryWriter();

    KTraceDirectoryWriter(const KTraceDirectoryWriter&) = delete;
    KTraceDirectoryWriter& operator=(const KTraceDirectoryWriter&) = delete;

    SyncTracer& tracer() noexcept { return tracer_; }
    const SyncTracer& tracer() const noexcept { return tracer_; }

    void flush();
    void close();

private:
    struct Sink {
        std::ofstream file;
        std::string path;
        SyncTraceRole role = SyncTraceRole::Server;
        ClientId client = invalid_client_id;
        std::uint64_t recorded_unix_ns = 0;
        std::uint64_t start_steady_us = 0;
        std::deque<std::vector<std::uint8_t>> queue;
        std::unordered_set<std::uint64_t> named_components;
        std::size_t queued_bytes = 0;
        std::mutex mutex;
        std::condition_variable cv;
        std::condition_variable drained_cv;
        std::thread thread;
        bool closing = false;
        bool flush_requested = false;
    };

    void record(const SyncTraceEvent& event);
    Sink& sink_for(const SyncTraceEvent& event);
    void run_sink(Sink& sink);
    void write_header(Sink& sink);

    KTraceDirectoryWriterOptions options_;
    std::mutex sinks_mutex_;
    std::unique_ptr<Sink> server_;
    std::unordered_map<ClientId, std::unique_ptr<Sink>> clients_;
    SyncTracer tracer_;
};

class KTraceReader {
public:
    KTraceFile read_file(const std::string& path) const;
    SyncTraceHistory read_directory(const std::string& directory) const;

private:
    static KTraceSourceHistory build_source_history(KTraceFile file);
};

}  // namespace kage::sync

#endif
