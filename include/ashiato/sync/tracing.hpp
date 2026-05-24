#pragma once

#include "ashiato/sync/types.hpp"

#ifdef ASHIATO_SYNC_ENABLE_TRACING

#ifndef ASHIATO_ENABLE_SERIALIZATION_TRACING
#error "ASHIATO_SYNC_ENABLE_TRACING requires Ashiato to be built with ASHIATO_ENABLE_SERIALIZATION_TRACING"
#endif

#include <condition_variable>
#include <array>
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

namespace ashiato::sync {

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
    ClockSkew = 29,
    SerializationPayload = 30,
    SerializationPayloadTagName = 31
};

enum class SyncTracePayloadSource : std::uint8_t {
    Network = 1,
    Replay = 2
};

using SyncPayloadTraceScope = ashiato::SerializationTraceScope;

inline constexpr std::uint8_t sync_trace_payload_tag_outgoing = 1U << 0U;
inline constexpr std::uint8_t sync_trace_payload_tag_incoming = 1U << 1U;

inline const char* sync_trace_payload_tag_name(std::uint8_t tag_bit) noexcept {
    switch (tag_bit) {
    case sync_trace_payload_tag_outgoing: return "outgoing";
    case sync_trace_payload_tag_incoming: return "incoming";
    default: return "";
    }
}

struct SyncTraceEvent {
    SyncTraceEventType type = SyncTraceEventType::ClientConnected;
    SyncTraceRole role = SyncTraceRole::Server;
    ClientId client = invalid_client_id;
    SyncFrame frame = 0;
    ashiato::Entity server_entity{};
    ashiato::Entity local_entity{};
    ClientEntityNetworkId client_network_id = invalid_client_entity_network_id;
    std::uint32_t wire_network_id = 0;
    std::uint32_t network_version = 0;
    SyncArchetypeId archetype = invalid_sync_archetype_id;
    ashiato::Entity component{};
    ashiato::Entity tag{};
    SyncCueTypeId cue_type = 0;
    ReplicationClientMode previous_mode = ReplicationClientMode::Snap;
    ReplicationClientMode mode = ReplicationClientMode::Snap;
    bool remove = false;
    SyncTracePayloadSource payload_source = SyncTracePayloadSource::Network;
    std::uint64_t wire_bits = 0;
    std::uint64_t payload_bits = 0;
    std::uint8_t payload_tag_bits = 0;
    std::vector<SyncPayloadTraceScope> payload_scopes;
    std::string data;
    std::string component_name;
};

inline void add_sync_trace_payload_tag(SyncTraceEvent& event, std::uint8_t tag_bit) noexcept {
    event.payload_tag_bits = static_cast<std::uint8_t>(event.payload_tag_bits | tag_bit);
}

inline bool sync_trace_payload_has_tag(const SyncTraceEvent& event, std::uint8_t tag_bit) noexcept {
    return (event.payload_tag_bits & tag_bit) != 0U;
}

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
    std::function<void(const SyncTraceEvent&)> on_serialization_payload;
#ifdef ASHIATO_SYNC_TRACE_PACKET_LOGS
    std::function<void(const SyncTraceEvent&)> on_packet_log;
#endif
};

class SyncTracer {
public:
    explicit SyncTracer(SyncTraceCallbacks callbacks = {});

    bool enabled() const noexcept { return enabled_; }
    bool frame_data_enabled() const noexcept { return frame_data_enabled_; }
#ifdef ASHIATO_SYNC_TRACE_PACKET_LOGS
    bool packet_logs_enabled() const noexcept { return packet_logs_enabled_; }
#endif
    bool serialization_payloads_enabled() const noexcept { return serialization_payloads_enabled_; }
    bool traces_client(ClientId client) const;

    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    void set_frame_data_enabled(bool enabled) noexcept { frame_data_enabled_ = enabled; }
#ifdef ASHIATO_SYNC_TRACE_PACKET_LOGS
    void set_packet_logs_enabled(bool enabled) noexcept { packet_logs_enabled_ = enabled; }
#endif
    void set_serialization_payloads_enabled(bool enabled) noexcept { serialization_payloads_enabled_ = enabled; }
    void set_monitored_clients(std::vector<ClientId> clients);
    void clear_monitored_clients();
    void set_callbacks(SyncTraceCallbacks callbacks);
    const SyncTraceCallbacks& callbacks() const noexcept { return callbacks_; }

