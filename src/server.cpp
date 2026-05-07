#include "kage/sync/server.hpp"

#include "detail/frame_data.hpp"
#include "detail/options_validation.hpp"
#include "server/detail.hpp"
#include "server/packet.hpp"
#include "server/state.hpp"

#include "kage/sync/protocol.hpp"
#include "kage/sync/tracing.hpp"

#include <spdlog/logger.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kage::sync {
namespace {

constexpr std::size_t max_pending_quantized_frames_per_entity = 64;
constexpr std::size_t max_cues_per_entity_record = server_detail::max_cues_per_entity_record;

using detail::frame_component_data;
using detail::frame_has_component;
using detail::has_tag_slot;
using detail::init_frame_data;
using detail::mutable_frame_component_data;
using detail::sync_slot_bit;
using detail::sync_slot_count;
using server_detail::boosted_candidate_priority;
using server_detail::configured_packet_id_bits;
using server_detail::destroy_record_bits;
using server_detail::make_server_packet;
using server_detail::server_update_header_bits;

struct OutboundPacket {
    ClientId client = invalid_client_id;
    ecs::BitBuffer packet;
};

void append_json_string(spdlog::memory_buf_t& dest, std::string_view value) {
    for (const char ch : value) {
        switch (ch) {
        case '"':
            fmt::format_to(std::back_inserter(dest), "\\\"");
            break;
        case '\\':
            fmt::format_to(std::back_inserter(dest), "\\\\");
            break;
        case '\b':
            fmt::format_to(std::back_inserter(dest), "\\b");
            break;
        case '\f':
            fmt::format_to(std::back_inserter(dest), "\\f");
            break;
        case '\n':
            fmt::format_to(std::back_inserter(dest), "\\n");
            break;
        case '\r':
            fmt::format_to(std::back_inserter(dest), "\\r");
            break;
        case '\t':
            fmt::format_to(std::back_inserter(dest), "\\t");
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20U) {
                fmt::format_to(std::back_inserter(dest), "\\u{:04x}", static_cast<unsigned int>(ch));
            } else {
                dest.push_back(ch);
            }
            break;
        }
    }
}

bool json_value_is_number(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    std::size_t index = 0;
    if (value[index] == '-') {
        ++index;
    }
    bool saw_digit = false;
    for (; index < value.size(); ++index) {
        const char ch = value[index];
        if (ch >= '0' && ch <= '9') {
            saw_digit = true;
            continue;
        }
        if (ch == '.') {
            continue;
        }
        return false;
    }
    return saw_digit;
}

void append_json_field_value(spdlog::memory_buf_t& dest, std::string_view value) {
    if (value == "true" || value == "false" || json_value_is_number(value)) {
        fmt::format_to(std::back_inserter(dest), "{}", value);
        return;
    }
    dest.push_back('"');
    append_json_string(dest, value);
    dest.push_back('"');
}

void append_payload_fields(spdlog::memory_buf_t& dest, std::string_view payload) {
    std::size_t start = 0;
    while (start < payload.size()) {
        while (start < payload.size() && payload[start] == ' ') {
            ++start;
        }
        const std::size_t end = payload.find(' ', start);
        const std::string_view token = payload.substr(
            start,
            end == std::string_view::npos ? std::string_view::npos : end - start);
        const std::size_t equals = token.find('=');
        if (equals != std::string_view::npos && equals != 0U && equals + 1U < token.size()) {
            fmt::format_to(std::back_inserter(dest), ",\"");
            append_json_string(dest, token.substr(0, equals));
            fmt::format_to(std::back_inserter(dest), "\":");
            append_json_field_value(dest, token.substr(equals + 1U));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
    }
}

std::string log_token(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        result.push_back(ch == ' ' ? '_' : ch);
    }
    return result;
}

class JsonLogFormatter final : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
        const auto level = spdlog::level::to_string_view(msg.level);
        fmt::format_to(std::back_inserter(dest), "{{\"level\":\"");
        append_json_string(dest, std::string_view(level.data(), level.size()));
        fmt::format_to(std::back_inserter(dest), "\",\"logger\":\"");
        append_json_string(dest, std::string_view(msg.logger_name.data(), msg.logger_name.size()));
        dest.push_back('"');
        append_payload_fields(dest, std::string_view(msg.payload.data(), msg.payload.size()));
        fmt::format_to(std::back_inserter(dest), "}}\n");
    }

    std::unique_ptr<spdlog::formatter> clone() const override {
        return std::make_unique<JsonLogFormatter>();
    }
};

spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return spdlog::level::trace;
    case LogLevel::Debug:
        return spdlog::level::debug;
    case LogLevel::Info:
        return spdlog::level::info;
    case LogLevel::Warning:
        return spdlog::level::warn;
    case LogLevel::Error:
        return spdlog::level::err;
    case LogLevel::Critical:
        return spdlog::level::critical;
    case LogLevel::Off:
        return spdlog::level::off;
    }
    return spdlog::level::off;
}

std::shared_ptr<spdlog::logger> make_server_logger(const LoggingOptions& options) {
    if (options.logger != nullptr) {
        options.logger->set_level(to_spdlog_level(options.level));
        return options.logger;
    }
    if (options.level == LogLevel::Off) {
        return nullptr;
    }

    auto logger = std::make_shared<spdlog::logger>(
        "kage.sync.server",
        std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    logger->set_level(to_spdlog_level(options.level));
    if (options.format == LogFormat::Json) {
        logger->set_formatter(std::make_unique<JsonLogFormatter>());
    } else {
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
    }
    return logger;
}

#ifdef KAGE_SYNC_ENABLE_TRACING
using server_detail::make_server_trace_event;

void append_trace_component_data(
    const SyncTracer* tracer,
    const SyncArchetype& archetype,
    std::size_t component_index,
    const std::uint8_t* bytes,
    SyncTraceEvent& event) {
    if (component_index < archetype.component_ops.size()) {
        event.component_name = archetype.component_ops[component_index].name;
    }
#ifdef KAGE_SYNC_TRACE_COMPONENT_DATA
    if (tracer == nullptr || !tracer->frame_data_enabled() || bytes == nullptr ||
        component_index >= archetype.component_ops.size()) {
        return;
    }
    const SyncComponentOps& ops = archetype.component_ops[component_index];
    if (ops.trace == nullptr) {
        return;
    }
    SyncTraceStringBuilder builder;
    ops.trace(bytes, builder);
    event.data = std::move(builder.value);
#else
    (void)tracer;
    (void)archetype;
    (void)component_index;
    (void)bytes;
    (void)event;
#endif
}

void append_trace_input_component_data(
    const SyncTracer* tracer,
    const SyncComponentOps& ops,
    const std::uint8_t* bytes,
    SyncTraceEvent& event) {
    event.component_name = ops.name;
#ifdef KAGE_SYNC_TRACE_COMPONENT_DATA
    if (tracer == nullptr || !tracer->frame_data_enabled() || bytes == nullptr || ops.trace == nullptr) {
        return;
    }
    SyncTraceStringBuilder builder;
    ops.trace(bytes, builder);
    event.data = std::move(builder.value);
#else
    (void)tracer;
    (void)ops;
    (void)bytes;
    (void)event;
#endif
}

void append_trace_cue_data(
    const SyncTracer* tracer,
    const SyncSettings& settings,
    SyncCueTypeId cue_type,
    const ecs::BitBuffer& payload,
    SyncTraceEvent& event) {
#ifdef KAGE_SYNC_TRACE_COMPONENT_DATA
    if (tracer == nullptr || !tracer->frame_data_enabled() ||
        cue_type >= settings.cue_ops.size() || settings.cue_ops[cue_type].trace == nullptr) {
        return;
    }
    SyncTraceStringBuilder builder;
    if (settings.cue_ops[cue_type].trace(payload, builder)) {
        event.data = std::move(builder.value);
    }
#else
    (void)tracer;
    (void)settings;
    (void)cue_type;
    (void)payload;
    (void)event;
#endif
}

void append_trace_data_field(SyncTraceEvent& event, const char* key, const char* value) {
    if (key == nullptr || value == nullptr || value[0] == '\0') {
        return;
    }
    if (!event.data.empty()) {
        event.data += ",";
    }
    event.data += key;
    event.data += "=";
    event.data += value;
}

void append_trace_cue_name(const SyncSettings& settings, SyncCueTypeId cue_type, SyncTraceEvent& event) {
    if (cue_type < settings.cue_ops.size()) {
        event.component_name = settings.cue_ops[cue_type].name;
    }
}

#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
std::string packet_ack_list(const std::vector<std::uint32_t>& acks) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < acks.size(); ++index) {
        if (index != 0U) {
            out << ",";
        }
        out << acks[index];
    }
    out << "]";
    return out.str();
}
#endif

#endif

std::uint64_t visible_tag_mask(
    const ecs::Registry& registry,
    const SyncArchetype& archetype,
    ecs::Entity entity,
    ClientId client) {
    std::uint64_t mask = 0;
    const NetworkOwner* owner = registry.try_get<NetworkOwner>(entity);
    for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
        const SyncTagReplication& replication = archetype.tags[tag_index];
        if (replication.audience == ReplicationAudience::Owner &&
            (owner == nullptr || owner->client != client)) {
            continue;
        }
        if (registry.has(entity, replication.tag)) {
            mask |= std::uint64_t{1} << tag_index;
        }
    }
    return mask;
}

}  // namespace

ReplicationServer::ReplicationServer(ReplicationServerOptions options)
    : options_(detail::validate_server_options(std::move(options))),
      frame_consumers_(std::make_shared<ServerFrameConsumerSubscription::State>()),
      logger_(make_server_logger(options_.logging)) {
#ifdef KAGE_SYNC_ENABLE_TRACING
    set_trace_options(options_.trace);
#endif
}

ReplicationServer::~ReplicationServer() = default;

ReplicationServer::ReplicationServer(ReplicationServer&& other) noexcept = default;

ReplicationServer& ReplicationServer::operator=(ReplicationServer&& other) noexcept = default;

void ReplicationServer::set_transport(TransportFn transport) {
    options_.transport = std::move(transport);
}

void ReplicationServer::set_logger(std::shared_ptr<spdlog::logger> logger) {
    options_.logging.logger = std::move(logger);
    logger_ = make_server_logger(options_.logging);
}

void ReplicationServer::set_log_level(LogLevel level) {
    options_.logging.level = level;
    if (logger_ == nullptr) {
        logger_ = make_server_logger(options_.logging);
    } else {
        logger_->set_level(to_spdlog_level(level));
    }
}

ServerFrameConsumerSubscription::ServerFrameConsumerSubscription(std::shared_ptr<State> state, std::uint64_t id)
    : state_(std::move(state)),
      id_(id) {}

ServerFrameConsumerSubscription::ServerFrameConsumerSubscription(ServerFrameConsumerSubscription&& other) noexcept
    : state_(std::move(other.state_)),
      id_(other.id_) {
    other.id_ = 0;
}

ServerFrameConsumerSubscription& ServerFrameConsumerSubscription::operator=(
    ServerFrameConsumerSubscription&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        id_ = other.id_;
        other.id_ = 0;
    }
    return *this;
}

ServerFrameConsumerSubscription::~ServerFrameConsumerSubscription() {
    reset();
}

void ServerFrameConsumerSubscription::reset() {
    if (id_ == 0) {
        return;
    }
    if (auto state = state_.lock()) {
        for (Entry& entry : state->consumers) {
            if (entry.id == id_) {
                entry.consumer = nullptr;
                break;
            }
        }
    }
    state_.reset();
    id_ = 0;
}

bool ServerFrameConsumerSubscription::active() const noexcept {
    return id_ != 0 && !state_.expired();
}

