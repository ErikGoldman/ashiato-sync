#include "kage/sync/tracing.hpp"

#ifdef KAGE_SYNC_ENABLE_TRACING

#include <chrono>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace kage::sync {
namespace {

template <typename T>
void append_pod(std::vector<std::uint8_t>& out, T value) {
    static_assert(std::is_integral<T>::value, "trace pod must be integral");
    const auto raw = static_cast<typename std::make_unsigned<T>::type>(value);
    for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
        out.push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(raw) >> (byte * 8U)) & 0xffU));
    }
}

void append_string(std::vector<std::uint8_t>& out, const std::string& value) {
    append_pod<std::uint32_t>(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

template <typename T>
T read_pod(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    static_assert(std::is_integral<T>::value, "trace pod must be integral");
    if (offset + sizeof(T) > bytes.size()) {
        throw std::runtime_error("ktrace record is truncated");
    }
    typename std::make_unsigned<T>::type raw = 0;
    for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
        raw |= static_cast<typename std::make_unsigned<T>::type>(bytes[offset + byte]) << (byte * 8U);
    }
    offset += sizeof(T);
    return static_cast<T>(raw);
}

std::string read_string(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    const std::uint32_t size = read_pod<std::uint32_t>(bytes, offset);
    if (offset + size > bytes.size()) {
        throw std::runtime_error("ktrace string is truncated");
    }
    std::string value(reinterpret_cast<const char*>(bytes.data() + offset), size);
    offset += size;
    return value;
}

std::uint8_t read_trace_byte(std::ifstream& file) {
    char byte = 0;
    file.read(&byte, 1);
    if (!file) {
        throw std::runtime_error("ktrace header is truncated");
    }
    return static_cast<std::uint8_t>(byte);
}

std::uint16_t read_trace_u16(std::ifstream& file) {
    return static_cast<std::uint16_t>(read_trace_byte(file) | (std::uint16_t{read_trace_byte(file)} << 8U));
}

std::uint32_t read_trace_u32(std::ifstream& file) {
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < 4U; ++byte) {
        value |= std::uint32_t{read_trace_byte(file)} << (byte * 8U);
    }
    return value;
}

std::uint64_t read_trace_u64(std::ifstream& file) {
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < 8U; ++byte) {
        value |= std::uint64_t{read_trace_byte(file)} << (byte * 8U);
    }
    return value;
}

KTraceFileHeader read_trace_file_header(std::ifstream& file, const std::string& path) {
    char magic[8]{};
    file.read(magic, sizeof(magic));
    if (std::string(magic, sizeof(magic)) != "KTRACE03") {
        throw std::runtime_error("unsupported ktrace magic");
    }

    KTraceFileHeader header;
    header.path = path;
    header.version = read_trace_u16(file);
    if (header.version != ktrace_format_version) {
        throw std::runtime_error("unsupported ktrace version");
    }
    header.role = static_cast<SyncTraceRole>(read_trace_byte(file));
    header.recorded_unix_ns = read_trace_u64(file);
    header.client = read_trace_u64(file);
    header.flags = read_trace_u32(file);
    header.data_offset = static_cast<std::uint64_t>(file.tellg());
    std::error_code ec;
    header.file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        header.file_size = header.data_offset;
    }
    return header;
}

std::uint64_t unix_time_ns() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

std::uint64_t steady_time_us() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

void append_event_payload(std::vector<std::uint8_t>& payload, const SyncTraceEvent& event) {
    append_pod<std::uint8_t>(payload, static_cast<std::uint8_t>(event.role));
    append_pod<std::uint64_t>(payload, event.client);
    append_pod<std::uint32_t>(payload, event.frame);
    append_pod<std::uint64_t>(payload, event.server_entity.value);
    append_pod<std::uint64_t>(payload, event.local_entity.value);
    append_pod<std::uint64_t>(payload, event.client_network_id);
    append_pod<std::uint32_t>(payload, event.wire_network_id);
    append_pod<std::uint32_t>(payload, event.network_version);
    append_pod<std::uint32_t>(payload, event.archetype.value);
    append_pod<std::uint64_t>(payload, event.component.value);
    append_pod<std::uint64_t>(payload, event.tag.value);
    append_pod<std::uint16_t>(payload, event.cue_type);
    append_pod<std::uint8_t>(payload, static_cast<std::uint8_t>(event.previous_mode));
    append_pod<std::uint8_t>(payload, static_cast<std::uint8_t>(event.mode));
    append_pod<std::uint8_t>(payload, event.remove ? 1U : 0U);
    append_string(payload, event.data);
    append_string(payload, event.component_name);
}

SyncTraceEvent read_event_payload(
    SyncTraceEventType type,
    const std::vector<std::uint8_t>& payload) {
    std::size_t offset = 0;
    SyncTraceEvent event;
    event.type = type;
    event.role = static_cast<SyncTraceRole>(read_pod<std::uint8_t>(payload, offset));
    event.client = read_pod<std::uint64_t>(payload, offset);
    event.frame = read_pod<std::uint32_t>(payload, offset);
    event.server_entity = ecs::Entity{read_pod<std::uint64_t>(payload, offset)};
    event.local_entity = ecs::Entity{read_pod<std::uint64_t>(payload, offset)};
    event.client_network_id = read_pod<std::uint64_t>(payload, offset);
    event.wire_network_id = read_pod<std::uint32_t>(payload, offset);
    event.network_version = read_pod<std::uint32_t>(payload, offset);
    event.archetype = SyncArchetypeId{read_pod<std::uint32_t>(payload, offset)};
    event.component = ecs::Entity{read_pod<std::uint64_t>(payload, offset)};
    event.tag = ecs::Entity{read_pod<std::uint64_t>(payload, offset)};
    event.cue_type = read_pod<std::uint16_t>(payload, offset);
    event.previous_mode = static_cast<ReplicationClientMode>(read_pod<std::uint8_t>(payload, offset));
    event.mode = static_cast<ReplicationClientMode>(read_pod<std::uint8_t>(payload, offset));
    event.remove = read_pod<std::uint8_t>(payload, offset) != 0U;
    event.data = read_string(payload, offset);
    event.component_name = read_string(payload, offset);
    if (offset != payload.size()) {
        throw std::runtime_error("ktrace record has trailing bytes");
    }
    return event;
}