    void trace(const SyncTraceEvent& event) const;

private:
    bool enabled_ = true;
    bool frame_data_enabled_ = false;
    bool serialization_payloads_enabled_ = false;
    std::vector<ClientId> monitored_clients_;
#ifdef ASHIATO_SYNC_TRACE_PACKET_LOGS
    bool packet_logs_enabled_ = false;
#endif
    SyncTraceCallbacks callbacks_;
};

class ScopedSerializationTraceCapture {
public:
    ScopedSerializationTraceCapture(
        const SyncTracer* tracer,
        SyncTracePayloadSource source,
        SyncTraceRole role,
        ClientId client,
        SyncFrame frame,
        const char* root_name,
        bool auto_flush = true);
    ~ScopedSerializationTraceCapture();

    ScopedSerializationTraceCapture(const ScopedSerializationTraceCapture&) = delete;
    ScopedSerializationTraceCapture& operator=(const ScopedSerializationTraceCapture&) = delete;

    bool active() const noexcept { return capture_.active(); }
    SyncTraceEvent& event() noexcept { return event_; }
    const SyncTraceEvent& event() const noexcept { return event_; }
    void set_target(const ashiato::BitBuffer* target) noexcept;
    void finish();
    SyncTraceEvent release_event();
    void flush();
    void truncate_to_bits(std::uint64_t bit_size);
    std::uint32_t push_scope(const char* name);
    void pop_scope(std::uint32_t id);
    ashiato::SerializationTraceCapture* payload_capture() noexcept { return &capture_; }
    const ashiato::SerializationTraceCapture* payload_capture() const noexcept { return &capture_; }

private:
    friend class ScopedSerializationTraceScope;

    const SyncTracer* tracer_ = nullptr;
    const ashiato::BitBuffer* target_ = nullptr;
    ashiato::SerializationTraceCapture capture_{false};
    SyncTraceEvent event_;
    bool flushed_ = false;
    bool auto_flush_ = true;
};

class ScopedSerializationTraceScope {
public:
    ScopedSerializationTraceScope(ScopedSerializationTraceCapture* capture, const char* name);
    ScopedSerializationTraceScope(ashiato::ComponentSerializationContext& context, const char* name);
    ~ScopedSerializationTraceScope();

    ScopedSerializationTraceScope(const ScopedSerializationTraceScope&) = delete;
    ScopedSerializationTraceScope& operator=(const ScopedSerializationTraceScope&) = delete;

private:
    ScopedSerializationTraceCapture* capture_ = nullptr;
    ashiato::SerializationTraceCapture* payload_capture_ = nullptr;
    std::uint32_t id_ = UINT32_MAX;
};

#define ASHIATO_SYNC_TRACE_SCOPE(name) ASHIATO_SERIALIZATION_TRACE_SCOPE(name)
#define ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(trace_context, name) \
    ::ashiato::sync::ScopedSerializationTraceScope ASHIATO_SYNC_TRACE_SCOPE_CONCAT(_ashiato_sync_trace_scope_, __LINE__)(&(trace_context), name)
#define ASHIATO_SYNC_TRACE_SCOPE_WITH_CAPTURE(trace_capture, name) \
    ::ashiato::sync::ScopedSerializationTraceScope ASHIATO_SYNC_TRACE_SCOPE_CONCAT(_ashiato_sync_trace_scope_, __LINE__)((trace_capture), name)