ServerFrameConsumerSubscription ReplicationServer::subscribe_frame_consumer(ServerFrameConsumer& consumer) {
    const std::uint64_t id = frame_consumers_->next_id++;
    frame_consumers_->consumers.push_back(ServerFrameConsumerSubscription::Entry{id, &consumer});
    return ServerFrameConsumerSubscription(frame_consumers_, id);
}

#ifdef KAGE_SYNC_ENABLE_TRACING
void ReplicationServer::set_tracer(SyncTracer* tracer) noexcept {
    trace_writer_.reset();
    tracer_ = tracer;
}

void ReplicationServer::set_trace_options(TraceOptions options) {
    options_.trace = std::move(options);
    trace_writer_ = make_trace_writer(options_.trace);
    tracer_ = trace_writer_ != nullptr ? &trace_writer_->tracer() : nullptr;
}

void ReplicationServer::flush_trace() {
    if (trace_writer_ != nullptr) {
        trace_writer_->flush();
    }
}

void ReplicationServer::close_trace() {
    if (trace_writer_ != nullptr) {
        trace_writer_->close();
    }
}
#endif

bool ReplicationServer::add_client(ClientId client) {
    return add_client_for_peer(client, client, true);
}

bool ReplicationServer::add_client_state(ClientState state) {
    if (state.id == invalid_client_id ||
        state.id > max_client_entity_network_id_client ||
        client_to_index_.find(state.id) != client_to_index_.end()) {
        return false;
    }
    if (!state.local && (state.peer == invalid_client_id || peer_to_index_.find(state.peer) != peer_to_index_.end())) {
        return false;
    }

    if (!state.local && state.ready_for_updates) {
        create_client_replicator(state);
    }

    const ClientId client = state.id;
    const ClientId peer = state.peer;
    const bool local = state.local;
    const bool ready_for_updates = state.ready_for_updates;
    const std::size_t index = clients_.size();
    clients_.push_back(std::move(state));
    client_to_index_[client] = index;
    if (!local) {
        peer_to_index_[peer] = index;
    }
    next_connect_client_id_ = std::max(next_connect_client_id_, client + 1U);
    if (!local) {
        ++observability_stats_.client_connects_accepted;
        log_info("client_connected", "peer=" + std::to_string(peer) +
            " client=" + std::to_string(client) +
            " ready=" + (ready_for_updates ? std::string("true") : std::string("false")));
    }
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (tracer_ != nullptr && tracer_->enabled()) {
        SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::ClientConnected, client, frame_);
        tracer_->trace(event);
    }
#endif
    return true;
}

void ReplicationServer::create_client_replicator(ClientState& client) {
    if (client.local || client.replication != nullptr) {
        return;
    }
    client.replication = std::make_unique<ServerClientReplicator>();
    client.replication->id = client.id;
    client.replication->peer = client.peer;
    client.replication->bandwidth = server_detail::BandwidthController(options_.bandwidth);
    client.replication->initialize_marking_all_dirty(*this, frame_);
    client.replication->frame_subscription = subscribe_frame_consumer(*client.replication);
}

bool ReplicationServer::add_client_for_peer(ClientId peer, ClientId client, bool ready_for_updates) {
    if (client == invalid_client_id ||
        client > max_client_entity_network_id_client ||
        peer == invalid_client_id ||
        client_to_index_.find(client) != client_to_index_.end() ||
        peer_to_index_.find(peer) != peer_to_index_.end()) {
        return false;
    }

    ClientState state;
    state.id = client;
    state.peer = peer;
    state.ready_for_updates = ready_for_updates;
    state.connect_resend_accumulator_seconds = options_.connect_resend_interval_seconds;
    return add_client_state(std::move(state));
}

ClientId ReplicationServer::add_local_client() {
    if (local_client_ != invalid_client_id) {
        return invalid_client_id;
    }
    while (next_connect_client_id_ != invalid_client_id &&
           next_connect_client_id_ <= max_client_entity_network_id_client &&
           client_to_index_.find(next_connect_client_id_) != client_to_index_.end()) {
        ++next_connect_client_id_;
    }
    if (next_connect_client_id_ == invalid_client_id ||
        next_connect_client_id_ > max_client_entity_network_id_client) {
        return invalid_client_id;
    }

    ClientState state;
    state.id = next_connect_client_id_;
    state.peer = invalid_client_id;
    state.local = true;
    state.ready_for_updates = true;
    state.connect_resend_accumulator_seconds = options_.connect_resend_interval_seconds;
    if (!add_client_state(std::move(state))) {
        return invalid_client_id;
    }
    local_client_ = next_connect_client_id_ - 1U;
    return local_client_;
}

bool ReplicationServer::is_local_client(ClientId client) const noexcept {
    const auto found = client_to_index_.find(client);
    return found != client_to_index_.end() && clients_[found->second].local;
}

#ifdef KAGE_SYNC_ENABLE_TRACING
void ReplicationServer::trace_frame_components(const ecs::Registry& registry, const SyncSettings& settings) {
    if (tracer_ == nullptr || !tracer_->enabled() || !tracer_->frame_data_enabled()) {
        return;
    }
    for (const ReplicatedSlot& slot : replicated_) {
        if (!slot.active || slot.archetype.value >= settings.archetypes.size()) {
            continue;
        }
        const SyncArchetype& archetype = settings.archetypes[slot.archetype.value];
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            const ComponentReplication& replication = archetype.components[component_index];
            const void* value = registry.get(slot.entity, replication.component);
            if (value == nullptr || component_index >= archetype.component_ops.size()) {
                continue;
            }
            const SyncComponentOps& ops = archetype.component_ops[component_index];
            if (ops.quantize == nullptr) {
                continue;
            }
            std::vector<std::uint8_t> bytes(ops.quantized_size);
            ops.quantize(value, bytes.data());
            SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::FrameComponent, invalid_client_id, frame_);
            event.server_entity = slot.entity;
            event.archetype = slot.archetype;
            event.component = replication.component;
            append_trace_component_data(tracer_, archetype, component_index, bytes.data(), event);
            tracer_->trace(event);
        }
    }
}

void ReplicationServer::trace_input_component(
    ecs::Entity entity,
    ClientState& client,
    SyncFrame frame,
    ecs::Entity component,
    const SyncComponentOps& ops,
    const std::uint8_t* quantized) {
    if (tracer_ == nullptr || !tracer_->enabled() || quantized == nullptr || !component) {
        return;
    }
    SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::FrameComponent, client.id, frame);
    event.server_entity = entity;
    event.component = component;
    const auto found_replicated = entity_to_replicated_index_.find(entity.value);
    if (client.replication != nullptr &&
        found_replicated != entity_to_replicated_index_.end()) {
        ServerClientReplicator& replication = *client.replication;
        (void)replication.network_id_for(*this, found_replicated->second);
        const ClientEntityState* state = replication.entities.try_get(found_replicated->second);
        if (state != nullptr && state->has_network_id) {
            event.wire_network_id = state->network_id;
            event.network_version = state->network_version;
            event.client_network_id = make_client_entity_network_id(client.id, state->network_id, state->network_version);
            if (found_replicated->second < replicated_.size()) {
                event.archetype = replicated_[found_replicated->second].archetype;
            }
        }
    }
    append_trace_input_component_data(tracer_, ops, quantized, event);
    tracer_->trace(event);
}

void ReplicationServer::trace_input_starved(
    ecs::Entity entity,
    ClientState& client,
    SyncFrame frame_for_input,
    SyncFrame frame_from_input,
    ecs::Entity component,
    const SyncComponentOps& ops) {
    if (tracer_ == nullptr || !tracer_->enabled() || !component) {
        return;
    }
    SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::InputStarved, client.id, frame_for_input);
    event.server_entity = entity;
    event.component = component;
    event.component_name = ops.name;
    const auto found_replicated = entity_to_replicated_index_.find(entity.value);
    if (client.replication != nullptr &&
        found_replicated != entity_to_replicated_index_.end()) {
        ServerClientReplicator& replication = *client.replication;
        (void)replication.network_id_for(*this, found_replicated->second);
        const ClientEntityState* state = replication.entities.try_get(found_replicated->second);
        if (state != nullptr && state->has_network_id) {
            event.wire_network_id = state->network_id;
            event.network_version = state->network_version;
            event.client_network_id = make_client_entity_network_id(client.id, state->network_id, state->network_version);
            if (found_replicated->second < replicated_.size()) {
                event.archetype = replicated_[found_replicated->second].archetype;
            }
        }
    }
    event.data = "input_frame=" + std::to_string(frame_from_input);
    tracer_->trace(event);
}

#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
void ReplicationServer::trace_incoming_ack_packet(ServerClientReplicator& client, const std::vector<std::uint32_t>& acks) const {
    if (tracer_ == nullptr || !tracer_->enabled() || !tracer_->packet_logs_enabled()) {
        return;
    }
    SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::PacketLog, client.id, frame_);
    event.data = "direction=in,message=client_ack,client=" + std::to_string(client.id) +
        ",acks=" + packet_ack_list(acks);
    tracer_->trace(event);
}

void ReplicationServer::trace_incoming_input_packet(
    ClientState& client,
    const std::vector<std::uint32_t>& acks,
    SyncFrame baseline_frame,
    SyncFrame first_input_frame,
    SyncFrame last_input_frame) const {
    if (tracer_ == nullptr || !tracer_->enabled() || !tracer_->packet_logs_enabled()) {
        return;
    }
    SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::PacketLog, client.id, frame_);
    std::ostringstream out;
    out << "direction=in,message=client_input,client=" << client.id
        << ",acks=" << packet_ack_list(acks)
        << ",input_frames=";
    if (first_input_frame != 0U && last_input_frame >= first_input_frame) {
        out << first_input_frame << "-" << last_input_frame;
    } else {
        out << "none";
    }
    out << ",baseline=" << baseline_frame;
    event.data = out.str();
    tracer_->trace(event);
}

void ReplicationServer::trace_outgoing_update_packet(
    ServerClientReplicator& client,
    SyncFrame frame,
    std::uint32_t packet_id,
    SyncFrame input_ack_frame,
    const std::vector<PacketAckRecord>& records) const {
    if (tracer_ == nullptr || !tracer_->enabled() || !tracer_->packet_logs_enabled()) {
        return;
    }
    SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::PacketLog, client.id, frame);
    std::ostringstream out;
    out << "direction=out,message=server_update,client=" << client.id
        << ",sequence=" << packet_id
        << ",server_frame=" << frame
        << ",input_ack=" << input_ack_frame
        << ",updated_server_entities=[";
    bool first = true;
    for (const PacketAckRecord& record : records) {
        if (record.destroy) {
            continue;
        }
        if (!first) {
            out << ",";
        }
        first = false;
        out << record.entity.value;
    }
    out << "]";
    out << ",cues=[";
    bool first_cue = true;
    for (const PacketAckRecord& record : records) {
        if (record.destroy) {
            continue;
        }
        for (const PacketAckRecord::CueSummary& cue : record.cues) {
            if (!first_cue) {
                out << ";";
            }
            first_cue = false;
            out << "{entity=" << record.entity.value
                << ",frame=" << cue.frame
                << ",type=" << cue.type;
#ifdef KAGE_SYNC_TRACE_COMPONENT_DATA
            if (tracer_ != nullptr && tracer_->frame_data_enabled() && !cue.data.empty()) {
                out << ",data=" << cue.data;
            }
#endif
            out << "}";
        }
    }
    out << "]";
    event.data = out.str();
    tracer_->trace(event);
}