SyncTraceEvent make_component_name_event(const SyncTraceEvent& event) {
    SyncTraceEvent mapping;
    mapping.type = SyncTraceEventType::ComponentName;
    mapping.role = event.role;
    mapping.client = event.client;
    mapping.frame = event.frame;
    mapping.component = event.component;
    mapping.data = event.component_name;
    mapping.component_name = event.component_name;
    return mapping;
}

SyncTraceEvent make_cue_name_event(const SyncTraceEvent& event) {
    SyncTraceEvent mapping;
    mapping.type = SyncTraceEventType::CueName;
    mapping.role = event.role;
    mapping.client = event.client;
    mapping.frame = event.frame;
    mapping.cue_type = event.cue_type;
    mapping.data = event.component_name;
    return mapping;
}

bool event_has_cue_name(const SyncTraceEvent& event) {
    switch (event.type) {
    case SyncTraceEventType::CueEmitted:
    case SyncTraceEventType::CueSent:
    case SyncTraceEventType::CueReceived:
    case SyncTraceEventType::CuePlayed:
    case SyncTraceEventType::CueRolledBack:
    case SyncTraceEventType::CueConfirmed:
        return !event.component_name.empty();
    default:
        return false;
    }
}

SyncTraceEvent stored_trace_event(SyncTraceEvent event) {
    if (event_has_cue_name(event)) {
        event.component_name.clear();
    }
    return event;
}