#define PAYLOAD_TRACE_SCOPE(name) ASHIATO_SYNC_TRACE_SCOPE(name)
#define SERIALIZE_TRACE(bitbuffer, data, bits, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE(name); \
        (bitbuffer).push_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_TRACE_WITH_CONTEXT(trace_context, bitbuffer, data, bits, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(trace_context, name); \
        (bitbuffer).push_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_TRACE_WITH_CAPTURE(trace_capture, bitbuffer, data, bits, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CAPTURE(trace_capture, name); \
        (bitbuffer).push_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_UNSIGNED_TRACE(bitbuffer, data, bits, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE(name); \
        (bitbuffer).push_unsigned_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_UNSIGNED_TRACE_WITH_CONTEXT(trace_context, bitbuffer, data, bits, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(trace_context, name); \
        (bitbuffer).push_unsigned_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_UNSIGNED_TRACE_WITH_CAPTURE(trace_capture, bitbuffer, data, bits, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CAPTURE(trace_capture, name); \
        (bitbuffer).push_unsigned_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_BOOL_TRACE(bitbuffer, data, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE(name); \
        (bitbuffer).push_bool((data)); \
    } while (false)
#define SERIALIZE_BOOL_TRACE_WITH_CONTEXT(trace_context, bitbuffer, data, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(trace_context, name); \
        (bitbuffer).push_bool((data)); \
    } while (false)
#define SERIALIZE_BOOL_TRACE_WITH_CAPTURE(trace_capture, bitbuffer, data, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CAPTURE(trace_capture, name); \
        (bitbuffer).push_bool((data)); \
    } while (false)
#define SERIALIZE_BYTES_TRACE(bitbuffer, data, bytes, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE(name); \
        (bitbuffer).push_bytes((data), (bytes)); \
    } while (false)
#define SERIALIZE_BYTES_TRACE_WITH_CONTEXT(trace_context, bitbuffer, data, bytes, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(trace_context, name); \
        (bitbuffer).push_bytes((data), (bytes)); \
    } while (false)
#define SERIALIZE_BYTES_TRACE_WITH_CAPTURE(trace_capture, bitbuffer, data, bytes, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CAPTURE(trace_capture, name); \
        (bitbuffer).push_bytes((data), (bytes)); \
    } while (false)
#define SERIALIZE_BUFFER_TRACE_WITH_CONTEXT(trace_context, bitbuffer, source, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(trace_context, name); \
        (bitbuffer).push_buffer_bits((source)); \
    } while (false)
#define SERIALIZE_BUFFER_TRACE_WITH_CAPTURE(trace_capture, bitbuffer, source, name) \
    do { \
        ASHIATO_SYNC_TRACE_SCOPE_WITH_CAPTURE(trace_capture, name); \
        (bitbuffer).push_buffer_bits((source)); \
    } while (false)
#define ASHIATO_SYNC_TRACE_SCOPE_CONCAT(a, b) ASHIATO_SYNC_TRACE_SCOPE_CONCAT_INNER(a, b)
#define ASHIATO_SYNC_TRACE_SCOPE_CONCAT_INNER(a, b) a##b
#ifndef BEGIN_TRACE_SCOPE
#define BEGIN_TRACE_SCOPE(name) ASHIATO_SYNC_TRACE_SCOPE(name)
#endif

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

inline constexpr std::uint16_t ktrace_format_version = 8;
inline constexpr ClientId ktrace_server_source_id = invalid_client_id;
inline constexpr std::uint32_t ktrace_flag_packet_logs = 1U << 0U;
inline constexpr std::uint32_t ktrace_flag_serialization_payloads = 1U << 1U;

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
    std::array<std::string, 8> payload_tag_names;
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

using KTraceRunId = std::uint32_t;
constexpr KTraceRunId invalid_trace_run = UINT32_MAX;

struct KTraceFrameRun {
    SyncFrame start_frame = 0;
    std::vector<KTraceFrameCell> frames;
    KTraceRunId prev = invalid_trace_run;
    std::vector<KTraceRunId> next;
};