void ReplicationServer::append_packet_ack_cues(
    const SyncSettings& settings,
    const ClientEntityState& state,
    PacketAckRecord& record) const {
    const std::size_t cue_count = std::min(state.pending_cues.size(), max_cues_per_entity_record);
    record.cues.reserve(cue_count);
    for (std::size_t index = 0; index < cue_count; ++index) {
        const ClientEntityState::PendingCue& cue = state.pending_cues[index];
        PacketAckRecord::CueSummary summary;
        summary.frame = cue.frame;
        summary.type = cue.type;
#ifdef KAGE_SYNC_TRACE_COMPONENT_DATA
        if (tracer_ != nullptr && tracer_->frame_data_enabled() &&
            cue.type < settings.cue_ops.size() && settings.cue_ops[cue.type].trace != nullptr) {
            SyncTraceStringBuilder builder;
            if (settings.cue_ops[cue.type].trace(cue.payload, builder)) {
                summary.data = std::move(builder.value);
            }
        }
#else
        (void)settings;
#endif
        record.cues.push_back(std::move(summary));
    }
}
#endif
#endif

bool ReplicationServer::remove_client(ClientId client) {
    const auto found = client_to_index_.find(client);
    if (found == client_to_index_.end()) {
        return false;
    }

    const std::size_t index = found->second;
    const ClientId removed_peer = clients_[index].peer;
    const ClientId removed_id = clients_[index].id;
    const bool removed_local = clients_[index].local;
#ifdef KAGE_SYNC_ENABLE_TRACING
    const ClientId removed_client = clients_[index].id;
#endif
    if (clients_[index].replication != nullptr) {
        clients_[index].replication->entities.clear_all(*this);
    }

    const std::size_t last = clients_.size() - 1;
    if (index != last) {
        clients_[index] = std::move(clients_[last]);
        client_to_index_[clients_[index].id] = index;
        if (!clients_[index].local) {
            peer_to_index_[clients_[index].peer] = index;
        }
    }

    clients_.pop_back();
    client_to_index_.erase(found);
    if (!removed_local) {
        peer_to_index_.erase(removed_peer);
        ++observability_stats_.clients_removed;
        log_info("client_removed", "peer=" + std::to_string(removed_peer) +
            " client=" + std::to_string(removed_id));
    } else {
        local_client_ = invalid_client_id;
    }
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (tracer_ != nullptr && tracer_->enabled()) {
        SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::ClientDisconnected, removed_client, frame_);
        tracer_->trace(event);
    }
#endif
    return true;
}

bool ReplicationServer::has_client(ClientId client) const {
    return client_to_index_.find(client) != client_to_index_.end();
}

std::size_t ReplicationServer::client_count() const noexcept {
    return clients_.size();
}

ReplicationServer::ClientInputStats ReplicationServer::input_stats(ClientId client) const noexcept {
    const auto found = client_to_index_.find(client);
    if (found == client_to_index_.end()) {
        return ClientInputStats{};
    }
    return clients_[found->second].input.stats();
}

ReplicationServer::ClientBandwidthStats ReplicationServer::bandwidth_stats(ClientId client) const noexcept {
    const auto found = client_to_index_.find(client);
    if (found == client_to_index_.end()) {
        return ClientBandwidthStats{};
    }
    const ClientState& state = clients_[found->second];
    if (!options_.bandwidth.enabled) {
        return ClientBandwidthStats{
            false,
            static_cast<double>(options_.bandwidth_limit_bytes_per_tick) / options_.fixed_dt_seconds,
            static_cast<double>(options_.bandwidth_limit_bytes_per_tick),
            0,
            0,
            0.0f};
    }
    if (state.replication == nullptr) {
        return ClientBandwidthStats{};
    }
    const ServerClientReplicator& replication = *state.replication;
    return ClientBandwidthStats{
        true,
        replication.bandwidth.target_bytes_per_second(),
        replication.bandwidth.available_bytes(),
        replication.bandwidth.in_flight_bytes(),
        replication.bandwidth.delivered_bytes(),
        replication.bandwidth.loss_rate()};
}

void ReplicationServer::rediscover_all_replicated_entities(ecs::Registry& registry) {
    if (!replicated_initialized_) {
        registry.view<const Replicated>().each([&](ecs::Entity entity, const Replicated& replicated) {
            upsert_replicated(registry, entity, replicated.archetype);
        });
        replicated_initialized_ = true;
        return;
    }

    for (std::uint32_t slot = 0; slot < replicated_.size(); ++slot) {
        if (replicated_[slot].active && !replicated_is_replicable(registry, slot)) {
            deactivate_replicated(slot);
        }
    }
    registry.view<const Replicated>().each([&](ecs::Entity entity, const Replicated& replicated) {
        upsert_replicated(registry, entity, replicated.archetype);
    });
}

void ReplicationServer::rediscover_replicated_entities(ecs::Registry& registry, ecs::Registry::DirtyView dirty) {
    if (!replicated_initialized_) {
        registry.view<const Replicated>().each([&](ecs::Entity entity, const Replicated& replicated) {
            upsert_replicated(registry, entity, replicated.archetype);
        });
        replicated_initialized_ = true;
        return;
    }

    dirty.each_removed<Replicated>([&](ecs::Registry::ComponentRemoval removal) {
        deactivate_entity_index(removal.entity_index);
    });

    dirty.each_dirty<Replicated>([&](ecs::Entity entity, const void* value) {
        upsert_replicated(registry, entity, static_cast<const Replicated*>(value)->archetype);
    });
}

bool ReplicationServer::is_replicated(ecs::Entity entity) const {
    return entity_to_replicated_index_.find(entity.value) != entity_to_replicated_index_.end();
}

std::size_t ReplicationServer::replicated_count() const noexcept
{
    return active_replicated_count_;
}

std::size_t ReplicationServer::replicated_slot_count() const noexcept {
    return replicated_.size();
}

std::size_t ReplicationServer::active_replicated_slot_count() const noexcept {
    return active_replicated_count_;
}

bool ReplicationServer::replicated_slot_active(std::uint32_t slot) const noexcept {
    return slot < replicated_.size() && replicated_[slot].active;
}

ecs::Entity ReplicationServer::replicated_slot_entity(std::uint32_t slot) const noexcept {
    return slot < replicated_.size() ? replicated_[slot].entity : ecs::Entity{};
}

SyncArchetypeId ReplicationServer::replicated_slot_archetype(std::uint32_t slot) const noexcept {
    return slot < replicated_.size() ? replicated_[slot].archetype : SyncArchetypeId{};
}

bool ReplicationServer::replicated_slot_is_replicable(const ecs::Registry& registry, std::uint32_t slot) const {
    return replicated_is_replicable(registry, slot);
}

void ReplicationServer::deactivate_replicated_slot(std::uint32_t slot) {
    deactivate_replicated(slot);
}

std::uint32_t ReplicationServer::replicated_slot_for_entity(ecs::Entity entity) const noexcept {
    const auto found = entity_to_replicated_index_.find(entity.value);
    return found != entity_to_replicated_index_.end() ? found->second : invalid_quantized_frame_id;
}

std::uint32_t ReplicationServer::replicated_slot_for_entity_index(std::uint32_t entity_index) const noexcept {
    const auto found = entity_index_to_replicated_index_.find(entity_index);
    return found != entity_index_to_replicated_index_.end() ? found->second : invalid_quantized_frame_id;
}

std::uint32_t ReplicationServer::quantized_frame_for_client(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    const server_detail::ServerClientReplicator& client,
    std::uint32_t slot,
    SyncFrame frame,
    QuantizedFrameData& scratch,
    std::vector<std::uint64_t>& scratch_dirty_generations) {
    return find_or_create_quantized_frame(
        registry,
        settings,
        client,
        slot,
        frame,
        scratch,
        scratch_dirty_generations);
}

bool ReplicationServer::quantized_frame_active(std::uint32_t quantized_frame) const noexcept {
    return quantized_frame < quantized_frames_.size() && quantized_frames_[quantized_frame].active;
}

SyncFrame ReplicationServer::quantized_frame_frame(std::uint32_t quantized_frame) const noexcept {
    return quantized_frame < quantized_frames_.size() ? quantized_frames_[quantized_frame].frame : 0;
}

SyncArchetypeId ReplicationServer::quantized_frame_archetype(std::uint32_t quantized_frame) const noexcept {
    return quantized_frame < quantized_frames_.size() ? quantized_frames_[quantized_frame].archetype : SyncArchetypeId{};
}

const QuantizedFrameData* ReplicationServer::quantized_frame_data(std::uint32_t quantized_frame) const noexcept {
    return quantized_frame_active(quantized_frame) ? &quantized_frames_[quantized_frame].data : nullptr;
}

void ReplicationServer::retain_server_quantized_frame(std::uint32_t quantized_frame) {
    retain_quantized_frame(quantized_frame);
}

void ReplicationServer::release_server_quantized_frame(std::uint32_t quantized_frame) {
    release_quantized_frame(quantized_frame);
}

void ReplicationServer::clear_server_client_entity_state(server_detail::ClientEntityState& state) {
    clear_client_entity_state(state);
}

void ReplicationServer::acknowledge_server_cues(server_detail::ClientEntityState& state, SyncFrame frame) {
    acknowledge_cues(state, frame);
}

void ReplicationServer::send_server_update_packet(
    server_detail::ServerClientReplicator& client,
    SyncFrame frame,
    std::uint16_t entity_count,
    const ecs::BitBuffer& records,
    const std::vector<server_detail::PacketAckRecord>& ack_records) {
    send_packet(client, frame, entity_count, records, ack_records);
}

bool ReplicationServer::prepare_client_update_send(server_detail::ServerClientReplicator& replication) {
    const auto found = client_to_index_.find(replication.id);
    if (found == client_to_index_.end() ||
        found->second >= clients_.size() ||
        clients_[found->second].replication.get() != &replication) {
        return false;
    }
    if (!options_.transport) {
        throw std::logic_error("replication server requires ReplicationServerOptions::transport for serialized sends");
    }

    ClientState& client_state = clients_[found->second];
    if (!client_state.ready_for_updates || client_state.replication == nullptr) {
        return false;
    }

    replication.input_ack_frame = client_state.input.ack_frame();
    return true;
}

#ifdef KAGE_SYNC_ENABLE_TRACING
SyncTracer* ReplicationServer::server_tracer() const noexcept {
    return tracer_;
}

void ReplicationServer::trace_entity_started_syncing(
    ClientId client,
    std::uint32_t slot,
    std::uint32_t network_id,
    std::uint32_t network_version) {
    if (tracer_ == nullptr || !tracer_->enabled() || slot >= replicated_.size()) {
        return;
    }
    SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::EntityStartedSyncing, client, frame_);
    event.server_entity = replicated_[slot].entity;
    event.wire_network_id = network_id;
    event.network_version = network_version;
    event.client_network_id = make_client_entity_network_id(client, network_id, network_version);
    event.archetype = replicated_[slot].archetype;
    tracer_->trace(event);
}

void ReplicationServer::trace_entity_destroyed(
    ClientId client,
    ecs::Entity entity,
    std::uint32_t network_id,
    std::uint32_t network_version) {
    if (tracer_ == nullptr || !tracer_->enabled()) {
        return;
    }
    SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::EntityDestroyed, client, frame_);
    event.server_entity = entity;
    event.wire_network_id = network_id;
    event.network_version = network_version;
    event.client_network_id = make_client_entity_network_id(client, network_id, network_version);
    event.remove = true;
    tracer_->trace(event);
}

#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
void ReplicationServer::append_server_packet_ack_cues(
    const SyncSettings& settings,
    const server_detail::ClientEntityState& state,
    server_detail::PacketAckRecord& record) const {
    append_packet_ack_cues(settings, state, record);
}
#endif
#endif

bool ReplicationServer::acknowledge_entity(ClientId client, ecs::Entity entity, SyncFrame frame) {
    const auto client_found = client_to_index_.find(client);
    const auto replicated_found = entity_to_replicated_index_.find(entity.value);
    if (client_found == client_to_index_.end() || replicated_found == entity_to_replicated_index_.end()) {
        return false;
    }

    ClientState& client_state = clients_[client_found->second];
    if (client_state.replication == nullptr) {
        return false;
    }
    return client_state.replication->acknowledge_entity(*this, replicated_found->second, frame);
}