std::vector<std::uint8_t> encode_v2_record(const SyncTraceEvent& event, std::uint64_t timestamp_us) {
    std::vector<std::uint8_t> payload;
    append_event_payload(payload, event);

    std::vector<std::uint8_t> out;
    out.reserve(1U + 8U + 4U + payload.size());
    append_pod<std::uint8_t>(out, static_cast<std::uint8_t>(event.type));
    append_pod<std::uint64_t>(out, timestamp_us);
    append_pod<std::uint32_t>(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

ClientEntityNetworkId event_network_key(const SyncTraceEvent& event) {
    if (event.role == SyncTraceRole::Server && event.server_entity) {
        return event.server_entity.value;
    }
    if (event.client_network_id != invalid_client_entity_network_id) {
        return event.client_network_id;
    }
    if (event.client != invalid_client_id && event.wire_network_id != 0U) {
        return make_client_entity_network_id(event.client, event.wire_network_id, event.network_version);
    }
    if (event.server_entity) {
        return event.server_entity.value;
    }
    return invalid_client_entity_network_id;
}

KTraceFrameRun& root_run(KTraceComponentRow& row) {
    if (row.runs.empty()) {
        KTraceFrameRun run;
        run.start_frame = 0;
        row.runs.push_back(std::move(run));
        row.active_run = 0;
    }
    if (row.active_run == invalid_trace_run || row.active_run >= row.runs.size()) {
        row.active_run = 0;
    }
    return row.runs[0];
}

KTraceFrameRun& active_run(KTraceComponentRow& row) {
    (void)root_run(row);
    return row.runs[row.active_run];
}

void add_cell_state(KTraceFrameRun& run, SyncFrame frame, KTraceCellState state, std::uint32_t event_index) {
    auto found = std::lower_bound(run.frames.begin(), run.frames.end(), frame, [](const KTraceFrameCell& cell, SyncFrame target) {
        return cell.frame < target;
    });
    if (found == run.frames.end() || found->frame != frame) {
        found = run.frames.insert(found, KTraceFrameCell{frame, 0, {}});
    }
    found->state_mask |= static_cast<std::uint32_t>(state);
    if (event_index != std::numeric_limits<std::uint32_t>::max() &&
        std::find(found->event_indices.begin(), found->event_indices.end(), event_index) ==
        found->event_indices.end()) {
        found->event_indices.push_back(event_index);
    }
}

void add_cell_state(KTraceComponentRow& row, SyncFrame frame, KTraceCellState state, std::uint32_t event_index) {
    add_cell_state(active_run(row), frame, state, event_index);
}

KTraceFrameRun* run_containing_event(KTraceComponentRow& row, SyncFrame frame, std::uint32_t event_index) {
    KTraceFrameRun* selected = nullptr;
    for (KTraceFrameRun& run : row.runs) {
        KTraceFrameCell* cell = nullptr;
        auto found = std::find_if(run.frames.begin(), run.frames.end(), [frame](const KTraceFrameCell& candidate) {
            return candidate.frame == frame;
        });
        if (found != run.frames.end()) {
            cell = &*found;
        }
        if (cell != nullptr &&
            std::find(cell->event_indices.begin(), cell->event_indices.end(), event_index) != cell->event_indices.end()) {
            selected = &run;
        }
    }
    return selected;
}

ecs::Entity cue_row_component() noexcept {
    return ecs::Entity{std::uint64_t{1} << 63U};
}

void add_cell_event(KTraceComponentRow& row, SyncFrame frame, std::uint32_t event_index) {
    add_cell_state(row, frame, static_cast<KTraceCellState>(0), event_index);
}

KTraceComponentRow& component_row(std::vector<KTraceComponentRow>& rows, ecs::Entity component) {
    auto found = std::find_if(rows.begin(), rows.end(), [component](const KTraceComponentRow& row) {
        return row.component == component;
    });
    if (found == rows.end()) {
        KTraceComponentRow row;
        row.component = component;
        row.runs.push_back(KTraceFrameRun{});
        row.active_run = 0;
        rows.push_back(std::move(row));
        return rows.back();
    }
    (void)root_run(*found);
    return *found;
}

const KTraceFrameCell* find_cell(const KTraceFrameRun& run, SyncFrame frame) {
    const auto found = std::find_if(run.frames.begin(), run.frames.end(), [frame](const KTraceFrameCell& cell) {
        return cell.frame == frame;
    });
    return found != run.frames.end() ? &*found : nullptr;
}

void copy_split_frame_authoritative_state(
    const KTraceComponentRow& row,
    KTraceFrameRun& child,
    SyncFrame frame,
    const KTraceSourceHistory& source) {
    if (row.active_run >= row.runs.size()) {
        return;
    }
    const KTraceFrameRun& parent = row.runs[row.active_run];
    const KTraceFrameCell* parent_cell = find_cell(parent, frame);
    if (parent_cell == nullptr) {
        return;
    }
    for (std::uint32_t event_index : parent_cell->event_indices) {
        if (event_index >= source.records.size()) {
            continue;
        }
        const SyncTraceEventType type = source.records[event_index].event.type;
        if (type == SyncTraceEventType::ComponentReceived) {
            add_cell_state(child, frame, KTraceCellState::ReceivedFromServer, event_index);
        }
    }
}

KTraceFrameRun& ensure_resimulation_run(KTraceComponentRow& row, SyncFrame frame, const KTraceSourceHistory& source) {
    (void)root_run(row);
    if (!row.pending_run_split) {
        return active_run(row);
    }
    const KTraceRunId parent = row.active_run;
    KTraceFrameRun child;
    child.start_frame = row.pending_split_frame;
    child.prev = parent;
    row.runs.push_back(std::move(child));
    const KTraceRunId child_id = static_cast<KTraceRunId>(row.runs.size() - 1U);
    row.runs[parent].next.push_back(child_id);
    copy_split_frame_authoritative_state(row, row.runs[child_id], row.pending_split_frame, source);
    if (frame != row.pending_split_frame) {
        copy_split_frame_authoritative_state(row, row.runs[child_id], frame, source);
    }
    add_cell_state(row.runs[child_id], row.pending_split_frame, KTraceCellState::Resimulated, std::numeric_limits<std::uint32_t>::max());
    row.active_run = child_id;
    row.pending_run_split = false;
    row.pending_split_frame = 0;
    return row.runs[child_id];
}

KTraceEntityRow& entity_row(std::vector<KTraceEntityRow>& rows, const SyncTraceEvent& event) {
    const ClientEntityNetworkId key = event_network_key(event);
    auto found = std::find_if(rows.begin(), rows.end(), [key](const KTraceEntityRow& row) {
        return row.client_network_id == key;
    });
    if (found == rows.end()) {
        KTraceEntityRow row;
        row.client_network_id = key;
        row.wire_network_id = event.wire_network_id;
        row.network_version = event.network_version;
        row.server_entity = event.server_entity;
        row.local_entity = event.local_entity;
        row.archetype = event.archetype;
        rows.push_back(row);
        return rows.back();
    }
    if (event.server_entity) {
        found->server_entity = event.server_entity;
    }
    if (event.local_entity) {
        found->local_entity = event.local_entity;
    }
    if (event.archetype != invalid_sync_archetype_id) {
        found->archetype = event.archetype;
    }
    if (event.wire_network_id != 0U) {
        found->wire_network_id = event.wire_network_id;
    }
    if (event.network_version != 0U) {
        found->network_version = event.network_version;
    }
    return *found;
}

}  // namespace

SyncTracer::SyncTracer(SyncTraceCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

void SyncTracer::set_callbacks(SyncTraceCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void SyncTracer::trace(const SyncTraceEvent& event) const {
    if (!enabled_) {
        return;
    }
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    if (event.type == SyncTraceEventType::PacketLog && !packet_logs_enabled_) {
        return;
    }
#endif
    if (callbacks_.on_event) {
        callbacks_.on_event(event);
    }
    switch (event.type) {
    case SyncTraceEventType::ClientConnected:
        if (callbacks_.on_client_connected) callbacks_.on_client_connected(event);
        break;
    case SyncTraceEventType::ClientDisconnected:
        if (callbacks_.on_client_disconnected) callbacks_.on_client_disconnected(event);
        break;
    case SyncTraceEventType::EntityStartedSyncing:
        if (callbacks_.on_entity_started_syncing) callbacks_.on_entity_started_syncing(event);
        break;
    case SyncTraceEventType::EntityReceived:
        if (callbacks_.on_entity_received) callbacks_.on_entity_received(event);
        break;
    case SyncTraceEventType::ComponentSent:
        if (callbacks_.on_component_sent) callbacks_.on_component_sent(event);
        break;
    case SyncTraceEventType::ComponentReceived:
        if (callbacks_.on_component_received) callbacks_.on_component_received(event);
        break;
    case SyncTraceEventType::ComponentApplied:
        if (callbacks_.on_component_applied) callbacks_.on_component_applied(event);
        break;
    case SyncTraceEventType::ComponentRemoved:
        if (callbacks_.on_component_removed) callbacks_.on_component_removed(event);
        break;
    case SyncTraceEventType::TagSent:
        if (callbacks_.on_tag_sent) callbacks_.on_tag_sent(event);
        break;
    case SyncTraceEventType::TagReceived:
        if (callbacks_.on_tag_received) callbacks_.on_tag_received(event);
        break;
    case SyncTraceEventType::TagApplied:
        if (callbacks_.on_tag_applied) callbacks_.on_tag_applied(event);
        break;
    case SyncTraceEventType::ModeChanged:
        if (callbacks_.on_mode_changed) callbacks_.on_mode_changed(event);
        break;
    case SyncTraceEventType::BufferedStarved:
        if (callbacks_.on_buffered_starved) callbacks_.on_buffered_starved(event);
        break;
    case SyncTraceEventType::PredictionRollbackConflict:
        if (callbacks_.on_prediction_rollback_conflict) callbacks_.on_prediction_rollback_conflict(event);
        break;
    case SyncTraceEventType::FrameComponent:
        if (callbacks_.on_frame_component) callbacks_.on_frame_component(event);
        break;
    case SyncTraceEventType::CueEmitted:
        if (callbacks_.on_cue_emitted) callbacks_.on_cue_emitted(event);
        break;
    case SyncTraceEventType::CueSent:
        if (callbacks_.on_cue_sent) callbacks_.on_cue_sent(event);
        break;
    case SyncTraceEventType::CueRolledBack:
        if (callbacks_.on_cue_rolled_back) callbacks_.on_cue_rolled_back(event);
        break;
    case SyncTraceEventType::CueReceived:
        if (callbacks_.on_cue_received) callbacks_.on_cue_received(event);
        break;
    case SyncTraceEventType::CuePlayed:
        if (callbacks_.on_cue_played) callbacks_.on_cue_played(event);
        break;
    case SyncTraceEventType::CueConfirmed:
        if (callbacks_.on_cue_confirmed) callbacks_.on_cue_confirmed(event);
        break;
    case SyncTraceEventType::EntityDestroyed:
        if (callbacks_.on_entity_destroyed) callbacks_.on_entity_destroyed(event);
        break;
    case SyncTraceEventType::ResimulatedFrameComponent:
        if (callbacks_.on_resimulated_frame_component) callbacks_.on_resimulated_frame_component(event);
        break;
    case SyncTraceEventType::ComponentName:
        if (callbacks_.on_component_name) callbacks_.on_component_name(event);
        break;
    case SyncTraceEventType::CueName:
        if (callbacks_.on_cue_name) callbacks_.on_cue_name(event);
        break;
    case SyncTraceEventType::RollbackReason:
        if (callbacks_.on_rollback_reason) callbacks_.on_rollback_reason(event);
        break;
    case SyncTraceEventType::InputStarved:
        if (callbacks_.on_input_starved) callbacks_.on_input_starved(event);
        break;
    case SyncTraceEventType::ClockSkew:
        if (callbacks_.on_clock_skew) callbacks_.on_clock_skew(event);
        break;
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    case SyncTraceEventType::PacketLog:
        if (!packet_logs_enabled_) {
            break;
        }
        if (callbacks_.on_packet_log) callbacks_.on_packet_log(event);
        break;
#else
    case SyncTraceEventType::PacketLog:
        break;
#endif
    }
}

BinarySyncTraceWriter::BinarySyncTraceWriter(BinarySyncTraceWriterOptions options)
    : options_(std::move(options)),
      tracer_([this]() {
          SyncTraceCallbacks callbacks;
          callbacks.on_event = [this](const SyncTraceEvent& event) { record(event); };
          return callbacks;
      }()) {
    if (!options_.server_path.empty()) {
        server_.file.open(options_.server_path, std::ios::binary | std::ios::trunc);
        if (!server_.file) {
            throw std::runtime_error("failed to open server sync trace file");
        }
        write_header(server_.file, SyncTraceRole::Server);
        server_.thread = std::thread([this]() { run_sink(server_); });
    }
    if (!options_.client_path.empty()) {
        client_.file.open(options_.client_path, std::ios::binary | std::ios::trunc);
        if (!client_.file) {
            close();
            throw std::runtime_error("failed to open client sync trace file");
        }
        write_header(client_.file, SyncTraceRole::Client);
        client_.thread = std::thread([this]() { run_sink(client_); });
    }
}

BinarySyncTraceWriter::~BinarySyncTraceWriter() {
    close();
}

void BinarySyncTraceWriter::write_header(std::ofstream& file, SyncTraceRole role) {
    const char magic[8] = {'K', 'S', 'Y', 'N', 'C', 'T', 'R', 'C'};
    file.write(magic, sizeof(magic));
    const std::uint8_t version = 1;
    const std::uint8_t role_byte = static_cast<std::uint8_t>(role);
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&role_byte), sizeof(role_byte));
}

void BinarySyncTraceWriter::append_event(std::vector<std::uint8_t>& out, const SyncTraceEvent& event) {
    std::vector<std::uint8_t> payload;
    append_event_payload(payload, event);

    append_pod<std::uint8_t>(out, static_cast<std::uint8_t>(event.type));
    append_pod<std::uint32_t>(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

void BinarySyncTraceWriter::record(const SyncTraceEvent& event) {
    Sink& sink = sink_for(event.role);
    if (!sink.file) {
        return;
    }

    std::lock_guard<std::mutex> lock(sink.mutex);
    if (event.component && !event.component_name.empty() &&
        sink.named_components.insert(event.component.value).second) {
        append_event(sink.pending, make_component_name_event(event));
    }
    if (event_has_cue_name(event) && sink.named_cues.insert(event.cue_type).second) {
        append_event(sink.pending, make_cue_name_event(event));
    }
    append_event(sink.pending, stored_trace_event(event));
    if (sink.pending.size() >= options_.flush_threshold_bytes) {
        sink.flush_requested = true;
        sink.cv.notify_one();
    }
}

void BinarySyncTraceWriter::run_sink(Sink& sink) {
    std::unique_lock<std::mutex> lock(sink.mutex);
    for (;;) {
        sink.cv.wait(lock, [&]() {
            return sink.closing || sink.flush_requested || !sink.pending.empty();
        });
        if (sink.pending.empty() && sink.closing) {
            break;
        }
        sink.writing.swap(sink.pending);
        sink.flush_requested = false;
        lock.unlock();
        if (!sink.writing.empty()) {
            sink.file.write(reinterpret_cast<const char*>(sink.writing.data()), static_cast<std::streamsize>(sink.writing.size()));
        }
        sink.file.flush();
        lock.lock();
        sink.writing.clear();
        sink.cv.notify_all();
    }
}

BinarySyncTraceWriter::Sink& BinarySyncTraceWriter::sink_for(SyncTraceRole role) noexcept {
    return role == SyncTraceRole::Server ? server_ : client_;
}

void BinarySyncTraceWriter::flush() {
    for (Sink* sink : {&server_, &client_}) {
        if (!sink->file) {
            continue;
        }
        {
            std::unique_lock<std::mutex> lock(sink->mutex);
            sink->flush_requested = true;
            sink->cv.notify_one();
            sink->cv.wait(lock, [&]() {
                return sink->pending.empty() && sink->writing.empty() && !sink->flush_requested;
            });
        }
    }
}

void BinarySyncTraceWriter::close() {
    for (Sink* sink : {&server_, &client_}) {
        if (!sink->file) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(sink->mutex);
            sink->closing = true;
            sink->flush_requested = true;
        }
        sink->cv.notify_one();
        if (sink->thread.joinable()) {
            sink->thread.join();
        }
        sink->file.close();
    }
}

KTraceDirectoryWriter::KTraceDirectoryWriter(KTraceDirectoryWriterOptions options)
    : options_(std::move(options)),
      tracer_([this]() {
          SyncTraceCallbacks callbacks;
          callbacks.on_event = [this](const SyncTraceEvent& event) { record(event); };
          return callbacks;
      }()) {
    if (options_.directory.empty()) {
        throw std::invalid_argument("ktrace directory path is empty");
    }
    if (options_.queue_capacity_bytes == 0U) {
        throw std::invalid_argument("ktrace queue capacity must be greater than zero");
    }
    std::filesystem::create_directories(options_.directory);
}

KTraceDirectoryWriter::~KTraceDirectoryWriter() {
    close();
}

void KTraceDirectoryWriter::write_header(Sink& sink) {
    const char magic[8] = {'K', 'T', 'R', 'A', 'C', 'E', '0', '3'};
    sink.file.write(magic, sizeof(magic));
    const std::uint16_t version = ktrace_format_version;
    const std::uint8_t role = static_cast<std::uint8_t>(sink.role);
    sink.file.put(static_cast<char>(version & 0xffU));
    sink.file.put(static_cast<char>((version >> 8U) & 0xffU));
    sink.file.put(static_cast<char>(role));
    for (std::size_t byte = 0; byte < sizeof(sink.recorded_unix_ns); ++byte) {
        sink.file.put(static_cast<char>((sink.recorded_unix_ns >> (byte * 8U)) & 0xffU));
    }
    for (std::size_t byte = 0; byte < sizeof(sink.client); ++byte) {
        sink.file.put(static_cast<char>((sink.client >> (byte * 8U)) & 0xffU));
    }
    const std::uint32_t flags =
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
        tracer_.packet_logs_enabled() ? ktrace_flag_packet_logs : 0U;
#else
        0U;
#endif
    for (std::size_t byte = 0; byte < sizeof(flags); ++byte) {
        sink.file.put(static_cast<char>((flags >> (byte * 8U)) & 0xffU));
    }
}

KTraceDirectoryWriter::Sink& KTraceDirectoryWriter::sink_for(const SyncTraceEvent& event) {
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    const bool server_event = event.role == SyncTraceRole::Server;
    const ClientId client = event.client == invalid_client_id ? 0U : event.client;
    std::unique_ptr<Sink>& slot = server_event ? server_ : clients_[client];
    if (!slot) {
        slot = std::make_unique<Sink>();
        Sink& sink = *slot;
        sink.role = event.role;
        sink.client = server_event ? ktrace_server_source_id : client;
        sink.recorded_unix_ns = unix_time_ns();
        sink.start_steady_us = steady_time_us();
        const std::filesystem::path directory(options_.directory);
        const std::filesystem::path filename = server_event
            ? std::filesystem::path("server.ktrace")
            : std::filesystem::path(std::to_string(client) + ".ktrace");
        sink.path = (directory / filename).string();
        const std::ios::openmode mode =
            std::ios::binary | (options_.truncate_existing ? std::ios::trunc : std::ios::app);
        sink.file.open(sink.path, mode);
        if (!sink.file) {
            throw std::runtime_error("failed to open ktrace file");
        }
        write_header(sink);
        sink.thread = std::thread([this, &sink]() { run_sink(sink); });
    }
    return *slot;
}

void KTraceDirectoryWriter::record(const SyncTraceEvent& event) {
    Sink& sink = sink_for(event);
    const std::uint64_t now_us = steady_time_us();
    const std::uint64_t timestamp_us = now_us >= sink.start_steady_us ? now_us - sink.start_steady_us : 0U;
    std::vector<std::uint8_t> record;
    std::vector<std::uint8_t> name_record;
    bool emit_name = false;
    std::size_t record_size = 0;
    std::unique_lock<std::mutex> lock(sink.mutex);
    if (event.component && !event.component_name.empty() &&
        sink.named_components.insert(event.component.value).second) {
        emit_name = true;
        name_record = encode_v2_record(make_component_name_event(event), timestamp_us);
        record_size += name_record.size();
    }
    if (event_has_cue_name(event) && sink.named_cues.insert(event.cue_type).second) {
        emit_name = true;
        name_record = encode_v2_record(make_cue_name_event(event), timestamp_us);
        record_size += name_record.size();
    }
    record = encode_v2_record(stored_trace_event(event), timestamp_us);
    record_size += record.size();
    sink.cv.wait(lock, [&]() {
        if (sink.closing) {
            return true;
        }
        if (record_size > options_.queue_capacity_bytes) {
            return sink.queue.empty() && sink.queued_bytes == 0U;
        }
        return sink.queued_bytes + record_size <= options_.queue_capacity_bytes;
    });
    if (sink.closing) {
        return;
    }
    sink.queued_bytes += record_size;
    if (emit_name) {
        sink.queue.push_back(std::move(name_record));
    }
    sink.queue.push_back(std::move(record));
    if (sink.queued_bytes >= options_.flush_threshold_bytes) {
        sink.flush_requested = true;
    }
    lock.unlock();
    sink.cv.notify_one();
}

void KTraceDirectoryWriter::run_sink(Sink& sink) {
    std::vector<std::uint8_t> record;
    for (;;) {
        std::unique_lock<std::mutex> lock(sink.mutex);
        sink.cv.wait(lock, [&]() {
            return sink.closing || sink.flush_requested || !sink.queue.empty();
        });
        if (sink.queue.empty()) {
            if (sink.closing) {
                break;
            }
            sink.flush_requested = false;
            sink.file.flush();
            sink.drained_cv.notify_all();
            continue;
        }
        record = std::move(sink.queue.front());
        sink.queue.pop_front();
        sink.queued_bytes -= record.size();
        sink.cv.notify_all();
        const bool flush_after = sink.flush_requested && sink.queue.empty();
        lock.unlock();

        sink.file.write(reinterpret_cast<const char*>(record.data()), static_cast<std::streamsize>(record.size()));
        record.clear();
        if (flush_after) {
            sink.file.flush();
            lock.lock();
            sink.flush_requested = false;
            sink.drained_cv.notify_all();
            lock.unlock();
        }
    }
    sink.file.flush();
    sink.drained_cv.notify_all();
}

void KTraceDirectoryWriter::flush() {
    std::vector<Sink*> sinks;
    {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (server_) {
            sinks.push_back(server_.get());
        }
        for (auto& client : clients_) {
            sinks.push_back(client.second.get());
        }
    }
    for (Sink* sink : sinks) {
        std::unique_lock<std::mutex> lock(sink->mutex);
        sink->flush_requested = true;
        sink->cv.notify_one();
        sink->drained_cv.wait(lock, [&]() {
            return sink->queue.empty() && !sink->flush_requested;
        });
    }
}

void KTraceDirectoryWriter::close() {
    std::vector<Sink*> sinks;
    {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        if (server_) {
            sinks.push_back(server_.get());
        }
        for (auto& client : clients_) {
            sinks.push_back(client.second.get());
        }
    }
    for (Sink* sink : sinks) {
        {
            std::lock_guard<std::mutex> lock(sink->mutex);
            sink->closing = true;
            sink->flush_requested = true;
        }
        sink->cv.notify_all();
    }
    for (Sink* sink : sinks) {
        if (sink->thread.joinable()) {
            sink->thread.join();
        }
        if (sink->file.is_open()) {
            sink->file.close();
        }
    }
}

KTraceStreamReader::KTraceStreamReader(const std::string& path)
    : file_(path, std::ios::binary) {
    if (!file_) {
        throw std::runtime_error("failed to open ktrace file");
    }
    header_ = read_trace_file_header(file_, path);
    position_ = header_.data_offset;
}

bool KTraceStreamReader::read_next(KTraceRecord& out) {
    const int type_byte = file_.get();
    if (type_byte == std::char_traits<char>::eof()) {
        position_ = header_.file_size;
        return false;
    }
    if (!file_) {
        position_ = header_.file_size;
        return false;
    }
    const auto type = static_cast<SyncTraceEventType>(static_cast<std::uint8_t>(type_byte));

    std::uint8_t record_header[12]{};
    file_.read(reinterpret_cast<char*>(record_header), sizeof(record_header));
    if (file_.gcount() != static_cast<std::streamsize>(sizeof(record_header))) {
        position_ = header_.file_size;
        return false;
    }
    std::uint64_t timestamp_us = 0;
    for (std::size_t byte = 0; byte < 8U; ++byte) {
        timestamp_us |= std::uint64_t{record_header[byte]} << (byte * 8U);
    }
    std::uint32_t payload_size = 0;
    for (std::size_t byte = 0; byte < 4U; ++byte) {
        payload_size |= std::uint32_t{record_header[8U + byte]} << (byte * 8U);
    }

    std::vector<std::uint8_t> payload(payload_size);
    if (payload_size != 0U) {
        file_.read(reinterpret_cast<char*>(payload.data()), payload.size());
        if (file_.gcount() != static_cast<std::streamsize>(payload.size())) {
            position_ = header_.file_size;
            return false;
        }
    }
    position_ = static_cast<std::uint64_t>(file_.tellg());
    out = KTraceRecord{read_event_payload(type, payload), timestamp_us};
    return true;
}

KTraceFile KTraceReader::read_file(const std::string& path) const {
    KTraceStreamReader stream(path);
    const KTraceFileHeader& header = stream.header();
    KTraceFile result;
    result.path = header.path;
    result.version = header.version;
    result.role = header.role;
    result.recorded_unix_ns = header.recorded_unix_ns;
    result.client = header.client;
    result.flags = header.flags;

    KTraceRecord record;
    while (stream.read_next(record)) {
        result.records.push_back(std::move(record));
    }
    return result;
}

KTraceSourceHistory KTraceReader::build_source_history(KTraceFile file) {
    KTraceSourceHistory source;
    source.role = file.role;
    source.client = file.client;
    source.path = std::move(file.path);
    source.recorded_unix_ns = file.recorded_unix_ns;
    source.flags = file.flags;
    source.records = std::move(file.records);
    source.component_names = std::move(file.component_names);
    source.cue_names = std::move(file.cue_names);

    std::unordered_map<ClientEntityNetworkId, ReplicationClientMode> modes;
    std::unordered_map<ClientEntityNetworkId, std::unordered_map<std::uint64_t, bool>> predicted_conflicts;

    for (std::size_t index = 0; index < source.records.size(); ++index) {
        const SyncTraceEvent& event = source.records[index].event;
        const auto event_index = static_cast<std::uint32_t>(index);
        if (event.client_network_id == invalid_client_entity_network_id &&
            !event.server_entity && !event.local_entity && event.wire_network_id == 0U) {
            if (event.type == SyncTraceEventType::ComponentName && event.component && !event.data.empty()) {
                source.component_names[event.component.value] = event.data;
            }
            if (event.type == SyncTraceEventType::CueName && !event.data.empty()) {
                source.cue_names[event.cue_type] = event.data;
            }
            continue;
        }
        if (event.type == SyncTraceEventType::ComponentName) {
            if (event.component && !event.data.empty()) {
                source.component_names[event.component.value] = event.data;
            }
            continue;
        }
        if (event.type == SyncTraceEventType::CueName) {
            if (!event.data.empty()) {
                source.cue_names[event.cue_type] = event.data;
            }
            continue;
        }
        KTraceEntityRow& entity = entity_row(source.entities, event);
        const ClientEntityNetworkId key = entity.client_network_id;
        if (event.type == SyncTraceEventType::ModeChanged) {
            modes[key] = event.mode;
            continue;
        }
        if (event.type == SyncTraceEventType::EntityStartedSyncing ||
            event.type == SyncTraceEventType::EntityReceived) {
            continue;
        }
        if (event.type == SyncTraceEventType::EntityDestroyed) {
            KTraceComponentRow& row = component_row(entity.components, ecs::Entity{});
            add_cell_state(row, event.frame, KTraceCellState::EntityDestroyed, event_index);
            continue;
        }
        switch (event.type) {
        case SyncTraceEventType::ComponentSent:
            if (!event.component) {
                break;
            }
            {
            KTraceComponentRow& row = component_row(entity.components, event.component);
            add_cell_state(row, event.frame, KTraceCellState::SentToClient, event_index);
            }
            break;
        case SyncTraceEventType::ComponentReceived:
            if (!event.component) {
                break;
            }
            {
            KTraceComponentRow& row = component_row(entity.components, event.component);
            add_cell_state(row, event.frame, KTraceCellState::ReceivedFromServer, event_index);
            }
            break;
        case SyncTraceEventType::ComponentApplied:
            if (!event.component) {
                break;
            }
            {
            KTraceComponentRow& row = component_row(entity.components, event.component);
            add_cell_state(row, event.frame, KTraceCellState::Applied, event_index);
            }
            break;
        case SyncTraceEventType::ComponentRemoved:
            if (!event.component) {
                break;
            }
            {
            KTraceComponentRow& row = component_row(entity.components, event.component);
            add_cell_state(row, event.frame, KTraceCellState::Removed, event_index);
            }
            break;
        case SyncTraceEventType::BufferedStarved:
        case SyncTraceEventType::InputStarved:
            {
            KTraceComponentRow& row = component_row(entity.components, event.component);
            add_cell_state(row, event.frame, KTraceCellState::Starved, event_index);
            }
            break;
        case SyncTraceEventType::PredictionRollbackConflict: {
            if (!event.component) {
                break;
            }
            KTraceComponentRow& row = component_row(entity.components, event.component);
            add_cell_state(row, event.frame, KTraceCellState::Mispredicted, event_index);
            row.pending_run_split = true;
            row.pending_split_frame = event.frame;
            predicted_conflicts[key][(std::uint64_t{event.component.value} << 32U) | event.frame] = true;
            break;
        }
        case SyncTraceEventType::RollbackReason: {
            if (!event.component) {
                break;
            }
            KTraceComponentRow& row = component_row(entity.components, event.component);
            add_cell_event(row, event.frame, event_index);
            break;
        }
        case SyncTraceEventType::ClockSkew: {
            KTraceComponentRow& row = component_row(entity.components, event.component);
            add_cell_event(row, event.frame, event_index);
            break;
        }
        case SyncTraceEventType::CueEmitted: {
            KTraceComponentRow& row = component_row(entity.components, cue_row_component());
            add_cell_state(row, event.frame, KTraceCellState::CueEmitted, event_index);
            break;
        }
        case SyncTraceEventType::CueSent: {
            KTraceComponentRow& row = component_row(entity.components, cue_row_component());
            add_cell_state(row, event.frame, KTraceCellState::CueSent, event_index);
            break;
        }
        case SyncTraceEventType::CueReceived: {
            KTraceComponentRow& row = component_row(entity.components, cue_row_component());
            add_cell_state(row, event.frame, KTraceCellState::CueReceived, event_index);
            break;
        }
        case SyncTraceEventType::CuePlayed: {
            KTraceComponentRow& row = component_row(entity.components, cue_row_component());
            add_cell_state(row, event.frame, KTraceCellState::CuePlayed, event_index);
            break;
        }
        case SyncTraceEventType::CueConfirmed: {
            KTraceComponentRow& row = component_row(entity.components, cue_row_component());
            add_cell_state(row, event.frame, KTraceCellState::CueConfirmed, event_index);
            break;
        }
        case SyncTraceEventType::CueRolledBack: {
            KTraceComponentRow& row = component_row(entity.components, cue_row_component());
            add_cell_state(row, event.frame, KTraceCellState::CueRolledBack, event_index);
            break;
        }
        case SyncTraceEventType::ResimulatedFrameComponent: {
            if (!event.component) {
                break;
            }
            KTraceComponentRow& row = component_row(entity.components, event.component);
            KTraceFrameRun& run = ensure_resimulation_run(row, event.frame, source);
            add_cell_state(run, event.frame, KTraceCellState::Resimulated, event_index);
            break;
        }
        case SyncTraceEventType::FrameComponent: {
            if (!event.component) {
                break;
            }
            KTraceComponentRow& row = component_row(entity.components, event.component);
            const ReplicationClientMode mode = modes.count(key) != 0U ? modes[key] : event.mode;
            if (source.role == SyncTraceRole::Server && event.client != invalid_client_id) {
                add_cell_state(row, event.frame, KTraceCellState::InputReceived, event_index);
            } else if (source.role == SyncTraceRole::Client && mode == ReplicationClientMode::BufferedInterpolation) {
                add_cell_state(row, event.frame, KTraceCellState::LocalInterpolated, event_index);
            } else if (source.role == SyncTraceRole::Client && mode == ReplicationClientMode::Predict) {
                add_cell_state(row, event.frame, KTraceCellState::LocalPredicted, event_index);
            }
            break;
        }
        default:
            break;
        }
    }

    for (std::size_t index = 0; index < source.records.size(); ++index) {
        const SyncTraceEvent& event = source.records[index].event;
        if (event.type != SyncTraceEventType::ComponentReceived || !event.component) {
            continue;
        }
        const ClientEntityNetworkId key = event_network_key(event);
        const std::uint64_t conflict_key = (std::uint64_t{event.component.value} << 32U) | event.frame;
        if (predicted_conflicts[key].count(conflict_key) != 0U) {
            continue;
        }
        auto found_entity = std::find_if(source.entities.begin(), source.entities.end(), [key](const KTraceEntityRow& row) {
            return row.client_network_id == key;
        });
        if (found_entity == source.entities.end()) {
            continue;
        }
        KTraceComponentRow& row = component_row(found_entity->components, event.component);
        KTraceFrameRun* run = run_containing_event(row, event.frame, static_cast<std::uint32_t>(index));
        if (run != nullptr) {
            add_cell_state(*run, event.frame, KTraceCellState::PredictedCorrect, static_cast<std::uint32_t>(index));
        }
    }

    return source;
}

SyncTraceHistory KTraceReader::read_directory(const std::string& directory) const {
    SyncTraceHistory history;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".ktrace") {
            continue;
        }
        history.sources.push_back(build_source_history(read_file(entry.path().string())));
    }

    std::unordered_map<ClientEntityNetworkId, ecs::Entity> server_entities_by_network_id;
    for (const KTraceSourceHistory& source : history.sources) {
        if (source.role != SyncTraceRole::Server) {
            continue;
        }
        for (const KTraceRecord& record : source.records) {
            const SyncTraceEvent& event = record.event;
            if (event.type == SyncTraceEventType::EntityStartedSyncing &&
                event.client_network_id != invalid_client_entity_network_id &&
                event.server_entity) {
                server_entities_by_network_id[event.client_network_id] = event.server_entity;
            }
        }
    }
    if (!server_entities_by_network_id.empty()) {
        for (KTraceSourceHistory& source : history.sources) {
            if (source.role != SyncTraceRole::Client) {
                continue;
            }
            for (KTraceEntityRow& entity : source.entities) {
                const auto found = server_entities_by_network_id.find(entity.client_network_id);
                if (found != server_entities_by_network_id.end()) {
                    entity.server_entity = found->second;
                }
            }
        }
    }
    return history;
}

std::unique_ptr<KTraceDirectoryWriter> make_trace_writer(const TraceOptions& options) {
    if (!options.enabled) {
        return nullptr;
    }
    KTraceDirectoryWriterOptions writer_options;
    writer_options.directory = options.directory;
    writer_options.queue_capacity_bytes = options.queue_capacity_bytes;
    writer_options.flush_threshold_bytes = options.flush_threshold_bytes;
    writer_options.truncate_existing = options.truncate_existing;
    auto writer = std::make_unique<KTraceDirectoryWriter>(std::move(writer_options));
    writer->tracer().set_frame_data_enabled(options.frame_data);
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    writer->tracer().set_packet_logs_enabled(options.packet_logs);
#endif
    return writer;
}

}  // namespace kage::sync

#endif
