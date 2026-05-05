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
    CueEmitted = 15,
    CueSent = 16,
    CueReceived = 17,
    CuePlayed = 18,
    CueRolledBack = 19,
    CueConfirmed = 20,
    TagReceived = 21,
    EntityDestroyed = 22,
    ResimulatedFrameComponent = 23,
    ComponentName = 24,
    CueName = 25,
    RollbackReason = 26,
    InputStarved = 27,
    PacketLog = 28,
    ClockSkew = 29
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
    std::function<void(const SyncTraceEvent&)> on_cue_emitted;
    std::function<void(const SyncTraceEvent&)> on_cue_sent;
    std::function<void(const SyncTraceEvent&)> on_cue_rolled_back;
    std::function<void(const SyncTraceEvent&)> on_cue_received;
    std::function<void(const SyncTraceEvent&)> on_cue_played;
    std::function<void(const SyncTraceEvent&)> on_cue_confirmed;
    std::function<void(const SyncTraceEvent&)> on_entity_destroyed;
    std::function<void(const SyncTraceEvent&)> on_resimulated_frame_component;
    std::function<void(const SyncTraceEvent&)> on_component_name;
    std::function<void(const SyncTraceEvent&)> on_cue_name;
    std::function<void(const SyncTraceEvent&)> on_rollback_reason;
    std::function<void(const SyncTraceEvent&)> on_input_starved;
    std::function<void(const SyncTraceEvent&)> on_clock_skew;
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    std::function<void(const SyncTraceEvent&)> on_packet_log;
#endif
};

class SyncTracer {
public:
    explicit SyncTracer(SyncTraceCallbacks callbacks = {});

    bool enabled() const noexcept { return enabled_; }
    bool frame_data_enabled() const noexcept { return frame_data_enabled_; }
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    bool packet_logs_enabled() const noexcept { return packet_logs_enabled_; }
#endif

    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    void set_frame_data_enabled(bool enabled) noexcept { frame_data_enabled_ = enabled; }
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    void set_packet_logs_enabled(bool enabled) noexcept { packet_logs_enabled_ = enabled; }
#endif
    void set_callbacks(SyncTraceCallbacks callbacks);
    const SyncTraceCallbacks& callbacks() const noexcept { return callbacks_; }

    void trace(const SyncTraceEvent& event) const;

private:
    bool enabled_ = true;
    bool frame_data_enabled_ = false;
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    bool packet_logs_enabled_ = false;
#endif
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
        std::unordered_set<std::uint64_t> named_cues;
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

inline constexpr std::uint16_t ktrace_format_version = 6;
inline constexpr ClientId ktrace_server_source_id = invalid_client_id;
inline constexpr std::uint32_t ktrace_flag_packet_logs = 1U << 0U;

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

struct KTraceFileHeader {
    std::string path;
    SyncTraceRole role = SyncTraceRole::Server;
    ClientId client = invalid_client_id;
    std::uint64_t recorded_unix_ns = 0;
    std::uint16_t version = 0;
    std::uint32_t flags = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t file_size = 0;
};

struct KTraceFile {
    std::string path;
    SyncTraceRole role = SyncTraceRole::Server;
    ClientId client = invalid_client_id;
    std::uint64_t recorded_unix_ns = 0;
    std::uint16_t version = 0;
    std::uint32_t flags = 0;
    std::vector<KTraceRecord> records;
    std::unordered_map<std::uint64_t, std::string> component_names;
    std::unordered_map<std::uint64_t, std::string> cue_names;
};

class KTraceStreamReader {
public:
    explicit KTraceStreamReader(const std::string& path);

    const KTraceFileHeader& header() const noexcept { return header_; }
    std::uint64_t position() const noexcept { return position_; }
    bool read_next(KTraceRecord& out);

private:
    std::ifstream file_;
    KTraceFileHeader header_;
    std::uint64_t position_ = 0;
};

enum class KTraceCellState : std::uint32_t {
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
    Resimulated = 1U << 10U,
    InputReceived = 1U << 11U,
    CueEmitted = 1U << 12U,
    CueSent = 1U << 13U,
    CueReceived = 1U << 14U,
    CuePlayed = 1U << 15U,
    CueConfirmed = 1U << 16U,
    CueRolledBack = 1U << 17U
};

struct KTraceFrameCell {
    SyncFrame frame = 0;
    std::uint32_t state_mask = 0;
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
    std::uint32_t flags = 0;
    std::vector<KTraceRecord> records;
    std::vector<KTraceEntityRow> entities;
    std::unordered_map<std::uint64_t, std::string> component_names;
    std::unordered_map<std::uint64_t, std::string> cue_names;
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
        std::unordered_set<std::uint64_t> named_cues;
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
    static KTraceSourceHistory build_source_history(KTraceFile file);
};

std::unique_ptr<KTraceDirectoryWriter> make_trace_writer(const TraceOptions& options);

}  // namespace kage::sync

#endif