void ReplicationServer::receive_packet(ClientId client, ecs::BitBuffer packet) {
    inbound_packets_.push_back(PendingInboundPacket{client, std::move(packet)});
}

bool ReplicationServer::process_packet(ClientId client, ecs::BitBuffer packet) {
    const bool previous_processing = processing_client_packet_;
    const bool previous_server_error_logged = server_error_logged_;
    processing_client_packet_ = true;
    server_error_logged_ = false;
    const auto finish = [this, previous_processing, previous_server_error_logged](bool result) {
        processing_client_packet_ = previous_processing;
        server_error_logged_ = previous_server_error_logged;
        return result;
    };
    try {
        if (packet.remaining_bits() < 8U) {
            log_client_packet_warning(client, 0, "missing_message_id", "packet_missing_message_id");
            return finish(false);
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));

        if (message == protocol::client_connect_request_message) {
            return finish(process_connect_request_packet(client, packet));
        }

        const auto peer_found = peer_to_index_.find(client);
        if (peer_found == peer_to_index_.end()) {
            log_client_packet_warning(client, message, "unknown_peer", "packet_from_unknown_peer");
            return finish(false);
        }

        ClientState& state = clients_[peer_found->second];
        state.idle_seconds = 0.0;
        switch (message) {
        case protocol::client_connect_ack_message:
            return finish(process_connection_request_ack_packet(state, packet));
        case protocol::client_ping_message:
            return finish(process_ping_packet(state, packet));
        case protocol::client_ack_message:
            return finish(state.replication != nullptr && process_client_ack_packet(*state.replication, packet));
        default:
            log_client_packet_warning(client, message, "unknown_message", "unknown_client_message");
            return finish(false);
        }
    } catch (const std::exception& ex) {
        if (server_error_logged_) {
            server_error_logged_ = false;
        } else {
            log_client_packet_warning(client, 0, "decode_exception", ex.what());
        }
        return finish(false);
    }
}

bool ReplicationServer::process_packet(ecs::Registry& registry, ClientId client, ecs::BitBuffer packet) {
    const bool previous_processing = processing_client_packet_;
    const bool previous_server_error_logged = server_error_logged_;
    processing_client_packet_ = true;
    server_error_logged_ = false;
    const auto finish = [this, previous_processing, previous_server_error_logged](bool result) {
        processing_client_packet_ = previous_processing;
        server_error_logged_ = previous_server_error_logged;
        return result;
    };
    try {
        if (packet.remaining_bits() < 8U) {
            log_client_packet_warning(client, 0, "missing_message_id", "packet_missing_message_id");
            return finish(false);
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));

        if (message == protocol::client_connect_request_message) {
            return finish(process_connect_request_packet(client, packet));
        }

        const auto peer_found = peer_to_index_.find(client);
        if (peer_found == peer_to_index_.end()) {
            log_client_packet_warning(client, message, "unknown_peer", "packet_from_unknown_peer");
            return finish(false);
        }

        ClientState& state = clients_[peer_found->second];
        state.idle_seconds = 0.0;
        return finish(process_message_from_connected_client(registry, state, message, packet));
    } catch (const std::exception& ex) {
        if (server_error_logged_) {
            server_error_logged_ = false;
        } else {
            log_client_packet_warning(client, 0, "decode_exception", ex.what());
        }
        return finish(false);
    }
}

bool ReplicationServer::process_connect_request_packet(ClientId peer, ecs::BitBuffer& packet) {
    std::string token;
    if (!protocol::read_string(packet, token)) {
        log_client_packet_warning(peer, protocol::client_connect_request_message, "malformed_connect_request", "malformed_connect_request");
        return false;
    }
    const auto peer_found = peer_to_index_.find(peer);
    if (peer_found != peer_to_index_.end()) {
        clients_[peer_found->second].idle_seconds = 0.0;
        log_info("client_connect_duplicate", "peer=" + std::to_string(peer) +
            " client=" + std::to_string(clients_[peer_found->second].id));
        send_connect_response(clients_[peer_found->second]);
        return true;
    }

    ClientId accepted_client = next_connect_client_id_;
    std::string error;
    bool accepted = true;
    if (options_.connect_handler) {
        try {
            accepted = options_.connect_handler(token, accepted_client, error);
        } catch (const std::exception& ex) {
            log_server_error(peer, "connect_handler", ex.what());
            throw;
        }
    }
    if (!accepted || accepted_client == invalid_client_id ||
        accepted_client > max_client_entity_network_id_client) {
        if (accepted && error.empty() && accepted_client > max_client_entity_network_id_client) {
            error = "client id out of range";
        }
        ecs::BitBuffer response;
        response.reserve_bytes(options_.mtu_bytes);
        response.push_bits(protocol::server_connect_response_message, 8U);
        response.push_bool(false);
        protocol::write_string(response, error);
        if (options_.transport) {
            try {
                options_.transport(peer, response);
            } catch (const std::exception& ex) {
                log_server_error(peer, "transport_error_connect_rejection", ex.what());
                throw;
            }
        }
        ++observability_stats_.client_connects_rejected;
        log_info("client_connect_rejected", "peer=" + std::to_string(peer) +
            " client=" + std::to_string(accepted_client));
        return accepted_client != invalid_client_id || !accepted;
    }

    if (!add_client_for_peer(peer, accepted_client, false)) {
        return false;
    }
    send_connect_response(clients_[client_to_index_[accepted_client]]);
    return true;
}

bool ReplicationServer::process_message_from_connected_client(
    ecs::Registry& registry,
    ClientState& client,
    std::uint8_t message,
    ecs::BitBuffer& packet) {
    switch (message) {
    case protocol::client_connect_ack_message:
        return process_connection_request_ack_packet(client, packet);
    case protocol::client_ping_message:
        if (!client.ready_for_updates) {
            log_client_packet_warning(client.peer, message, "ping_before_ready", "ping_before_connection_ready");
            return false;
        }
        return process_ping_packet(client, packet);
    case protocol::client_input_message:
        if (!client.ready_for_updates || client.replication == nullptr) {
            log_client_packet_warning(client.peer, message, "input_before_ready", "input_before_connection_ready");
            return false;
        }
        return process_input_with_acks_packet(registry, client, packet);
    case protocol::client_ack_message:
        return client.ready_for_updates && client.replication != nullptr && process_client_ack_packet(*client.replication, packet);
    default:
        log_client_packet_warning(client.peer, message, "unknown_message", "unknown_client_message");
        return false;
    }
}

bool ReplicationServer::process_connection_request_ack_packet(ClientState& client, ecs::BitBuffer& packet) {
    if (packet.remaining_bits() < 64U) {
        log_client_packet_warning(client.peer, protocol::client_connect_ack_message, "truncated_connect_ack", "truncated_connect_ack");
        return false;
    }
    const ClientId acked_client = static_cast<ClientId>(packet.read_unsigned_bits(64U));
    if (acked_client != client.id) {
        log_client_packet_warning(client.peer, protocol::client_connect_ack_message, "connect_ack_client_mismatch", "connect_ack_client_id_mismatch");
        return false;
    }
    client.ready_for_updates = true;
    create_client_replicator(client);
    ++observability_stats_.clients_ready;
    log_info("client_ready", "peer=" + std::to_string(client.peer) +
        " client=" + std::to_string(client.id));
    return true;
}

bool ReplicationServer::process_ping_packet(ClientState& client, ecs::BitBuffer& packet) {
    if (packet.remaining_bits() < 64U) {
        log_client_packet_warning(client.peer, protocol::client_ping_message, "truncated_ping", "truncated_ping");
        return false;
    }
    const auto sequence = static_cast<std::uint32_t>(packet.read_bits(32U));
    const auto send_frame = static_cast<SyncFrame>(packet.read_bits(32U));
    std::uint16_t send_subframe = 0;
    if (packet.remaining_bits() >= protocol::frame_subframe_bits) {
        send_subframe = static_cast<std::uint16_t>(packet.read_bits(protocol::frame_subframe_bits));
    }
    send_pong(client.peer, sequence, send_frame, send_subframe);
    return true;
}

bool ReplicationServer::process_client_ack_packet(ServerClientReplicator& replication, ecs::BitBuffer& packet) {
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    std::vector<std::uint32_t> acks;
#endif
    const ClientUpdateAckResult ack_result = process_client_acks_from_packet(
        replication,
        packet
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
        ,
        acks
#endif
    );
    if (!ack_result.packet_valid) {
        log_client_packet_warning(replication.peer, protocol::client_ack_message, "malformed_ack_packet", "malformed_ack_packet");
        return false;
    }
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    trace_incoming_ack_packet(replication, acks);
#endif
    return ack_result.all_acknowledged;
}

bool ReplicationServer::set_local_input_bytes(ecs::Registry& registry, ecs::Entity component, const void* input) {
    if (local_client_ == invalid_client_id || input == nullptr) {
        return false;
    }
    const auto found_client = client_to_index_.find(local_client_);
    if (found_client == client_to_index_.end() || !clients_[found_client->second].local) {
        return false;
    }
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!settings.input_component || settings.input_component != component) {
        return false;
    }
    const auto found_ops = settings.component_ops.find(component.value);
    if (found_ops == settings.component_ops.end() ||
        found_ops->second.quantize == nullptr ||
        found_ops->second.push_to_ecs == nullptr ||
        found_ops->second.quantized_size == 0U) {
        return false;
    }

    const SyncComponentOps& ops = found_ops->second;
    std::vector<std::uint8_t> quantized(ops.quantized_size);
    ops.quantize(input, quantized.data());
    return clients_[found_client->second].input.set_local_frame(
        frame_ + 1U,
        quantized.data(),
        quantized.size(),
        options_.input_buffer_capacity_frames);
}

ReplicationServer::ClientUpdateAckResult ReplicationServer::process_client_acks_from_packet(
    ServerClientReplicator& replication,
    ecs::BitBuffer& packet
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    ,
    std::vector<std::uint32_t>& trace_acks
#endif
) {
    const std::size_t packet_id_bits = configured_packet_id_bits(options_);
    if (packet.remaining_bits() < 16U) {
        return ClientUpdateAckResult{};
    }
    const auto ack_count = static_cast<std::uint16_t>(packet.read_bits(16U));
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    trace_acks.reserve(trace_acks.size() + ack_count);
#endif

    bool all_acknowledged = true;
    for (std::uint16_t ack = 0; ack < ack_count; ++ack) {
        if (packet.remaining_bits() < packet_id_bits) {
            return ClientUpdateAckResult{};
        }
        const auto packet_id = static_cast<std::uint32_t>(packet.read_bits(packet_id_bits));
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
        trace_acks.push_back(packet_id);
#endif
        all_acknowledged =
            replication.ack_tracker.acknowledge_packet(*this, replication, packet_id) && all_acknowledged;
    }
    replication.ack_tracker.cleanup_packet_acks(*this, replication);
    return ClientUpdateAckResult{true, all_acknowledged};
}

bool ReplicationServer::process_input_with_acks_packet(
    ecs::Registry& registry,
    ClientState& client,
    ecs::BitBuffer& packet) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!settings.input_component) {
        log_server_error(client.peer, "client_input", "server registry has no input component");
        return false;
    }
    const auto found_ops = settings.component_ops.find(settings.input_component.value);
    if (found_ops == settings.component_ops.end() ||
        found_ops->second.deserialize == nullptr ||
        found_ops->second.push_to_ecs == nullptr ||
        found_ops->second.quantized_size == 0U) {
        log_server_error(client.peer, "client_input", "server registry input component is not serializable");
        return false;
    }
    const SyncComponentOps& ops = found_ops->second;

    if (client.replication == nullptr) {
        log_client_packet_warning(client.peer, protocol::client_input_message, "input_without_replication_state", "input_from_client_without_replication_state");
        return false;
    }
    ServerClientReplicator& replication = *client.replication;
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    std::vector<std::uint32_t> acks;
#endif
    const ClientUpdateAckResult ack_result = process_client_acks_from_packet(
        replication,
        packet
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
        ,
        acks
#endif
    );
    if (!ack_result.packet_valid) {
        log_client_packet_warning(client.peer, protocol::client_input_message, "malformed_input_ack_records", "malformed_input_ack_records");
        return false;
    }

    server_detail::ServerInputPacketTrace trace;
    if (!client.input.process_packet_payload(packet, ops, options_.input_buffer_capacity_frames, &trace)) {
        log_client_packet_warning(client.peer, protocol::client_input_message, "malformed_input_payload", "malformed_input_payload");
        return false;
    }
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    trace_incoming_input_packet(client, acks, trace.baseline_frame, trace.first_input_frame, trace.last_input_frame);
#endif
    return true;
}