struct KTraceComponentRow {
    ashiato::Entity component{};
    std::vector<KTraceFrameRun> runs;
    std::vector<KTraceRunId> run_skiplist;
    KTraceRunId active_run = invalid_trace_run;
    bool pending_run_split = false;
    SyncFrame pending_split_frame = 0;
};

struct KTraceEntityRow {
    ClientEntityNetworkId client_network_id = invalid_client_entity_network_id;
    std::uint32_t wire_network_id = 0;
    std::uint32_t network_version = 0;
    ashiato::Entity server_entity{};
    ashiato::Entity local_entity{};
    SyncArchetypeId archetype = invalid_sync_archetype_id;
    std::vector<KTraceComponentRow> components;
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
    std::array<std::string, 8> payload_tag_names;
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
        std::uint8_t named_payload_tags = 0;
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

}  // namespace ashiato::sync

#endif

#ifndef ASHIATO_SYNC_ENABLE_TRACING
#define ASHIATO_SYNC_TRACE_SCOPE(name) ((void)0)
#define ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(trace_context, name) ((void)0)
#define ASHIATO_SYNC_TRACE_SCOPE_WITH_CAPTURE(trace_capture, name) ((void)0)
#define PAYLOAD_TRACE_SCOPE(name) ((void)0)
#define SERIALIZE_TRACE(bitbuffer, data, bits, name) \
    do { \
        (bitbuffer).push_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_TRACE_WITH_CONTEXT(trace_context, bitbuffer, data, bits, name) \
    do { \
        (bitbuffer).push_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_TRACE_WITH_CAPTURE(trace_capture, bitbuffer, data, bits, name) \
    do { \
        (bitbuffer).push_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_UNSIGNED_TRACE(bitbuffer, data, bits, name) \
    do { \
        (bitbuffer).push_unsigned_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_UNSIGNED_TRACE_WITH_CONTEXT(trace_context, bitbuffer, data, bits, name) \
    do { \
        (bitbuffer).push_unsigned_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_UNSIGNED_TRACE_WITH_CAPTURE(trace_capture, bitbuffer, data, bits, name) \
    do { \
        (bitbuffer).push_unsigned_bits((data), (bits)); \
    } while (false)
#define SERIALIZE_BOOL_TRACE(bitbuffer, data, name) \
    do { \
        (bitbuffer).push_bool((data)); \
    } while (false)
#define SERIALIZE_BOOL_TRACE_WITH_CONTEXT(trace_context, bitbuffer, data, name) \
    do { \
        (bitbuffer).push_bool((data)); \
    } while (false)
#define SERIALIZE_BOOL_TRACE_WITH_CAPTURE(trace_capture, bitbuffer, data, name) \
    do { \
        (bitbuffer).push_bool((data)); \
    } while (false)
#define SERIALIZE_BYTES_TRACE(bitbuffer, data, bytes, name) \
    do { \
        (bitbuffer).push_bytes((data), (bytes)); \
    } while (false)
#define SERIALIZE_BYTES_TRACE_WITH_CONTEXT(trace_context, bitbuffer, data, bytes, name) \
    do { \
        (bitbuffer).push_bytes((data), (bytes)); \
    } while (false)
#define SERIALIZE_BYTES_TRACE_WITH_CAPTURE(trace_capture, bitbuffer, data, bytes, name) \
    do { \
        (bitbuffer).push_bytes((data), (bytes)); \
    } while (false)
#define SERIALIZE_BUFFER_TRACE_WITH_CONTEXT(trace_context, bitbuffer, source, name) \
    do { \
        (bitbuffer).push_buffer_bits((source)); \
    } while (false)
#define SERIALIZE_BUFFER_TRACE_WITH_CAPTURE(trace_capture, bitbuffer, source, name) \
    do { \
        (bitbuffer).push_buffer_bits((source)); \
    } while (false)
#ifndef BEGIN_TRACE_SCOPE
#define BEGIN_TRACE_SCOPE(name) ((void)0)
#endif
#endif