void ReplicationServer::log_info(const char* event, const std::string& fields) const {
    if (logger_ != nullptr && logger_->should_log(spdlog::level::info)) {
        logger_->info("event={} frame={} {}", event, frame_, fields);
    }
}

void ReplicationServer::log_client_packet_warning(
    ClientId peer,
    std::uint8_t message,
    const char* reason_code,
    const char* reason_detail) {
    ++observability_stats_.client_packet_warnings;
    const std::uint32_t max_logs = options_.logging.max_warning_logs_per_peer;
    std::uint32_t& logged_count = warning_logs_by_peer_[peer];
    if (max_logs != 0U && logged_count >= max_logs) {
        ++observability_stats_.suppressed_client_packet_warnings;
        if (logged_count == max_logs) {
            ++logged_count;
            if (logger_ != nullptr && logger_->should_log(spdlog::level::warn)) {
                logger_->warn(
                    "event=client_packet_warnings_suppressed frame={} peer={} max_logs={}",
                    frame_,
                    peer,
                    max_logs);
            }
        }
        return;
    }
    ++logged_count;
    if (logger_ != nullptr && logger_->should_log(spdlog::level::warn)) {
        logger_->warn(
            "event=client_packet_rejected frame={} peer={} message_id={} reason={} detail={}",
            frame_,
            peer,
            message,
            reason_code,
            log_token(reason_detail));
    }
}

void ReplicationServer::log_server_error(ClientId peer, const char* event, const char* reason) {
    if (processing_client_packet_) {
        server_error_logged_ = true;
    }
    ++observability_stats_.server_errors;
    if (logger_ != nullptr && logger_->should_log(spdlog::level::err)) {
        logger_->error("event={} frame={} peer={} reason={}", event, frame_, peer, log_token(reason));
    }
}

std::size_t ReplicationServer::retained_quantized_frame_count() const noexcept {
    std::size_t count = 0;
    for (const QuantizedFrame& quantized_frame : quantized_frames_) {
        if (quantized_frame.active && quantized_frame.ref_count != 0) {
            ++count;
        }
    }
    return count;
}

std::size_t ReplicationServer::retained_quantized_frame_bytes() const noexcept {
    std::size_t bytes = 0;
    for (const QuantizedFrame& quantized_frame : quantized_frames_) {
        if (!quantized_frame.active || quantized_frame.ref_count == 0) {
            continue;
        }
        bytes += quantized_frame.data.bytes.size();
    }
    return bytes;
}

void ReplicationServer::push_client_inputs_to_ecs(ecs::Registry& registry) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!settings.input_component) {
        return;
    }
    const auto found_ops = settings.component_ops.find(settings.input_component.value);
    if (found_ops == settings.component_ops.end() || found_ops->second.push_to_ecs == nullptr) {
        return;
    }
    const SyncComponentOps& ops = found_ops->second;
    const SyncFrame input_frame_num = frame_;
    std::unordered_map<ClientId, server_detail::ServerInputForFrame> input_for_frame_cache;

    registry.view<const NetworkOwner>().each([this, &input_for_frame_cache, input_frame_num, &ops, &settings, &registry]
          (ecs::Entity entity, const NetworkOwner& owner) {
        const auto found_client = client_to_index_.find(owner.client);
        if (found_client == client_to_index_.end()) {
            return;
        }
        ClientState& client = clients_[found_client->second];

        server_detail::ServerInputForFrame& input_for_frame = input_for_frame_cache[client.id];
        if (!input_for_frame.cached) {
            input_for_frame = client.input.select_input_for_frame(input_frame_num, ops.quantized_size);
        }

        if (input_for_frame.bytes == nullptr || input_for_frame.bytes->size() != ops.quantized_size) {
#ifdef KAGE_SYNC_ENABLE_TRACING
            trace_input_starved(entity, client, input_frame_num, input_for_frame.input_frame, settings.input_component, ops);
#endif
            return;
        }
        (void)ops.push_to_ecs(registry, entity, input_for_frame.bytes->data());
#ifdef KAGE_SYNC_ENABLE_TRACING
        trace_input_component(entity, client, input_frame_num, settings.input_component, ops, input_for_frame.bytes->data());
        if (input_for_frame.input_frame < input_frame_num) {
            trace_input_starved(entity, client, input_frame_num, input_for_frame.input_frame, settings.input_component, ops);
        }
#endif
    });
}

bool ReplicationServer::tick(ecs::Registry& registry, double dt_seconds) {
    if (dt_seconds < 0.0 || !std::isfinite(dt_seconds)) {
        return false;
    }

    advance_client_idle_timers(dt_seconds);
    tick_accumulator_seconds_ += dt_seconds;

    for (PendingInboundPacket& inbound : inbound_packets_) {
        (void)process_packet(registry, inbound.client, std::move(inbound.packet));
    }
    inbound_packets_.clear();

    resend_pending_connect_responses(dt_seconds);
    disconnect_timed_out_clients();

    std::uint32_t completed_frames = 0;
    while (tick_accumulator_seconds_ >= options_.fixed_dt_seconds) {
        tick_accumulator_seconds_ -= options_.fixed_dt_seconds;
        ++frame_;
        ++completed_frames;
        push_client_inputs_to_ecs(registry);
        registry.run_jobs();
        push_dirty_info_to_listeners(registry);
    }

    if (completed_frames > 0U) {
        push_frame_to_listeners(registry, dt_seconds, completed_frames);
    }

    return true;
}

void ReplicationServer::advance_client_idle_timers(double dt_seconds) {
    if (options_.idle_client_timeout_seconds == 0.0 || dt_seconds == 0.0) {
        return;
    }
    for (ClientState& client : clients_) {
        if (!client.local) {
            client.idle_seconds += dt_seconds;
        }
    }
}

void ReplicationServer::resend_pending_connect_responses(double dt_seconds) {
    if (dt_seconds == 0.0 || options_.connect_resend_interval_seconds == 0.0) {
        return;
    }
    for (ClientState& client : clients_) {
        if (client.local || client.ready_for_updates || client.replication != nullptr) {
            continue;
        }
        client.connect_resend_accumulator_seconds += dt_seconds;
        if (client.connect_resend_accumulator_seconds >= options_.connect_resend_interval_seconds) {
            send_connect_response(client);
        }
    }
}

void ReplicationServer::disconnect_timed_out_clients() {
    if (options_.idle_client_timeout_seconds == 0.0) {
        return;
    }
    for (std::size_t index = 0; index < clients_.size();) {
        const ClientState& client = clients_[index];
        if (client.local) {
            ++index;
            continue;
        }
        if (client.idle_seconds >= options_.idle_client_timeout_seconds) {
            const ClientId client_id = client.id;
            ++observability_stats_.clients_timed_out;
            log_info("client_timed_out", "peer=" + std::to_string(client.peer) +
                " client=" + std::to_string(client.id));
            remove_client(client_id);
            continue;
        }
        ++index;
    }
}

void ReplicationServer::push_dirty_info_to_listeners(ecs::Registry& registry) {
    const auto dirty_scope = registry.dirty_scope();
    const ecs::Registry::DirtyView dirty = dirty_scope.view();
    rediscover_replicated_entities(registry, dirty);
    const SyncSettings& settings = registry.get<SyncSettings>();
    capture_dirty_generations(dirty, settings);
#ifdef KAGE_SYNC_ENABLE_TRACING
    trace_frame_components(registry, settings);
#endif
    capture_queued_cues(registry, settings);
    const QueuedSyncCueView cues{
        post_tick_cues_.empty() ? nullptr : post_tick_cues_.data(),
        post_tick_cues_.size()};
    ServerFrameDelta frame_delta{*this, registry, frame_, dirty, cues};

    for (const ServerFrameConsumerSubscription::Entry& entry : frame_consumers_->consumers) {
        if (entry.consumer != nullptr) {
            entry.consumer->accumulate_frame_delta(frame_delta);
        }
    }
    remove_unsubscribed_frame_consumers();

    post_tick_cues_.clear();
}

void ReplicationServer::push_frame_to_listeners(
    ecs::Registry& registry,
    double dt_seconds,
    std::uint32_t completed_frames) {
    ServerFrameFlushContext context{*this, registry, frame_, dt_seconds, completed_frames};
    for (const ServerFrameConsumerSubscription::Entry& entry : frame_consumers_->consumers) {
        if (entry.consumer != nullptr) {
            entry.consumer->flush(context);
        }
    }
    remove_unsubscribed_frame_consumers();
}

void ReplicationServer::remove_unsubscribed_frame_consumers() {
    frame_consumers_->consumers.erase(
        std::remove_if(
            frame_consumers_->consumers.begin(),
            frame_consumers_->consumers.end(),
            [](const ServerFrameConsumerSubscription::Entry& entry) {
                return entry.consumer == nullptr;
            }),
        frame_consumers_->consumers.end());
}

void ReplicationServer::capture_dirty_generations(ecs::Registry::DirtyView dirty, const SyncSettings& settings) {
    for (const SyncArchetype& archetype : settings.archetypes) {
        for (const SyncTagReplication& tag_replication : archetype.tags) {
            dirty.each_dirty(tag_replication.tag, [&](ecs::Entity entity, const void*) {
                const auto found = entity_to_replicated_index_.find(entity.value);
                if (found != entity_to_replicated_index_.end()) {
                    mark_dirty_tag(settings, found->second, tag_replication.tag);
                }
            });
            dirty.each_removed(tag_replication.tag, [&](ecs::Registry::ComponentRemoval removal) {
                const auto found = entity_index_to_replicated_index_.find(removal.entity_index);
                if (found != entity_index_to_replicated_index_.end()) {
                    mark_dirty_tag(settings, found->second, tag_replication.tag);
                }
            });
        }
    }

    for (const auto& component_ops : settings.component_ops) {
        const ecs::Entity component{component_ops.first};
        dirty.each_dirty(component, [&](ecs::Entity entity, const void*) {
            const auto found = entity_to_replicated_index_.find(entity.value);
            if (found != entity_to_replicated_index_.end()) {
                mark_dirty_component(settings, found->second, component);
            }
        });
        dirty.each_removed(component, [&](ecs::Registry::ComponentRemoval removal) {
            const auto found = entity_index_to_replicated_index_.find(removal.entity_index);
            if (found != entity_index_to_replicated_index_.end()) {
                mark_dirty_component(settings, found->second, component);
            }
        });
    }

    dirty.each_dirty<NetworkOwner>([&](ecs::Entity entity, const void*) {
        const auto found = entity_to_replicated_index_.find(entity.value);
        if (found != entity_to_replicated_index_.end()) {
            mark_owner_visibility_dirty(settings, found->second);
        }
    });
    dirty.each_removed<NetworkOwner>([&](ecs::Registry::ComponentRemoval removal) {
        const auto found = entity_index_to_replicated_index_.find(removal.entity_index);
        if (found != entity_index_to_replicated_index_.end()) {
            mark_owner_visibility_dirty(settings, found->second);
        }
    });
}

bool ReplicationServer::play_local_cue(
    ecs::Registry& registry,
    const SyncSettings& settings,
    const QueuedSyncCue& cue) {
    if (local_client_ == invalid_client_id ||
        cue.type >= settings.cue_ops.size() ||
        settings.cue_ops[cue.type].play == nullptr ||
        !registry.alive(cue.entity)) {
        return false;
    }
    const NetworkOwner* owner = registry.try_get<NetworkOwner>(cue.entity);
    if (cue.only_replicate_to_owner && (owner == nullptr || owner->client != local_client_)) {
        return true;
    }

    struct ReferenceContextData {
        ReplicationServer* server = nullptr;
    } reference_context_data{this};
    EntityReferenceContext reference_context;
    reference_context.user = &reference_context_data;
    reference_context.network_entity_id_tier0_bits = options_.protocol.network_entity_id_tier0_bits;
    reference_context.server_network_id_for_entity = [](void* user, ecs::Entity entity) {
        ReferenceContextData& data = *static_cast<ReferenceContextData*>(user);
        const auto found = data.server->entity_to_replicated_index_.find(entity.value);
        if (found == data.server->entity_to_replicated_index_.end()) {
            return 0U;
        }
        return found->second + 1U;
    };
    reference_context.client_entity_network_id_for_wire = [](void* user, std::uint32_t wire_network_id) {
        if (wire_network_id == 0U) {
            return invalid_client_entity_network_id;
        }
        ReferenceContextData& data = *static_cast<ReferenceContextData*>(user);
        const std::uint32_t slot = wire_network_id - 1U;
        if (slot >= data.server->replicated_.size() || !data.server->replicated_[slot].active) {
            return invalid_client_entity_network_id;
        }
        return make_client_entity_network_id(data.server->local_client_, wire_network_id, 1U);
    };
    reference_context.client_local_entity = [](void* user, ClientEntityNetworkId network_id) {
        ReferenceContextData& data = *static_cast<ReferenceContextData*>(user);
        const std::uint32_t slot = client_entity_network_id_wire_id(network_id) - 1U;
        if (slot >= data.server->replicated_.size() || !data.server->replicated_[slot].active) {
            return ecs::Entity{};
        }
        return data.server->replicated_[slot].entity;
    };

    ecs::BitBuffer payload = cue.payload;
    if (settings.cue_ops[cue.type].references_entities) {
        if (!cue.value || settings.cue_ops[cue.type].serialize == nullptr) {
            return false;
        }
        payload.clear();
        settings.cue_ops[cue.type].serialize(cue.value.get(), payload, &reference_context);
    }
    return settings.cue_ops[cue.type].play(registry, cue.entity, payload, 0.0f, cue.frame, &reference_context);
}

void ReplicationServer::capture_queued_cues(ecs::Registry& registry, const SyncSettings& settings) {
    post_tick_cues_.clear();
    if (!settings.cue_queue) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(settings.cue_queue->mutex);
        post_tick_cues_.swap(settings.cue_queue->cues);
    }

    for (QueuedSyncCue& cue : post_tick_cues_) {
        if (cue.frame == 0U) {
            cue.frame = frame_;
        }
        const auto found = entity_to_replicated_index_.find(cue.entity.value);
        if (found == entity_to_replicated_index_.end()) {
            continue;
        }
        (void)play_local_cue(registry, settings, cue);
#ifdef KAGE_SYNC_ENABLE_TRACING
        if (tracer_ != nullptr && tracer_->enabled()) {
            SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::CueEmitted, invalid_client_id, cue.frame);
            event.server_entity = cue.entity;
            if (found->second < replicated_.size()) {
                event.archetype = replicated_[found->second].archetype;
            }
            event.cue_type = cue.type;
            append_trace_cue_name(settings, cue.type, event);
            append_trace_cue_data(tracer_, settings, cue.type, cue.payload, event);
            append_trace_data_field(event, "source", "server");
            tracer_->trace(event);
        }
#endif
        attach_cue_to_clients(registry, settings, found->second, cue);
    }
}

void ReplicationServer::attach_cue_to_clients(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    std::uint32_t slot,
    const QueuedSyncCue& cue) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }
    if (cue.type >= settings.cue_ops.size() || settings.cue_ops[cue.type].serialize == nullptr) {
        return;
    }
    const SyncFrame relevance_frames = static_cast<SyncFrame>(
        std::ceil(static_cast<double>(cue.relevance_seconds) / options_.fixed_dt_seconds));
    const SyncFrame expire_frame = cue.frame + relevance_frames;
    const NetworkOwner* owner = registry.try_get<NetworkOwner>(cue.entity);
    for (ClientState& client_state : clients_) {
        if (client_state.replication == nullptr) {
            continue;
        }
        ServerClientReplicator& client = *client_state.replication;
        if (cue.only_replicate_to_owner && (owner == nullptr || owner->client != client.id)) {
            continue;
        }
        ClientEntityState* state = client.entities.try_get(slot);
        if (state == nullptr) {
            continue;
        }
        ecs::BitBuffer payload = cue.payload;
        if (settings.cue_ops[cue.type].references_entities) {
            if (!cue.value) {
                continue;
            }
            struct ReferenceContextData {
                ReplicationServer* server = nullptr;
                ServerClientReplicator* client = nullptr;
                std::uint32_t source_slot = 0;
            } reference_context_data{this, &client, slot};
            EntityReferenceContext reference_context;
            reference_context.user = &reference_context_data;
            reference_context.network_entity_id_tier0_bits = options_.protocol.network_entity_id_tier0_bits;
            reference_context.server_network_id_for_entity = [](void* user, ecs::Entity entity) {
                ReferenceContextData& data = *static_cast<ReferenceContextData*>(user);
                const auto found = data.server->entity_to_replicated_index_.find(entity.value);
                if (found == data.server->entity_to_replicated_index_.end()) {
                    return 0U;
                }
                const std::uint32_t reference_slot = found->second;
                if (reference_slot >= data.server->replicated_.size() ||
                    data.client->entities.try_get(reference_slot) == nullptr ||
                    !data.server->replicated_[reference_slot].active) {
                    return 0U;
                }
                if (reference_slot != data.source_slot) {
                    data.client->entities.try_get(reference_slot)->reference_priority_boost_pending = true;
                }
                return data.client->network_id_for(*data.server, reference_slot);
            };
            payload.clear();
            settings.cue_ops[cue.type].serialize(cue.value.get(), payload, &reference_context);
        }
        state->pending_cues.push_back(ClientEntityState::PendingCue{
            cue.frame,
            expire_frame,
            cue.type,
            cue.relevance_seconds,
            payload});
        client.mark_dirty(*this, slot, frame_);
    }
}

void ReplicationServer::mark_dirty_component(
    const SyncSettings& settings,
    std::uint32_t slot,
    ecs::Entity component) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }
    ReplicatedSlot& replicated = replicated_[slot];
    if (replicated.archetype.value >= settings.archetypes.size()) {
        return;
    }
    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    if (replicated.component_dirty_generations.size() < sync_slot_count(archetype)) {
        replicated.component_dirty_generations.resize(sync_slot_count(archetype), 1U);
    }
    for (std::size_t index = 0; index < archetype.components.size(); ++index) {
        if (archetype.components[index].component == component) {
            const std::size_t dirty_index = index + 1U;
            ++replicated.component_dirty_generations[dirty_index];
            if (replicated.component_dirty_generations[dirty_index] == 0) {
                replicated.component_dirty_generations[dirty_index] = 1;
            }
            return;
        }
    }
}

void ReplicationServer::mark_dirty_tag(const SyncSettings& settings, std::uint32_t slot, ecs::Entity tag) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }
    const ReplicatedSlot& replicated = replicated_[slot];
    if (replicated.archetype.value >= settings.archetypes.size()) {
        return;
    }
    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    for (const SyncTagReplication& replication : archetype.tags) {
        if (replication.tag == tag) {
            mark_dirty_tags(settings, slot);
            return;
        }
    }
}

void ReplicationServer::mark_dirty_tags(const SyncSettings& settings, std::uint32_t slot) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }
    ReplicatedSlot& replicated = replicated_[slot];
    if (replicated.archetype.value >= settings.archetypes.size()) {
        return;
    }
    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    if (archetype.tags.empty()) {
        return;
    }
    if (replicated.component_dirty_generations.size() < sync_slot_count(archetype)) {
        replicated.component_dirty_generations.resize(sync_slot_count(archetype), 1U);
    }
    ++replicated.component_dirty_generations[0];
    if (replicated.component_dirty_generations[0] == 0) {
        replicated.component_dirty_generations[0] = 1;
    }
}

void ReplicationServer::mark_owner_visibility_dirty(const SyncSettings& settings, std::uint32_t slot) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }
    ReplicatedSlot& replicated = replicated_[slot];
    if (replicated.archetype.value >= settings.archetypes.size()) {
        return;
    }
    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    if (replicated.component_dirty_generations.size() < sync_slot_count(archetype)) {
        replicated.component_dirty_generations.resize(sync_slot_count(archetype), 1U);
    }
    for (const SyncTagReplication& replication : archetype.tags) {
        if (replication.audience == ReplicationAudience::Owner) {
            ++replicated.component_dirty_generations[0];
            if (replicated.component_dirty_generations[0] == 0) {
                replicated.component_dirty_generations[0] = 1;
            }
            break;
        }
    }
    for (std::size_t index = 0; index < archetype.components.size(); ++index) {
        if (archetype.components[index].audience == ReplicationAudience::Owner) {
            const std::size_t dirty_index = index + 1U;
            ++replicated.component_dirty_generations[dirty_index];
            if (replicated.component_dirty_generations[dirty_index] == 0) {
                replicated.component_dirty_generations[dirty_index] = 1;
            }
        }
    }
}

bool ReplicationServer::archetype_is_same_frame_cacheable(const SyncArchetype& archetype) {
    for (const SyncTagReplication& replication : archetype.tags) {
        if (replication.audience != ReplicationAudience::All) {
            return false;
        }
    }
    for (const ComponentReplication& replication : archetype.components) {
        if (replication.audience != ReplicationAudience::All) {
            return false;
        }
    }
    return true;
}

bool server_detail::ServerClientReplicator::UpdateWriter::serialize_entity(
    ReplicationServer& server,
    const ecs::Registry& registry,
    const SyncSettings& settings,
    ServerClientReplicator& client,
    std::uint32_t slot,
    SyncFrame frame,
    std::uint64_t component_mask,
    SerializedEntity& out) {
    if (slot >= server.replicated_slot_count() || client.entities.try_get(slot) == nullptr) {
        return false;
    }

    const SyncArchetypeId archetype = server.replicated_slot_archetype(slot);
    if (archetype.value >= settings.archetypes.size()) {
        return false;
    }

    const std::uint32_t quantized_frame = server.quantized_frame_for_client(
        registry,
        settings,
        client,
        slot,
        frame,
        quantized_frame_scratch_,
        quantized_frame_dirty_scratch_);
    if (quantized_frame == server_detail::invalid_quantized_frame_id) {
        return false;
    }

    out.quantized_frame = quantized_frame;
    write_entity_record(
        server,
        registry,
        settings,
        client,
        slot,
        quantized_frame,
        component_mask,
        out.payload);
    return !out.payload.empty();
}

std::uint32_t ReplicationServer::find_or_create_quantized_frame(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    const ServerClientReplicator& client,
    std::uint32_t slot,
    SyncFrame frame,
    QuantizedFrameData& scratch,
    std::vector<std::uint64_t>& scratch_dirty_generations) {
    if (slot >= replicated_.size()) {
        return invalid_quantized_frame_id;
    }

    const ReplicatedSlot& replicated = replicated_[slot];
    const SyncArchetype& archetype = settings.archetypes[replicated.archetype.value];
    if (replicated.same_frame_cacheable &&
        replicated.same_frame_quantized_frame_frame == frame &&
        replicated.same_frame_quantized_frame != invalid_quantized_frame_id &&
        replicated.same_frame_quantized_frame < quantized_frames_.size()) {
        const QuantizedFrame& cached = quantized_frames_[replicated.same_frame_quantized_frame];
        if (cached.active && cached.slot == slot && cached.frame == frame &&
            cached.archetype == replicated.archetype) {
            return replicated.same_frame_quantized_frame;
        }
    }

    const NetworkOwner* owner = registry.try_get<NetworkOwner>(replicated.entity);
    const ClientEntityState* entity_state = client.entities.try_get(slot);
    const QuantizedFrame* baseline_quantized_frame = nullptr;
    if (entity_state != nullptr &&
        entity_state->baseline != invalid_quantized_frame_id &&
        entity_state->baseline < quantized_frames_.size() &&
        quantized_frames_[entity_state->baseline].active &&
        quantized_frames_[entity_state->baseline].archetype == replicated.archetype) {
        baseline_quantized_frame = &quantized_frames_[entity_state->baseline];
    }

    if (!init_frame_data(archetype, scratch)) {
        return invalid_quantized_frame_id;
    }
    scratch_dirty_generations.assign(sync_slot_count(archetype), 0U);
    if (has_tag_slot(archetype)) {
        scratch.tag_mask = visible_tag_mask(registry, archetype, replicated.entity, client.id);
        scratch_dirty_generations[0] = !replicated.component_dirty_generations.empty()
            ? replicated.component_dirty_generations[0]
            : 0;
    }
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        const ComponentReplication& replication = archetype.components[component_index];
        if (replication.audience == ReplicationAudience::Owner &&
            (owner == nullptr || owner->client != client.id)) {
            continue;
        }

        const void* component_value = registry.get(replicated.entity, replication.component);
        if (component_value == nullptr) {
            continue;
        }

        if (component_index >= archetype.component_ops.size()) {
            throw std::logic_error("sync component traits are not registered for replicated component");
        }
        const SyncComponentOps& ops = archetype.component_ops[component_index];

        const std::size_t dirty_index = component_index + 1U;
        const std::uint64_t dirty_generation = dirty_index < replicated.component_dirty_generations.size()
            ? replicated.component_dirty_generations[dirty_index]
            : 0;
        scratch_dirty_generations[dirty_index] = dirty_generation;

        std::uint8_t* destination = mutable_frame_component_data(archetype, scratch, component_index);
        if (destination == nullptr) {
            return invalid_quantized_frame_id;
        }
        if (baseline_quantized_frame != nullptr &&
            dirty_index < baseline_quantized_frame->dirty_generations.size() &&
            baseline_quantized_frame->dirty_generations[dirty_index] == dirty_generation &&
            frame_has_component(baseline_quantized_frame->data, component_index)) {
            const std::size_t offset = archetype.component_offsets[component_index];
            const std::size_t size = archetype.component_ops[component_index].quantized_size;
            if (offset + size > baseline_quantized_frame->data.bytes.size()) {
                return invalid_quantized_frame_id;
            }
            std::memcpy(destination, baseline_quantized_frame->data.bytes.data() + offset, size);
        } else {
            if (ops.quantize == nullptr) {
                return invalid_quantized_frame_id;
            }
            ops.quantize(component_value, destination);
        }
    }

    if (scratch.present_mask == 0U && !has_tag_slot(archetype)) {
        return invalid_quantized_frame_id;
    }

    for (const std::uint32_t index : replicated_[slot].quantized_frames) {
        if (index >= quantized_frames_.size()) {
            continue;
        }
        const QuantizedFrame& quantized_frame = quantized_frames_[index];
        if (quantized_frame.active && quantized_frame.slot == slot && quantized_frame.frame == frame &&
            quantized_frame.archetype == replicated.archetype &&
            same_quantized_frame_components(quantized_frame, scratch, scratch_dirty_generations)) {
            return index;
        }
    }

    std::uint32_t index = invalid_quantized_frame_id;
    if (!free_quantized_frames_.empty()) {
        index = free_quantized_frames_.back();
        free_quantized_frames_.pop_back();
    } else {
        if (quantized_frames_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::length_error("kage sync quantized frame space exhausted");
        }
        index = static_cast<std::uint32_t>(quantized_frames_.size());
        quantized_frames_.push_back(QuantizedFrame{});
    }

    QuantizedFrame& quantized_frame = quantized_frames_[index];
    quantized_frame.slot = slot;
    quantized_frame.frame = frame;
    quantized_frame.archetype = replicated.archetype;
    quantized_frame.ref_count = 0;
    quantized_frame.active = true;
    quantized_frame.data = std::move(scratch);
    quantized_frame.dirty_generations = std::move(scratch_dirty_generations);
    replicated_[slot].quantized_frames.push_back(index);
    if (replicated_[slot].same_frame_cacheable) {
        replicated_[slot].same_frame_quantized_frame = index;
        replicated_[slot].same_frame_quantized_frame_frame = frame;
    }
    return index;
}

void server_detail::ServerClientReplicator::UpdateWriter::write_entity_record(
    ReplicationServer& server,
    const ecs::Registry& registry,
    const SyncSettings& settings,
    ServerClientReplicator& client,
    std::uint32_t slot,
    std::uint32_t quantized_frame_id,
    std::uint64_t component_mask,
    ecs::BitBuffer& out) {
    const ClientEntityState* entity_state = client.entities.try_get(slot);
    if (entity_state == nullptr) {
        return;
    }
    const QuantizedFrameData* quantized_data = server.quantized_frame_data(quantized_frame_id);
    if (quantized_data == nullptr) {
        return;
    }
    const SyncFrame quantized_frame = server.quantized_frame_frame(quantized_frame_id);
    const SyncArchetypeId quantized_archetype = server.quantized_frame_archetype(quantized_frame_id);
    bool delta = entity_state->baseline != server_detail::invalid_quantized_frame_id &&
        server.quantized_frame_active(entity_state->baseline) &&
        server.quantized_frame_archetype(entity_state->baseline) == quantized_archetype;
    if (delta) {
        const QuantizedFrameData* baseline_data = server.quantized_frame_data(entity_state->baseline);
        delta = baseline_data != nullptr && baseline_data->present_mask == quantized_data->present_mask;
    }

    const std::uint32_t network_id = client.network_id_for(server, slot);
    if (network_id == 0U) {
        return;
    }
    struct ReferenceContextData {
        ReplicationServer* server = nullptr;
        ServerClientReplicator* client = nullptr;
        std::uint32_t source_slot = 0;
    } reference_context_data{&server, &client, slot};
    EntityReferenceContext reference_context;
    bool reference_context_initialized = false;
    auto references_for_component = [&]() -> EntityReferenceContext* {
        if (!reference_context_initialized) {
            reference_context.user = &reference_context_data;
            reference_context.network_entity_id_tier0_bits = server.options().protocol.network_entity_id_tier0_bits;
            reference_context.server_network_id_for_entity = [](void* user, ecs::Entity entity) {
                ReferenceContextData& data = *static_cast<ReferenceContextData*>(user);
                const std::uint32_t reference_slot = data.server->replicated_slot_for_entity(entity);
                if (reference_slot == server_detail::invalid_quantized_frame_id ||
                    data.client->entities.try_get(reference_slot) == nullptr ||
                    !data.server->replicated_slot_active(reference_slot)) {
                    return 0U;
                }
                if (reference_slot != data.source_slot) {
                    data.client->entities.try_get(reference_slot)->reference_priority_boost_pending = true;
                }
                return data.client->network_id_for(*data.server, reference_slot);
            };
            reference_context_initialized = true;
        }
        return &reference_context;
    };

    protocol::write_network_entity_id(out, network_id, server.options().protocol.network_entity_id_tier0_bits);
    out.push_bool(!delta);
    if (!delta) {
        out.push_bits(quantized_archetype.value, 32U);
    } else {
        protocol::write_baseline_frame(out, quantized_frame, server.quantized_frame_frame(entity_state->baseline));
    }

    const SyncArchetype& archetype = settings.archetypes[quantized_archetype.value];
    if (delta) {
        const QuantizedFrameData* baseline_quantized_frame = server.quantized_frame_data(entity_state->baseline);
        if (baseline_quantized_frame == nullptr) {
            return;
        }
        std::uint64_t changed_mask = 0;
        if (has_tag_slot(archetype)) {
            const std::uint64_t tag_bit_mask = archetype.tags.size() == 64U
                ? std::numeric_limits<std::uint64_t>::max()
                : ((std::uint64_t{1} << archetype.tags.size()) - 1U);
            const bool tags_changed =
                (quantized_data->tag_mask & tag_bit_mask) !=
                (baseline_quantized_frame->tag_mask & tag_bit_mask);
            out.push_bool(tags_changed);
            if (tags_changed) {
                changed_mask |= sync_slot_bit(0);
            }
        } else {
            out.push_bool(false);
        }
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            if (!frame_has_component(*quantized_data, component_index) ||
                (component_mask & (std::uint64_t{1} << component_index)) == 0U) {
                out.push_bool(false);
                continue;
            }
            const std::size_t offset = archetype.component_offsets[component_index];
            const std::size_t size = archetype.component_ops[component_index].quantized_size;
            if (offset + size > quantized_data->bytes.size() ||
                offset + size > baseline_quantized_frame->bytes.size()) {
                throw std::logic_error("replicated quantized frame component bytes are out of range");
            }
            const bool component_changed =
                std::memcmp(
                    (*quantized_data).bytes.data() + offset,
                    baseline_quantized_frame->bytes.data() + offset,
                    size) != 0;
            out.push_bool(component_changed);
            if (component_changed) {
                changed_mask |= sync_slot_bit(component_index + 1U);
            }
        }
        if ((changed_mask & sync_slot_bit(0)) != 0U) {
            out.push_unsigned_bits(quantized_data->tag_mask, archetype.tags.size());
#ifdef KAGE_SYNC_ENABLE_TRACING
            if (server.server_tracer() != nullptr && server.server_tracer()->enabled()) {
                for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
                    SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::TagSent, client.id, quantized_frame);
                    event.server_entity = server.replicated_slot_entity(slot);
                    event.wire_network_id = network_id;
                    event.network_version = entity_state->network_version;
                    event.client_network_id = make_client_entity_network_id(client.id, network_id, entity_state->network_version);
                    event.archetype = quantized_archetype;
                    event.tag = archetype.tags[tag_index].tag;
                    event.remove = ((*quantized_data).tag_mask & (std::uint64_t{1} << tag_index)) == 0U;
                    server.server_tracer()->trace(event);
                }
            }
#endif
        }
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            if ((changed_mask & sync_slot_bit(component_index + 1U)) == 0U) {
                continue;
            }
            if (component_index >= archetype.component_ops.size()) {
                throw std::logic_error("sync component traits are not registered for replicated component");
            }
            const SyncComponentOps& ops = archetype.component_ops[component_index];
            const std::uint8_t* previous = frame_component_data(archetype, *baseline_quantized_frame, component_index);
            const std::uint8_t* current = frame_component_data(archetype, (*quantized_data), component_index);
            if (previous == nullptr || current == nullptr) {
                throw std::logic_error("replicated quantized frame component bytes are missing");
            }
#ifdef KAGE_SYNC_ENABLE_TRACING
            if (server.server_tracer() != nullptr && server.server_tracer()->enabled()) {
                SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::ComponentSent, client.id, quantized_frame);
                event.server_entity = server.replicated_slot_entity(slot);
                event.wire_network_id = network_id;
                event.network_version = entity_state->network_version;
                event.client_network_id = make_client_entity_network_id(client.id, network_id, entity_state->network_version);
                event.archetype = quantized_archetype;
                event.component = archetype.components[component_index].component;
                append_trace_component_data(server.server_tracer(), archetype, component_index, current, event);
                server.server_tracer()->trace(event);
            }
#endif
            ops.serialize(previous, current, out, ops.references_entities ? references_for_component() : nullptr);
        }
        const std::size_t cue_count = std::min(entity_state->pending_cues.size(), max_cues_per_entity_record);
        out.push_bool(cue_count != 0U);
        if (cue_count != 0U) {
            out.push_bits(static_cast<std::int64_t>(cue_count), 16U);
            for (std::size_t cue_index = 0; cue_index < cue_count; ++cue_index) {
                const ClientEntityState::PendingCue& cue = entity_state->pending_cues[cue_index];
                out.push_bits(cue.frame, 32U);
                out.push_bits(cue.type, 16U);
                out.push_bytes(reinterpret_cast<const char*>(&cue.relevance_seconds), sizeof(cue.relevance_seconds));
                out.push_bits(static_cast<std::int64_t>(cue.payload.bit_size()), 16U);
                out.push_buffer_bits(cue.payload);
#ifdef KAGE_SYNC_ENABLE_TRACING
                if (server.server_tracer() != nullptr && server.server_tracer()->enabled()) {
                    SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::CueSent, client.id, cue.frame);
                    event.server_entity = server.replicated_slot_entity(slot);
                    event.wire_network_id = network_id;
                    event.network_version = entity_state->network_version;
                    event.client_network_id = make_client_entity_network_id(client.id, network_id, entity_state->network_version);
                    event.archetype = quantized_archetype;
                    event.cue_type = cue.type;
                    append_trace_cue_name(settings, cue.type, event);
                    append_trace_cue_data(server.server_tracer(), settings, cue.type, cue.payload, event);
                    append_trace_data_field(event, "source", "server");
                    server.server_tracer()->trace(event);
                }
#endif
            }
        }
        (void)registry;
        return;
    }

    std::uint16_t component_count = 0;
    std::uint64_t present_sync_slots = 0;
    if (has_tag_slot(archetype)) {
        ++component_count;
        present_sync_slots |= sync_slot_bit(0);
    }
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (frame_has_component((*quantized_data), component_index) &&
            (component_mask & (std::uint64_t{1} << component_index)) != 0U) {
            ++component_count;
            present_sync_slots |= sync_slot_bit(component_index + 1U);
        }
    }

    const std::size_t sync_slots = sync_slot_count(archetype);
    const std::size_t sync_slot_bits = protocol::bits_for_range(sync_slot_count(archetype));
    const std::size_t slot_list_bits = 16U + static_cast<std::size_t>(component_count) * sync_slot_bits;
    const bool use_presence_mask = sync_slots < slot_list_bits;
    out.push_bool(use_presence_mask);
    if (use_presence_mask) {
        out.push_unsigned_bits(present_sync_slots, sync_slots);
    } else {
        out.push_bits(static_cast<std::int64_t>(component_count), 16U);
    }

    if (has_tag_slot(archetype)) {
        if (!use_presence_mask) {
            out.push_bits(0, sync_slot_bits);
        }
        out.push_unsigned_bits((*quantized_data).tag_mask, archetype.tags.size());
#ifdef KAGE_SYNC_ENABLE_TRACING
        if (server.server_tracer() != nullptr && server.server_tracer()->enabled()) {
            for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
                SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::TagSent, client.id, quantized_frame);
                event.server_entity = server.replicated_slot_entity(slot);
                event.wire_network_id = network_id;
                event.network_version = entity_state->network_version;
                event.client_network_id = make_client_entity_network_id(client.id, network_id, entity_state->network_version);
                event.archetype = quantized_archetype;
                event.tag = archetype.tags[tag_index].tag;
                event.remove = ((*quantized_data).tag_mask & (std::uint64_t{1} << tag_index)) == 0U;
                server.server_tracer()->trace(event);
            }
        }
#endif
    }
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (!frame_has_component((*quantized_data), component_index) ||
            (component_mask & (std::uint64_t{1} << component_index)) == 0U) {
            continue;
        }
        if (component_index >= archetype.component_ops.size()) {
            throw std::logic_error("sync component traits are not registered for replicated component");
        }
        const SyncComponentOps& ops = archetype.component_ops[component_index];
        const std::uint8_t* current = frame_component_data(archetype, (*quantized_data), component_index);
        if (current == nullptr) {
            throw std::logic_error("replicated quantized frame component bytes are missing");
        }

        if (!use_presence_mask) {
            out.push_bits(static_cast<std::int64_t>(component_index + 1U), sync_slot_bits);
        }
#ifdef KAGE_SYNC_ENABLE_TRACING
        if (server.server_tracer() != nullptr && server.server_tracer()->enabled()) {
            SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::ComponentSent, client.id, quantized_frame);
            event.server_entity = server.replicated_slot_entity(slot);
            event.wire_network_id = network_id;
            event.network_version = entity_state->network_version;
            event.client_network_id = make_client_entity_network_id(client.id, network_id, entity_state->network_version);
            event.archetype = quantized_archetype;
            event.component = archetype.components[component_index].component;
            append_trace_component_data(server.server_tracer(), archetype, component_index, current, event);
            server.server_tracer()->trace(event);
        }
#endif
        ops.serialize(nullptr, current, out, ops.references_entities ? references_for_component() : nullptr);
    }

    const std::size_t cue_count = std::min(entity_state->pending_cues.size(), max_cues_per_entity_record);
    out.push_bool(cue_count != 0U);
    if (cue_count != 0U) {
        out.push_bits(static_cast<std::int64_t>(cue_count), 16U);
        for (std::size_t cue_index = 0; cue_index < cue_count; ++cue_index) {
            const ClientEntityState::PendingCue& cue = entity_state->pending_cues[cue_index];
            out.push_bits(cue.frame, 32U);
            out.push_bits(cue.type, 16U);
            out.push_bytes(reinterpret_cast<const char*>(&cue.relevance_seconds), sizeof(cue.relevance_seconds));
            out.push_bits(static_cast<std::int64_t>(cue.payload.bit_size()), 16U);
            out.push_buffer_bits(cue.payload);
#ifdef KAGE_SYNC_ENABLE_TRACING
            if (server.server_tracer() != nullptr && server.server_tracer()->enabled()) {
                SyncTraceEvent event = make_server_trace_event(SyncTraceEventType::CueSent, client.id, cue.frame);
                event.server_entity = server.replicated_slot_entity(slot);
                event.wire_network_id = network_id;
                event.network_version = entity_state->network_version;
                event.client_network_id = make_client_entity_network_id(client.id, network_id, entity_state->network_version);
                event.archetype = quantized_archetype;
                event.cue_type = cue.type;
                append_trace_cue_name(settings, cue.type, event);
                append_trace_cue_data(server.server_tracer(), settings, cue.type, cue.payload, event);
                append_trace_data_field(event, "source", "server");
                server.server_tracer()->trace(event);
            }
#endif
        }
    }

    (void)registry;
}

bool ReplicationServer::valid_archetype(const ecs::Registry& registry, SyncArchetypeId archetype) const {
    const SyncSettings& settings = registry.get<SyncSettings>();
    return archetype.value < settings.archetypes.size();
}

bool ReplicationServer::upsert_replicated(ecs::Registry& registry, ecs::Entity entity, SyncArchetypeId archetype) {
    if (!registry.alive(entity) || !valid_archetype(registry, archetype)) {
        deactivate_entity_index(ecs::Registry::entity_index(entity));
        return false;
    }

    const EntityKey key = entity.value;
    const auto found = entity_to_replicated_index_.find(key);
    if (found != entity_to_replicated_index_.end()) {
        replicated_[found->second].archetype = archetype;
        replicated_[found->second].same_frame_quantized_frame = invalid_quantized_frame_id;
        replicated_[found->second].same_frame_quantized_frame_frame = 0;
        const SyncSettings& settings = registry.get<SyncSettings>();
        if (archetype.value < settings.archetypes.size()) {
            replicated_[found->second].same_frame_cacheable =
                archetype_is_same_frame_cacheable(settings.archetypes[archetype.value]);
            replicated_[found->second].component_dirty_generations.assign(
                sync_slot_count(settings.archetypes[archetype.value]),
                1U);
        }
        for (ClientState& client : clients_) {
            if (client.replication == nullptr) {
                continue;
            }
            ServerClientReplicator& replication = *client.replication;
            replication.entities.clear_preserving_network_identity(*this, found->second);
            replication.mark_dirty(*this, found->second, frame_);
            if (found->second < replication.dirty_queue.entries.size()) {
                replication.dirty_queue.entries[found->second].baseline_frame = 0;
            }
        }
        return true;
    }

    deactivate_entity_index(ecs::Registry::entity_index(entity));

    const std::uint32_t slot = allocate_replicated_slot(entity, archetype);
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (archetype.value < settings.archetypes.size()) {
        replicated_[slot].same_frame_cacheable =
            archetype_is_same_frame_cacheable(settings.archetypes[archetype.value]);
        replicated_[slot].component_dirty_generations.assign(
            sync_slot_count(settings.archetypes[archetype.value]),
            1U);
    }
    entity_to_replicated_index_[key] = slot;
    entity_index_to_replicated_index_[ecs::Registry::entity_index(entity)] = slot;
    for (ClientState& client : clients_) {
        if (client.replication == nullptr) {
            continue;
        }
        ServerClientReplicator& replication = *client.replication;
        replication.ensure_capacity(replicated_.size());
        replication.entities.clear(*this, slot);
        replication.mark_dirty(*this, slot, frame_);
    }

    ++active_replicated_count_;
    return true;
}

std::uint32_t ReplicationServer::allocate_replicated_slot(ecs::Entity entity, SyncArchetypeId archetype) {
    if (!free_replicated_indices_.empty()) {
        const std::uint32_t slot = free_replicated_indices_.back();
        free_replicated_indices_.pop_back();
        replicated_[slot] =
            ReplicatedSlot{entity, archetype, {}, {}, invalid_quantized_frame_id, 0, false, true};
        return slot;
    }

    if (replicated_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::length_error("kage sync replicated slot space exhausted");
    }

    const std::uint32_t slot = static_cast<std::uint32_t>(replicated_.size());
    replicated_.push_back(
        ReplicatedSlot{entity, archetype, {}, {}, invalid_quantized_frame_id, 0, false, true});
    return slot;
}

void ReplicationServer::deactivate_replicated(std::uint32_t slot) {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return;
    }

    const ecs::Entity entity = replicated_[slot].entity;
    entity_to_replicated_index_.erase(entity.value);
    entity_index_to_replicated_index_.erase(ecs::Registry::entity_index(entity));
    replicated_[slot].active = false;
    replicated_[slot].quantized_frames.clear();
    replicated_[slot].same_frame_quantized_frame = invalid_quantized_frame_id;
    replicated_[slot].same_frame_quantized_frame_frame = 0;
    replicated_[slot].same_frame_cacheable = false;
    free_replicated_indices_.push_back(slot);
    --active_replicated_count_;
    for (ClientState& client_state : clients_) {
        if (client_state.replication == nullptr) {
            continue;
        }
        ServerClientReplicator& client = *client_state.replication;
        client.enqueue_destroy(*this, slot, entity, frame_);
    }
    remove_replicated_from_client_replicators(slot);
}

void ReplicationServer::deactivate_entity_index(std::uint32_t entity_index) {
    const auto found = entity_index_to_replicated_index_.find(entity_index);
    if (found == entity_index_to_replicated_index_.end()) {
        return;
    }

    deactivate_replicated(found->second);
}

void ReplicationServer::remove_replicated_from_client_replicators(std::uint32_t slot) {
    for (ClientState& client : clients_) {
        if (client.replication != nullptr) {
            client.replication->clear_dirty(slot);
        }
    }
}

bool ReplicationServer::replicated_is_replicable(const ecs::Registry& registry, std::uint32_t slot) const {
    if (slot >= replicated_.size() || !replicated_[slot].active) {
        return false;
    }

    const ecs::Entity entity = replicated_[slot].entity;
    return registry.alive(entity) && registry.contains<Replicated>(entity);
}

}  // namespace kage::sync
