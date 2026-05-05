#include "kage/sync/client.hpp"

#include "client/ack_queue.hpp"
#include "client/frame_data.hpp"
#include "client/input_buffer.hpp"
#include "client/packet_window.hpp"
#include "client/state.hpp"
#include "detail/options_validation.hpp"

#include "kage/sync/protocol.hpp"
#include "kage/sync/tracing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace kage::sync {
namespace {

constexpr std::size_t max_baseline_history_per_entity = 64;
constexpr std::size_t max_destroy_tombstones = 65536;

using client_detail::apply_archetype_tags;
using client_detail::frame_component_data;
using client_detail::frame_has_component;
using client_detail::has_tag_slot;
using client_detail::init_frame_data;
using client_detail::mutable_frame_component_data;
using client_detail::remove_archetype_tags;
using client_detail::sync_slot_bit;
using client_detail::sync_slot_count;
using client_detail::tag_bit_set;
using client_detail::unchecked_frame_component_data;
using client_detail::unchecked_mutable_frame_component_data;

std::size_t configured_packet_id_bits(const ReplicationClientOptions& options) noexcept {
    return protocol::packet_id_bits(options.protocol);
}

ReplicationClientClockConfig make_clock_config(const ReplicationClientOptions& options) noexcept {
    ReplicationClientClockConfig config;
    config.fixed_dt_seconds = options.fixed_dt_seconds;
    config.interpolation_buffer_frames = options.interpolation_buffer_frames;
    config.interpolation_buffer_capacity_frames = options.interpolation_buffer_capacity_frames;
    config.auto_interpolation_buffer_frames = options.auto_interpolation_buffer_frames;
    config.auto_interpolation_min_frames = options.auto_interpolation_min_frames;
    config.auto_interpolation_jitter_multiplier = options.auto_interpolation_jitter_multiplier;
    config.auto_interpolation_smoothing = options.auto_interpolation_smoothing;
    config.auto_interpolation_time_dilation_min = options.auto_interpolation_time_dilation_min;
    config.auto_interpolation_time_dilation_max = options.auto_interpolation_time_dilation_max;
    config.auto_interpolation_time_dilation_gain = options.auto_interpolation_time_dilation_gain;
    config.auto_prediction_lead_frames = options.auto_prediction_lead_frames;
    config.prediction_lead_frames = options.prediction_lead_frames;
    config.input_buffer_capacity_frames = options.input_buffer_capacity_frames;
    config.auto_prediction_min_frames = options.auto_prediction_min_frames;
    config.auto_prediction_safety_frames = options.auto_prediction_safety_frames;
    config.auto_prediction_jitter_multiplier = options.auto_prediction_jitter_multiplier;
    config.auto_prediction_time_dilation_min = options.auto_prediction_time_dilation_min;
    config.auto_prediction_time_dilation_max = options.auto_prediction_time_dilation_max;
    config.auto_prediction_time_dilation_gain = options.auto_prediction_time_dilation_gain;
    config.auto_timing_warmup_samples = options.auto_timing_warmup_samples;
    config.auto_timing_fast_recovery = options.auto_timing_fast_recovery;
    config.auto_timing_fast_recovery_min_frame_gap = options.auto_timing_fast_recovery_min_frame_gap;
    return config;
}

bool all_zero(const SyncComponentOps::QuantizedBytes& bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) {
        return byte == 0U;
    });
}

std::uint16_t encode_subframe(double subframe) noexcept {
    if (!std::isfinite(subframe)) {
        return 0;
    }
    const auto scaled = static_cast<std::uint32_t>(std::floor(
        std::clamp(subframe, 0.0, 1.0) * static_cast<double>(protocol::frame_subframe_scale)));
    return static_cast<std::uint16_t>(std::min(scaled, protocol::frame_subframe_scale - 1U));
}

double decode_continuous_frame(SyncFrame frame, std::uint16_t subframe) noexcept {
    return static_cast<double>(frame) +
        static_cast<double>(subframe) / static_cast<double>(protocol::frame_subframe_scale);
}

#ifdef KAGE_SYNC_ENABLE_TRACING
SyncTraceEvent make_client_trace_event(SyncTraceEventType type, ClientId client, SyncFrame frame) {
    SyncTraceEvent event;
    event.type = type;
    event.role = SyncTraceRole::Client;
    event.client = client;
    event.frame = frame;
    return event;
}

struct RollbackReasonTraceContext {
    const SyncTracer* tracer = nullptr;
    ClientId client = invalid_client_id;
    SyncFrame frame = 0;
    ecs::Entity server_entity{};
    ecs::Entity local_entity{};
    ClientEntityNetworkId client_network_id = invalid_client_entity_network_id;
    std::uint32_t wire_network_id = 0;
    std::uint32_t network_version = 0;
    SyncArchetypeId archetype = invalid_sync_archetype_id;
    ecs::Entity component{};
    std::string component_name;
};

thread_local const RollbackReasonTraceContext* rollback_reason_trace_context = nullptr;

class ScopedRollbackReasonTraceContext {
public:
    explicit ScopedRollbackReasonTraceContext(const RollbackReasonTraceContext& context)
        : previous_(rollback_reason_trace_context) {
        rollback_reason_trace_context = &context;
    }

    ~ScopedRollbackReasonTraceContext() {
        rollback_reason_trace_context = previous_;
    }

    ScopedRollbackReasonTraceContext(const ScopedRollbackReasonTraceContext&) = delete;
    ScopedRollbackReasonTraceContext& operator=(const ScopedRollbackReasonTraceContext&) = delete;

private:
    const RollbackReasonTraceContext* previous_ = nullptr;
};

void emit_rollback_reason(const std::string& reason) {
    const RollbackReasonTraceContext* context = rollback_reason_trace_context;
    if (context == nullptr || context->tracer == nullptr || !context->tracer->enabled() || reason.empty()) {
        return;
    }
    SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::RollbackReason, context->client, context->frame);
    event.server_entity = context->server_entity;
    event.local_entity = context->local_entity;
    event.client_network_id = context->client_network_id;
    event.wire_network_id = context->wire_network_id;
    event.network_version = context->network_version;
    event.archetype = context->archetype;
    event.component = context->component;
    event.component_name = context->component_name;
    event.data = reason;
    context->tracer->trace(event);
}

}  // namespace

void trace_rollback_reason(const char* reason) {
    emit_rollback_reason(reason != nullptr ? std::string(reason) : std::string{});
}

void trace_rollback_reason(const std::string& reason) {
    emit_rollback_reason(reason);
}

namespace {

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

void append_trace_component_name(
    const SyncArchetype& archetype,
    std::size_t component_index,
    SyncTraceEvent& event) {
    if (component_index < archetype.component_ops.size()) {
        event.component_name = archetype.component_ops[component_index].name;
    }
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

}  // namespace

bool ReplicatedEntityUpdateView::try_get(
    const ecs::Registry& registry,
    ecs::Entity component,
    void* out) const {
    if (components == nullptr || out == nullptr) {
        return false;
    }

    const auto found = std::find_if(
        components->begin(),
        components->end(),
        [component](const ReplicatedComponentUpdate& update) {
            return update.component == component;
        });
    if (found == components->end()) {
        return false;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    const auto found_ops = settings.component_ops.find(component.value);
    if (found_ops == settings.component_ops.end() || found_ops->second.dequantize == nullptr) {
        return false;
    }

    if (found->bytes.size() != found_ops->second.quantized_size) {
        return false;
    }
    found_ops->second.dequantize(found->bytes.data(), out);
    return true;
}

bool ReplicatedEntityUpdateView::has_tag(const ecs::Registry& registry, ecs::Entity tag) const {
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (archetype.value >= settings.archetypes.size()) {
        return false;
    }
    const SyncArchetype& definition = settings.archetypes[archetype.value];
    for (std::size_t tag_index = 0; tag_index < definition.tags.size(); ++tag_index) {
        if (definition.tags[tag_index].tag == tag) {
            return tag_bit_set(tag_mask, tag_index);
        }
    }
    return false;
}

bool FractionalTickSample::try_get_sampled_value(
    const ecs::Registry& registry,
    ecs::Entity component,
    void* out) const {
    if (out == nullptr) {
        return false;
    }
    if (!registry.has<FractionalTickSampled>(component)) {
        throw std::logic_error("fractional tick sample component is not marked fractional-tick-sampled");
    }

    const auto found = std::find_if(
        components_.begin(),
        components_.end(),
        [component](const ReplicatedComponentUpdate& update) {
            return update.component == component;
        });
    if (found == components_.end()) {
        return false;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    const auto found_ops = settings.component_ops.find(component.value);
    if (found_ops == settings.component_ops.end() || found_ops->second.dequantize == nullptr) {
        return false;
    }

    if (found->bytes.size() != found_ops->second.quantized_size) {
        return false;
    }
    found_ops->second.dequantize(found->bytes.data(), out);
    return true;
}

ReplicationClient::ReplicationClient(ReplicationClientOptions options)
    : options_(detail::validate_client_options(std::move(options))),
      clock_(make_clock_config(options_)),
      ack_queue_(std::make_unique<client_detail::ClientAckQueue>()),
      input_(std::make_unique<client_detail::ClientInputBuffer>()) {
#ifdef KAGE_SYNC_ENABLE_TRACING
    set_trace_options(options_.trace);
#endif
    adaptive_ping_active_ = options_.adaptive_ping_interval;
    if (options_.connect_token.empty()) {
        connection_state_ = ReplicationClientConnectionState::Ready;
        (void)clock_.bootstrap_from_server_update(0, false, true);
    }
}

ReplicationClient::~ReplicationClient() = default;

ReplicationClient::ReplicationClient(ReplicationClient&& other) noexcept = default;

ReplicationClient& ReplicationClient::operator=(ReplicationClient&& other) noexcept = default;

ReplicationClient::EntityState* ReplicationClient::find_entity_state(ClientEntityNetworkId network_id) noexcept {
    const auto found = network_entity_indices_.find(network_id);
    if (found == network_entity_indices_.end() || found->second >= entities_.size()) {
        return nullptr;
    }
    EntityState& state = entities_[found->second];
    return state.client_entity_network_id == network_id ? &state : nullptr;
}

const ReplicationClient::EntityState* ReplicationClient::find_entity_state(
    ClientEntityNetworkId network_id) const noexcept {
    const auto found = network_entity_indices_.find(network_id);
    if (found == network_entity_indices_.end() || found->second >= entities_.size()) {
        return nullptr;
    }
    const EntityState& state = entities_[found->second];
    return state.client_entity_network_id == network_id ? &state : nullptr;
}

ReplicationClient::EntityState* ReplicationClient::find_entity_state(std::uint32_t network_id) noexcept {
    if (network_id == 0U || network_id >= wire_network_ids_.size()) {
        return nullptr;
    }
    const std::uint32_t entity_index = wire_network_ids_[network_id].entity_index;
    if (entity_index >= entities_.size()) {
        return nullptr;
    }
    EntityState& state = entities_[entity_index];
    return state.wire_network_id == network_id ? &state : nullptr;
}

const ReplicationClient::EntityState* ReplicationClient::find_entity_state(std::uint32_t network_id) const noexcept {
    if (network_id == 0U || network_id >= wire_network_ids_.size()) {
        return nullptr;
    }
    const std::uint32_t entity_index = wire_network_ids_[network_id].entity_index;
    if (entity_index >= entities_.size()) {
        return nullptr;
    }
    const EntityState& state = entities_[entity_index];
    return state.wire_network_id == network_id ? &state : nullptr;
}

ReplicationClient::EntityState* ReplicationClient::find_entity_state_for_local(ecs::Entity local) noexcept {
    if (!local) {
        return nullptr;
    }
    for (EntityState& state : entities_) {
        if (state.local == local) {
            return &state;
        }
    }
    return nullptr;
}

ReplicationClient::EntityState* ReplicationClient::ensure_entity_state(
    ecs::Registry& registry,
    ClientEntityNetworkId network_id,
    std::uint32_t wire_network_id) {
    (void)registry;
    if (network_id == invalid_client_entity_network_id || wire_network_id == 0U) {
        return nullptr;
    }

    if (EntityState* existing = find_entity_state(network_id)) {
        return existing;
    }

    std::uint32_t entity_index = invalid_entity_index;
    if (!free_entity_indices_.empty()) {
        entity_index = free_entity_indices_.back();
        free_entity_indices_.pop_back();
    } else {
        if (entities_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::length_error("kage sync client entity state space exhausted");
        }
        entity_index = static_cast<std::uint32_t>(entities_.size());
        entities_.emplace_back();
    }
    EntityState& fresh = entities_[entity_index];
    fresh = EntityState{};
    fresh.client_entity_network_id = network_id;
    fresh.wire_network_id = wire_network_id;
    fresh.active_index = active_entities_.size();
    active_entities_.push_back(entity_index);
    network_entity_indices_[network_id] = entity_index;
    if (wire_network_id >= wire_network_ids_.size()) {
        wire_network_ids_.resize(static_cast<std::size_t>(wire_network_id) + 1U);
    }
    wire_network_ids_[wire_network_id].entity_index = entity_index;
    wire_network_ids_[wire_network_id].alive = true;
    return &fresh;
}

void ReplicationClient::erase_entity_state(
    ecs::Registry& registry,
    std::uint32_t entity_index,
    bool destroy_local) {
    if (entity_index >= entities_.size()) {
        return;
    }

    EntityState& state = entities_[entity_index];
    if (state.client_entity_network_id == invalid_client_entity_network_id) {
        return;
    }
    const auto found = network_entity_indices_.find(state.client_entity_network_id);
    if (found != network_entity_indices_.end() && found->second == entity_index) {
        network_entity_indices_.erase(found);
    }
    if (state.wire_network_id < wire_network_ids_.size() &&
        wire_network_ids_[state.wire_network_id].entity_index == entity_index) {
        wire_network_ids_[state.wire_network_id].entity_index = invalid_entity_index;
        wire_network_ids_[state.wire_network_id].alive = false;
    }

    if (destroy_local && state.local && registry.alive(state.local)) {
        registry.destroy(state.local);
    }

    set_buffered_membership(entity_index, false);
    set_snap_error_membership(entity_index, false);
    set_prediction_rollback_membership(entity_index, false);
    if (state.active_index != invalid_ack_index && state.active_index < active_entities_.size()) {
        const std::uint32_t moved = active_entities_.back();
        active_entities_[state.active_index] = moved;
        entities_[moved].active_index = state.active_index;
        active_entities_.pop_back();
    }

    state = EntityState{};
    free_entity_indices_.push_back(entity_index);
}

void ReplicationClient::set_buffered_membership(std::uint32_t entity_index, bool active) {
    EntityState& state = entities_[entity_index];
    if (active) {
        if (state.buffered_index == invalid_ack_index) {
            state.buffered_index = buffered_entities_.size();
            buffered_entities_.push_back(entity_index);
        }
        return;
    }

    if (state.buffered_index == invalid_ack_index || state.buffered_index >= buffered_entities_.size()) {
        state.buffered_index = invalid_ack_index;
        return;
    }

    const std::uint32_t moved = buffered_entities_.back();
    buffered_entities_[state.buffered_index] = moved;
    entities_[moved].buffered_index = state.buffered_index;
    buffered_entities_.pop_back();
    state.buffered_index = invalid_ack_index;
}

void ReplicationClient::set_snap_error_membership(std::uint32_t entity_index, bool active) {
    EntityState& state = entities_[entity_index];
    if (active) {
        if (state.snap_error_index == invalid_ack_index) {
            state.snap_error_index = snap_error_entities_.size();
            snap_error_entities_.push_back(entity_index);
        }
        return;
    }

    if (state.snap_error_index == invalid_ack_index || state.snap_error_index >= snap_error_entities_.size()) {
        state.snap_error_index = invalid_ack_index;
        return;
    }

    const std::uint32_t moved = snap_error_entities_.back();
    snap_error_entities_[state.snap_error_index] = moved;
    entities_[moved].snap_error_index = state.snap_error_index;
    snap_error_entities_.pop_back();
    state.snap_error_index = invalid_ack_index;
}

void ReplicationClient::set_prediction_rollback_membership(std::uint32_t entity_index, bool active) {
    EntityState& state = entities_[entity_index];
    if (active) {
        if (state.prediction_rollback_index == invalid_ack_index) {
            state.prediction_rollback_index = prediction_rollback_entities_.size();
            prediction_rollback_entities_.push_back(entity_index);
        }
        return;
    }

    if (state.prediction_rollback_index == invalid_ack_index ||
        state.prediction_rollback_index >= prediction_rollback_entities_.size()) {
        state.prediction_rollback_index = invalid_ack_index;
        return;
    }

    const std::uint32_t moved = prediction_rollback_entities_.back();
    prediction_rollback_entities_[state.prediction_rollback_index] = moved;
    entities_[moved].prediction_rollback_index = state.prediction_rollback_index;
    prediction_rollback_entities_.pop_back();
    state.prediction_rollback_index = invalid_ack_index;
}

void ReplicationClient::sync_entity_memberships(EntityState& state) {
    if (state.client_entity_network_id == invalid_client_entity_network_id) {
        return;
    }
    const auto found = network_entity_indices_.find(state.client_entity_network_id);
    if (found == network_entity_indices_.end()) {
        return;
    }
    const std::uint32_t entity_index = found->second;
    set_buffered_membership(entity_index, state.mode == ReplicationClientMode::BufferedInterpolation);
    set_snap_error_membership(
        entity_index,
        (state.mode == ReplicationClientMode::Snap || state.mode == ReplicationClientMode::Predict) &&
            !state.snap_errors.empty());
}

bool ReplicationClient::destroy_tombstone_blocks(std::uint32_t wire_network_id, SyncFrame frame) const {
    const auto found = destroy_tombstones_.find(wire_network_id);
    return found != destroy_tombstones_.end() && frame <= found->second.frame;
}

void ReplicationClient::record_destroy_tombstone(std::uint32_t wire_network_id, SyncFrame frame) {
    if (wire_network_id == 0U) {
        return;
    }

    auto [found, inserted] = destroy_tombstones_.try_emplace(wire_network_id);
    if (!inserted && frame <= found->second.frame) {
        return;
    }
    if (inserted) {
        found->second.frame = frame;
        found->second.age_order = destroy_tombstone_next_age_order_++;
    } else {
        found->second.frame = frame;
        ++found->second.generation;
        found->second.age_order = destroy_tombstone_next_age_order_++;
    }
    destroy_tombstone_ages_.push_back(
        DestroyTombstoneAgeEntry{wire_network_id, found->second.generation, found->second.age_order});
    compact_destroy_tombstone_ages();
    if (destroy_tombstones_.size() <= max_destroy_tombstones) {
        return;
    }

    while (destroy_tombstones_.size() > max_destroy_tombstones &&
           destroy_tombstone_age_begin_ < destroy_tombstone_ages_.size()) {
        const DestroyTombstoneAgeEntry age = destroy_tombstone_ages_[destroy_tombstone_age_begin_++];
        const auto erase = destroy_tombstones_.find(age.wire_network_id);
        if (erase == destroy_tombstones_.end() ||
            erase->second.generation != age.generation ||
            erase->second.age_order != age.age_order) {
            continue;
        }
        destroy_tombstones_.erase(erase);
    }
    compact_destroy_tombstone_ages();
}

void ReplicationClient::compact_destroy_tombstone_ages() {
    if (destroy_tombstone_age_begin_ == destroy_tombstone_ages_.size()) {
        destroy_tombstone_ages_.clear();
        destroy_tombstone_age_begin_ = 0;
        return;
    }
    if (destroy_tombstone_age_begin_ != 0U &&
        destroy_tombstone_age_begin_ >= 4096U &&
        destroy_tombstone_age_begin_ * 2U >= destroy_tombstone_ages_.size()) {
        destroy_tombstone_ages_.erase(
            destroy_tombstone_ages_.begin(),
            destroy_tombstone_ages_.begin() + static_cast<std::ptrdiff_t>(destroy_tombstone_age_begin_));
        destroy_tombstone_age_begin_ = 0;
    }
    if (destroy_tombstone_ages_.size() <= max_destroy_tombstones * 2U) {
        return;
    }
    destroy_tombstone_ages_.clear();
    destroy_tombstone_ages_.reserve(destroy_tombstones_.size());
    for (const auto& tombstone : destroy_tombstones_) {
        destroy_tombstone_ages_.push_back(DestroyTombstoneAgeEntry{
            tombstone.first,
            tombstone.second.generation,
            tombstone.second.age_order});
    }
    std::sort(
        destroy_tombstone_ages_.begin(),
        destroy_tombstone_ages_.end(),
        [](const DestroyTombstoneAgeEntry& lhs, const DestroyTombstoneAgeEntry& rhs) {
            return lhs.age_order < rhs.age_order;
        });
    destroy_tombstone_age_begin_ = 0;
}

const QuantizedFrameData* ReplicationClient::find_baseline(
    const EntityState& state,
    SyncFrame frame) const noexcept {
    if (state.history.empty()) {
        return nullptr;
    }

    const std::size_t count = state.history.size();
    if (count == max_baseline_history_per_entity) {
        const client_detail::EntityFrameBaseline& baseline =
            state.history[frame & (max_baseline_history_per_entity - 1U)];
        return baseline.valid && baseline.frame == frame ? &baseline.baseline : nullptr;
    }

    for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t index = (state.history_next + count - 1U - offset) % count;
        const client_detail::EntityFrameBaseline& baseline = state.history[index];
        if (baseline.valid && baseline.frame == frame) {
            return &baseline.baseline;
        }
    }
    return nullptr;
}

bool ReplicationClient::receive(ecs::Registry& registry, ecs::BitBuffer packet) {
    try {
        if (packet.remaining_bits() < 8U) {
            return false;
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message == protocol::server_connect_response_message) {
            return receive_connect_response(registry, packet);
        }
        if (message == protocol::server_pong_message) {
            return receive_pong(registry, packet, clock_.receive_frame());
        }
        if (message != protocol::server_update_message) {
            return false;
        }
        const auto frame = static_cast<SyncFrame>(packet.read_bits(32U));
        const auto packet_id = static_cast<std::uint32_t>(packet.read_bits(configured_packet_id_bits(options_)));
        const auto input_ack_frame = static_cast<SyncFrame>(packet.read_bits(32U));
        const auto record_count = static_cast<std::uint16_t>(packet.read_bits(16U));
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
        if (client_id_ == invalid_client_id) {
            const SyncSettings& settings = registry.get<SyncSettings>();
            if (settings.local_client != invalid_client_id) {
                client_id_ = settings.local_client;
            }
        }
        current_packet_cue_summaries_.clear();
#endif
        acknowledge_input_frame(input_ack_frame);
        const bool applied = apply_update(registry, packet, packet_id, frame, record_count);
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
        trace_incoming_update_packet(clock_.receive_frame(), frame, packet_id, input_ack_frame, record_count);
#endif
        if (applied) {
            connection_state_ = ReplicationClientConnectionState::Ready;
            last_server_update_frame_ = frame;
            last_server_update_receive_frame_ = clock_.continuous_receive_frame();
            has_received_server_update_ = true;
            if (clock_.bootstrap_from_server_update(frame)) {
                const SyncSettings& settings = registry.get<SyncSettings>();
                (void)fill_input_frames_through(registry, settings, clock_.input_frame());
            }
            record_server_packet_sequence(packet_id);
            const SyncFrame decision_last_recorded_input_frame = input_->last_recorded_frame();
            SyncFrame prefill_input_frame =
                clock_.record_server_update(
                    frame,
                    clock_.continuous_receive_frame(),
                    clock_.continuous_playback_frame(),
                    clock_.continuous_input_frame(),
                    has_buffered_entities(),
                    decision_last_recorded_input_frame);
#ifdef KAGE_SYNC_ENABLE_TRACING
            const bool clock_requested_prefill = prefill_input_frame != 0U;
#endif
            if (prefill_input_frame != 0U) {
                active_prediction_snap_lead_frames_ = clock_.stats().target_prediction_lead_frames;
            } else if (active_prediction_snap_lead_frames_ != 0U) {
                if (clock_.stats().target_prediction_lead_frames < active_prediction_snap_lead_frames_) {
                    active_prediction_snap_lead_frames_ = 0;
                } else if (clock_.stats().current_prediction_lead_frames < active_prediction_snap_lead_frames_) {
                    prefill_input_frame = frame + active_prediction_snap_lead_frames_;
                }
            }
#ifdef KAGE_SYNC_ENABLE_TRACING
            trace_clock_skew(
                clock_requested_prefill ? "clock_requested_prefill" :
                    (prefill_input_frame != 0U ? "active_snap_topup" : "no_prefill"),
                clock_.receive_frame(),
                frame,
                clock_.receive_frame(),
                clock_.playback_frame(),
                decision_last_recorded_input_frame,
                prefill_input_frame);
#endif
            if (prefill_input_frame > input_->last_recorded_frame()) {
                const SyncSettings& settings = registry.get<SyncSettings>();
                (void)fill_input_frames_through(registry, settings, prefill_input_frame);
                if (prefill_input_frame > pending_prediction_catchup_frame_) {
                    pending_prediction_catchup_frame_ = prefill_input_frame;
                    pending_prediction_catchup_server_frame_ = frame;
                }
                if (!has_predicted_entities()) {
                    clock_.advance_input_frame_to(prefill_input_frame);
                    clock_.record_prediction_lead(frame, prefill_input_frame);
                } else {
                    clock_.record_prediction_lead(frame, prefill_input_frame);
                }
#ifdef KAGE_SYNC_ENABLE_TRACING
                trace_clock_skew(
                    "prefill_applied",
                    clock_.receive_frame(),
                    frame,
                    clock_.receive_frame(),
                    clock_.playback_frame(),
                    decision_last_recorded_input_frame,
                    prefill_input_frame);
#endif
            }
        }
        return applied;
    } catch (const std::out_of_range&) {
        return false;
    }
}

bool ReplicationClient::receive(ecs::Registry& registry, ecs::BitBuffer packet, SyncFrame client_frame) {
    return receive(registry, std::move(packet), client_frame, client_frame);
}

bool ReplicationClient::receive(
    ecs::Registry& registry,
    ecs::BitBuffer packet,
    SyncFrame receive_frame,
    SyncFrame playback_frame) {
    try {
        clock_.advance_receive_frame_to(receive_frame);
        if (packet.remaining_bits() < 8U) {
            return false;
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message == protocol::server_connect_response_message) {
            return receive_connect_response(registry, packet);
        }
        if (message == protocol::server_pong_message) {
            return receive_pong(registry, packet, receive_frame);
        }
        if (message != protocol::server_update_message) {
            return false;
        }
        const auto frame = static_cast<SyncFrame>(packet.read_bits(32U));
        const auto packet_id = static_cast<std::uint32_t>(packet.read_bits(configured_packet_id_bits(options_)));
        const auto input_ack_frame = static_cast<SyncFrame>(packet.read_bits(32U));
        const auto record_count = static_cast<std::uint16_t>(packet.read_bits(16U));
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
        if (client_id_ == invalid_client_id) {
            const SyncSettings& settings = registry.get<SyncSettings>();
            if (settings.local_client != invalid_client_id) {
                client_id_ = settings.local_client;
            }
        }
        current_packet_cue_summaries_.clear();
#endif
        acknowledge_input_frame(input_ack_frame);
        const bool applied = apply_update(registry, packet, packet_id, frame, record_count);
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
        trace_incoming_update_packet(receive_frame, frame, packet_id, input_ack_frame, record_count);
#endif
        if (applied) {
            connection_state_ = ReplicationClientConnectionState::Ready;
            last_server_update_frame_ = frame;
            last_server_update_receive_frame_ = static_cast<double>(receive_frame);
            has_received_server_update_ = true;
            if (clock_.bootstrap_from_server_update(frame)) {
                const SyncSettings& settings = registry.get<SyncSettings>();
                (void)fill_input_frames_through(registry, settings, clock_.input_frame());
            }
            record_server_packet_sequence(packet_id);
            const SyncFrame decision_last_recorded_input_frame = input_->last_recorded_frame();
            SyncFrame prefill_input_frame = clock_.record_server_update(
                frame,
                static_cast<double>(receive_frame),
                static_cast<double>(playback_frame),
                static_cast<double>(decision_last_recorded_input_frame),
                has_buffered_entities());
#ifdef KAGE_SYNC_ENABLE_TRACING
            const bool clock_requested_prefill = prefill_input_frame != 0U;
#endif
            if (prefill_input_frame != 0U) {
                active_prediction_snap_lead_frames_ = clock_.stats().target_prediction_lead_frames;
            } else if (active_prediction_snap_lead_frames_ != 0U) {
                if (clock_.stats().target_prediction_lead_frames < active_prediction_snap_lead_frames_) {
                    active_prediction_snap_lead_frames_ = 0;
                } else if (clock_.stats().current_prediction_lead_frames < active_prediction_snap_lead_frames_) {
                    prefill_input_frame = frame + active_prediction_snap_lead_frames_;
                }
            }
#ifdef KAGE_SYNC_ENABLE_TRACING
            trace_clock_skew(
                clock_requested_prefill ? "clock_requested_prefill" :
                    (prefill_input_frame != 0U ? "active_snap_topup" : "no_prefill"),
                receive_frame,
                frame,
                receive_frame,
                playback_frame,
                decision_last_recorded_input_frame,
                prefill_input_frame);
#endif
            if (prefill_input_frame > input_->last_recorded_frame()) {
                const SyncSettings& settings = registry.get<SyncSettings>();
                (void)fill_input_frames_through(registry, settings, prefill_input_frame);
                if (prefill_input_frame > pending_prediction_catchup_frame_) {
                    pending_prediction_catchup_frame_ = prefill_input_frame;
                    pending_prediction_catchup_server_frame_ = frame;
                }
                if (!has_predicted_entities()) {
                    clock_.advance_input_frame_to(prefill_input_frame);
                    clock_.record_prediction_lead(frame, prefill_input_frame);
                } else {
                    clock_.record_prediction_lead(frame, prefill_input_frame);
                }
#ifdef KAGE_SYNC_ENABLE_TRACING
                trace_clock_skew(
                    "prefill_applied",
                    receive_frame,
                    frame,
                    receive_frame,
                    playback_frame,
                    decision_last_recorded_input_frame,
                    prefill_input_frame);
#endif
            }
        }
        return applied;
    } catch (const std::out_of_range&) {
        return false;
    }
}

#ifdef KAGE_SYNC_ENABLE_TRACING
void ReplicationClient::set_tracer(SyncTracer* tracer) noexcept {
    trace_writer_.reset();
    tracer_ = tracer;
}

void ReplicationClient::set_trace_options(TraceOptions options) {
    options_.trace = std::move(options);
    trace_writer_ = make_trace_writer(options_.trace);
    tracer_ = trace_writer_ != nullptr ? &trace_writer_->tracer() : nullptr;
}

void ReplicationClient::flush_trace() {
    if (trace_writer_ != nullptr) {
        trace_writer_->flush();
    }
}

void ReplicationClient::close_trace() {
    if (trace_writer_ != nullptr) {
        trace_writer_->close();
    }
}
#endif

bool ReplicationClient::set_default_entity_mode(ReplicationClientMode mode) noexcept {
    options_.default_entity_mode = mode;
    return true;
}

void ReplicationClient::set_entity_mode(
    ecs::Registry& registry,
    ClientEntityNetworkId network_id,
    ReplicationClientMode mode) {
    EntityState* state = find_entity_state(network_id);
    if (state == nullptr) {
        throw ClientError(ClientStatus::EntityNotFound, "client entity network id is not alive");
    }

    if (!state->entity_present && !state->local) {
        throw ClientError(ClientStatus::EntityUnavailable, "client entity is not currently present");
    }
    if (state->mode == mode) {
        return;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    if (mode == ReplicationClientMode::Predict) {
        (void)validate_predicted_archetype(settings, state->archetype);
    }
    if (mode == ReplicationClientMode::Predict && !has_predicted_entities()) {
        has_predicted_frame_ = false;
        last_predicted_frame_ = 0;
    }
#ifdef KAGE_SYNC_ENABLE_TRACING
    const ReplicationClientMode previous_mode = state->mode;
#endif
    const bool switched = switch_entity_mode(registry, settings, *state, mode);
    if (switched) {
        sync_entity_memberships(*state);
#ifdef KAGE_SYNC_ENABLE_TRACING
        if (tracer_ != nullptr && tracer_->enabled() && previous_mode != state->mode) {
            SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::ModeChanged, client_id_, state->frame);
            event.server_entity = ecs::Entity{state->client_entity_network_id};
            event.local_entity = state->local;
            event.client_network_id = state->client_entity_network_id;
            event.wire_network_id = state->wire_network_id;
            event.network_version = client_entity_network_id_version(state->client_entity_network_id);
            event.archetype = state->archetype;
            event.previous_mode = previous_mode;
            event.mode = state->mode;
            tracer_->trace(event);
        }
#endif
    }
    if (switched && mode != ReplicationClientMode::Predict && !has_predicted_entities()) {
        has_predicted_frame_ = false;
        last_predicted_frame_ = 0;
    }
    if (!switched) {
        throw ClientError(ClientStatus::ModeSwitchFailed, "client entity mode switch failed");
    }
}

#ifdef KAGE_SYNC_ENABLE_TRACING
void ReplicationClient::trace_frame_components(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    bool resimulated,
    bool only_pending_rollback,
    TraceFrameComponentScope scope) {
    if (tracer_ == nullptr || !tracer_->enabled() || !tracer_->frame_data_enabled()) {
        return;
    }
    for (const EntityState& state : entities_) {
        if (state.client_entity_network_id == invalid_client_entity_network_id ||
            !state.local || !registry.alive(state.local) ||
            state.archetype.value >= settings.archetypes.size()) {
            continue;
        }
        if (only_pending_rollback && !state.prediction_rollback_pending) {
            continue;
        }
        if (scope == TraceFrameComponentScope::Predicted && state.mode != ReplicationClientMode::Predict) {
            continue;
        }
        if (scope == TraceFrameComponentScope::NonPredicted && state.mode == ReplicationClientMode::Predict) {
            continue;
        }
        const SyncArchetype& archetype = settings.archetypes[state.archetype.value];
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            const ComponentReplication& replication = archetype.components[component_index];
            const void* value = registry.get(state.local, replication.component);
            if (value == nullptr || component_index >= archetype.component_ops.size()) {
                continue;
            }
            const SyncComponentOps& ops = archetype.component_ops[component_index];
            if (ops.quantize == nullptr) {
                continue;
            }
            SyncComponentOps::QuantizedBytes bytes;
            bytes.resize(ops.quantized_size);
            ops.quantize(value, bytes.data());
            SyncTraceEvent event = make_client_trace_event(
                resimulated ? SyncTraceEventType::ResimulatedFrameComponent : SyncTraceEventType::FrameComponent,
                client_id_,
                frame);
            event.local_entity = state.local;
            event.server_entity = ecs::Entity{state.client_entity_network_id};
            event.client_network_id = state.client_entity_network_id;
            event.wire_network_id = state.wire_network_id;
            event.network_version = client_entity_network_id_version(state.client_entity_network_id);
            event.archetype = state.archetype;
            event.component = replication.component;
            event.mode = state.mode;
            append_trace_component_data(tracer_, archetype, component_index, bytes.data(), event);
            tracer_->trace(event);
        }
    }
}

void ReplicationClient::trace_input_components(
    ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    ecs::Entity component,
    const std::uint8_t* quantized) {
    if (tracer_ == nullptr || !tracer_->enabled() || !tracer_->frame_data_enabled() ||
        settings.local_client == invalid_client_id || quantized == nullptr || !component) {
        return;
    }
    const auto found_ops = settings.component_ops.find(component.value);
    if (found_ops == settings.component_ops.end()) {
        return;
    }
    const SyncComponentOps& ops = found_ops->second;
    registry.view<const NetworkOwner>().each([&](ecs::Entity entity, const NetworkOwner& owner) {
        if (owner.client != settings.local_client) {
            return;
        }
        SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::FrameComponent, client_id_, frame);
        event.local_entity = entity;
        const EntityState* state = find_entity_state_for_local(entity);
        if (state != nullptr && state->client_entity_network_id != invalid_client_entity_network_id) {
            event.server_entity = ecs::Entity{state->client_entity_network_id};
            event.client_network_id = state->client_entity_network_id;
            event.wire_network_id = state->wire_network_id;
            event.network_version = client_entity_network_id_version(state->client_entity_network_id);
            event.archetype = state->archetype;
            event.mode = state->mode;
        }
        event.component = component;
        append_trace_input_component_data(tracer_, ops, quantized, event);
        tracer_->trace(event);
    });
}

void ReplicationClient::trace_clock_skew(
    const char* stage,
    SyncFrame local_frame,
    SyncFrame server_frame,
    SyncFrame observed_receive_frame,
    SyncFrame observed_playback_frame,
    SyncFrame last_recorded_input_frame,
    SyncFrame prefill_input_frame) const {
    if (tracer_ == nullptr || !tracer_->enabled()) {
        return;
    }
    const ReplicationClientTimingStats& timing = clock_.stats();
    const SyncFrame observed_downstream =
        observed_receive_frame > server_frame ? observed_receive_frame - server_frame : 0U;
    const SyncFrame measured_prediction =
        last_recorded_input_frame >= server_frame ? last_recorded_input_frame - server_frame : 0U;
    const SyncFrame input_frame = clock_.input_frame();
    const SyncFrame input_lead = input_frame >= server_frame ? input_frame - server_frame : 0U;

    SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::ClockSkew, client_id_, local_frame);
    event.local_entity = ecs::Entity{client_id_ == invalid_client_id ? 0U : client_id_};
    event.component = ecs::Entity{std::numeric_limits<std::uint64_t>::max() - 1U};
    event.component_name = "Clock";
    std::ostringstream out;
    out << "stage=" << stage
        << ",server_frame=" << server_frame
        << ",receive_frame=" << observed_receive_frame
        << ",playback_frame=" << observed_playback_frame
        << ",input_frame=" << input_frame
        << ",last_recorded_input=" << last_recorded_input_frame
        << ",last_predicted=" << last_predicted_frame_
        << ",observed_downstream=" << observed_downstream
        << ",measured_prediction=" << measured_prediction
        << ",input_lead=" << input_lead
        << ",prediction_current=" << timing.current_prediction_lead_frames
        << ",prediction_target=" << timing.target_prediction_lead_frames
        << ",prediction_desired=" << timing.desired_prediction_lead_frames
        << ",prediction_measured=" << timing.measured_prediction_lead_frames
        << ",prediction_dilation=" << timing.prediction_time_dilation
        << ",interpolation_current=" << timing.current_interpolation_buffer_frames
        << ",interpolation_target=" << timing.target_interpolation_buffer_frames
        << ",interpolation_desired=" << timing.desired_interpolation_buffer_frames
        << ",interpolation_measured=" << timing.measured_interpolation_buffer_frames
        << ",time_dilation=" << timing.time_dilation
        << ",prefill_input=" << prefill_input_frame
        << ",active_snap_lead=" << active_prediction_snap_lead_frames_
        << ",pending_catchup=" << pending_prediction_catchup_frame_
        << ",pending_catchup_server=" << pending_prediction_catchup_server_frame_
        << ",has_predicted_entities=" << (has_predicted_entities() ? 1 : 0)
        << ",has_predicted_frame=" << (has_predicted_frame_ ? 1 : 0);
    event.data = out.str();
    tracer_->trace(event);
}

void ReplicationClient::trace_cue_event(
    SyncTraceEventType type,
    const SyncSettings& settings,
    const EntityState& state,
    const EntityCue& cue,
    const char* rollback_reason,
    const char* cue_source) const {
    if (tracer_ == nullptr || !tracer_->enabled()) {
        return;
    }
    SyncTraceEvent event = make_client_trace_event(type, client_id_, cue.frame);
    event.server_entity = ecs::Entity{state.client_entity_network_id};
    event.local_entity = state.local;
    event.client_network_id = state.client_entity_network_id;
    event.wire_network_id = state.wire_network_id;
    event.network_version = client_entity_network_id_version(state.client_entity_network_id);
    event.archetype = state.archetype;
    event.cue_type = cue.type;
    append_trace_cue_name(settings, cue.type, event);
    append_trace_cue_data(tracer_, settings, cue.type, cue.payload, event);
    append_trace_data_field(event, "source", cue_source);
    append_trace_data_field(event, "rollback_reason", rollback_reason);
    tracer_->trace(event);
}

void ReplicationClient::trace_cue_event(
    SyncTraceEventType type,
    const SyncSettings& settings,
    const EntityState& state,
    const EntityPlayedCue& cue,
    const char* rollback_reason,
    const char* cue_source) const {
    if (tracer_ == nullptr || !tracer_->enabled()) {
        return;
    }
    SyncTraceEvent event = make_client_trace_event(type, client_id_, cue.frame);
    event.server_entity = ecs::Entity{state.client_entity_network_id};
    event.local_entity = state.local;
    event.client_network_id = state.client_entity_network_id;
    event.wire_network_id = state.wire_network_id;
    event.network_version = client_entity_network_id_version(state.client_entity_network_id);
    event.archetype = state.archetype;
    event.cue_type = cue.type;
    append_trace_cue_name(settings, cue.type, event);
    append_trace_cue_data(tracer_, settings, cue.type, cue.payload, event);
    append_trace_data_field(event, "source", cue_source);
    append_trace_data_field(event, "rollback_reason", rollback_reason);
    tracer_->trace(event);
}

#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
void ReplicationClient::trace_outgoing_ack_packet(const std::vector<std::uint32_t>& acks) const {
    if (tracer_ == nullptr || !tracer_->enabled() || !tracer_->packet_logs_enabled()) {
        return;
    }
    SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::PacketLog, client_id_, clock_.input_frame());
    event.data = "direction=out,message=client_ack,acks=" + packet_ack_list(acks);
    tracer_->trace(event);
}

void ReplicationClient::trace_outgoing_input_packet(
    const std::vector<std::uint32_t>& acks,
    SyncFrame baseline_frame,
    SyncFrame first_input_frame,
    SyncFrame last_input_frame) const {
    if (tracer_ == nullptr || !tracer_->enabled() || !tracer_->packet_logs_enabled()) {
        return;
    }
    SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::PacketLog, client_id_, clock_.input_frame());
    std::ostringstream out;
    out << "direction=out,message=client_input,acks=" << packet_ack_list(acks)
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

void ReplicationClient::trace_incoming_update_packet(
    SyncFrame local_frame,
    SyncFrame server_frame,
    std::uint32_t packet_id,
    SyncFrame input_ack_frame,
    std::uint16_t record_count) const {
    if (tracer_ == nullptr || !tracer_->enabled() || !tracer_->packet_logs_enabled()) {
        return;
    }
    SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::PacketLog, client_id_, local_frame);
    std::ostringstream out;
    out << "direction=in,message=server_update,client=" << client_id_
        << ",sequence=" << packet_id
        << ",server_frame=" << server_frame
        << ",input_ack=" << input_ack_frame
        << ",record_count=" << record_count
        << ",cues=[";
    for (std::size_t index = 0; index < current_packet_cue_summaries_.size(); ++index) {
        if (index != 0U) {
            out << ";";
        }
        out << current_packet_cue_summaries_[index];
    }
    out << "]";
    event.data = out.str();
    tracer_->trace(event);
}
#endif
#endif

ReplicationClientMode ReplicationClient::entity_mode(ClientEntityNetworkId network_id) const noexcept {
    const EntityState* state = find_entity_state(network_id);
    return state != nullptr ? state->mode : options_.default_entity_mode;
}

bool ReplicationClient::set_interpolation_buffer_frames(SyncFrame frames) noexcept {
    if (!clock_.set_interpolation_buffer_frames(frames)) {
        return false;
    }
    options_.interpolation_buffer_frames = frames;
    return true;
}

SyncFrame ReplicationClient::current_interpolation_buffer_frames() const noexcept {
    return clock_.interpolation_buffer_frames();
}

double ReplicationClient::estimated_server_frame() const noexcept {
    if (!has_received_server_update_) {
        return 0.0;
    }
    const double elapsed_receive_frames = clock_.continuous_receive_frame() - last_server_update_receive_frame_;
    return static_cast<double>(last_server_update_frame_) + std::max(0.0, elapsed_receive_frames);
}

double ReplicationClient::continuous_prediction_frames_ahead() const noexcept {
    if (!has_received_server_update_) {
        return 0.0;
    }
    return clock_.continuous_input_frame() - estimated_server_frame();
}

double ReplicationClient::continuous_interpolation_frames_behind() const noexcept {
    if (!has_received_server_update_) {
        return 0.0;
    }
    return clock_.continuous_playback_frame() - clock_.display_target_frame();
}

bool ReplicationClient::tick(ecs::Registry& registry, double dt_seconds) {
    return tick(registry, dt_seconds, {});
}

void ReplicationClient::set_packet_sender(std::function<void(const ecs::BitBuffer&)> sender) {
    packet_sender_ = std::move(sender);
}

void ReplicationClient::receive_packet(ecs::BitBuffer packet) {
    inbound_packets_.push_back(std::move(packet));
}

bool ReplicationClient::tick(ecs::Registry& registry, double dt_seconds, ecs::RunJobsOptions prediction_options) {
    if (dt_seconds < 0.0 || !std::isfinite(dt_seconds)) {
        return false;
    }

    ReplicationClientClock::AdvanceResult advance;
    advance.receive = clock_.advance_receive(dt_seconds);

    if (connection_state_ == ReplicationClientConnectionState::Connecting ||
        connection_state_ == ReplicationClientConnectionState::Accepted) {
        connect_resend_accumulator_seconds_ += dt_seconds;
    }
    if (connection_state_ == ReplicationClientConnectionState::Accepted ||
        connection_state_ == ReplicationClientConnectionState::Ready) {
        ping_accumulator_seconds_ += dt_seconds;
    }

    if (!process_inbound_packets(registry)) {
        return false;
    }

    const ReplicationClientClock::AdvanceResult playback_input = clock_.advance_playback_input(dt_seconds);
    advance.playback = playback_input.playback;
    advance.input = playback_input.input;

    for (SyncFrame frame = advance.playback.first; !advance.playback.empty() && frame <= advance.playback.last; ++frame) {
        (void)apply_frame(registry, frame);
    }

    for (SyncFrame frame = advance.input.first; !advance.input.empty() && frame <= advance.input.last; ++frame) {
        const SyncSettings& settings = registry.get<SyncSettings>();
        (void)record_input_frame(registry, settings, frame);
        if (has_predicted_entities() && has_predicted_frame_ && frame > last_predicted_frame_) {
            if (!run_prediction_frame(registry, frame, prediction_options)) {
                return false;
            }
        }
    }

    if (options_.auto_timing_fast_recovery &&
        has_predicted_entities() &&
        has_predicted_frame_ &&
        pending_prediction_catchup_frame_ > clock_.input_frame()) {
        const SyncFrame first = clock_.input_frame() + 1U;
        const SyncFrame last = pending_prediction_catchup_frame_;
        const SyncSettings& settings = registry.get<SyncSettings>();
        for (SyncFrame frame = first; frame <= last; ++frame) {
            (void)record_input_frame(registry, settings, frame);
            if (frame > last_predicted_frame_ && !run_prediction_frame(registry, frame, prediction_options)) {
                return false;
            }
            clock_.advance_input_frame_to(frame);
        }
        if (pending_prediction_catchup_server_frame_ != 0U) {
#ifdef KAGE_SYNC_ENABLE_TRACING
            const SyncFrame catchup_server_frame = pending_prediction_catchup_server_frame_;
#endif
            clock_.record_prediction_lead(pending_prediction_catchup_server_frame_, clock_.input_frame());
#ifdef KAGE_SYNC_ENABLE_TRACING
            trace_clock_skew(
                "prediction_catchup_applied",
                clock_.receive_frame(),
                catchup_server_frame,
                clock_.receive_frame(),
                clock_.playback_frame(),
                input_->last_recorded_frame(),
                clock_.input_frame());
#endif
        }
    }
    if (pending_prediction_catchup_frame_ <= clock_.input_frame()) {
        pending_prediction_catchup_frame_ = 0;
        pending_prediction_catchup_server_frame_ = 0;
    }

    clock_.update_display_target(dt_seconds);
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (!advance.receive.empty()) {
        const SyncSettings& settings = registry.get<SyncSettings>();
        for (SyncFrame frame = advance.receive.first; frame <= advance.receive.last; ++frame) {
            trace_frame_components(
                registry,
                settings,
                frame,
                false,
                false,
                TraceFrameComponentScope::NonPredicted);
            if (frame == advance.receive.last) {
                break;
            }
        }
    }
#endif
    send_pending_packets();
    return true;
}

bool ReplicationClient::predict_tick(ecs::Registry& registry, SyncFrame frame, ecs::RunJobsOptions options) {
    return run_prediction_frame(registry, frame, options);
}

bool ReplicationClient::run_prediction_frame(ecs::Registry& registry, SyncFrame frame, ecs::RunJobsOptions options) {
    if (!apply_pending_prediction_rollback(registry, options)) {
        return false;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!apply_input_frame(registry, settings, frame)) {
        return false;
    }
    registry.run_jobs(options);

    drain_emitted_prediction_cues(registry, settings, frame, true);
    bool all_valid = true;
    for (std::uint32_t entity_index : active_entities_) {
        if (entity_index >= entities_.size()) {
            continue;
        }
        EntityState& state = entities_[entity_index];
        if (state.mode != ReplicationClientMode::Predict || state.client_entity_network_id == invalid_client_entity_network_id) {
            continue;
        }
        if (!quantize_predicted_entity(registry, settings, state, frame)) {
            all_valid = false;
        }
    }

    last_predicted_frame_ = frame;
    has_predicted_frame_ = true;
#ifdef KAGE_SYNC_ENABLE_TRACING
    trace_frame_components(registry, settings, frame, false, false, TraceFrameComponentScope::Predicted);
#endif
    return all_valid;
}

bool ReplicationClient::apply_frame(ecs::Registry& registry, SyncFrame client_frame) {
    if (!has_buffered_entities()) {
        return true;
    }
    const SyncFrame interpolation_buffer_frames = clock_.interpolation_buffer_frames();
    if (client_frame < interpolation_buffer_frames) {
        return false;
    }

    const SyncFrame target_frame = client_frame - interpolation_buffer_frames;
    last_applied_buffered_frame_ = target_frame;
    has_applied_buffered_frame_ = true;
    const SyncSettings& settings = registry.get<SyncSettings>();
    bool all_valid = true;
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    std::uint64_t interpolation_checks = 0;
    std::uint64_t interpolation_starvations = 0;
#endif
    for (const std::uint32_t entity_index : buffered_entities_) {
        if (entity_index >= entities_.size()) {
            continue;
        }
        EntityState& state = entities_[entity_index];
        if (state.buffered_frames.empty()) {
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
            ++interpolation_checks;
            ++interpolation_starvations;
#endif
#ifdef KAGE_SYNC_ENABLE_TRACING
            if (tracer_ != nullptr && tracer_->enabled()) {
                SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::BufferedStarved, client_id_, target_frame);
                event.server_entity = ecs::Entity{state.client_entity_network_id};
                event.local_entity = state.local;
                event.client_network_id = state.client_entity_network_id;
                event.wire_network_id = state.wire_network_id;
                event.network_version = client_entity_network_id_version(state.client_entity_network_id);
                event.archetype = state.archetype;
                tracer_->trace(event);
            }
#endif
            continue;
        }

        const std::size_t mask = state.buffered_frames.size() - 1U;
        const EntityBufferedFrame& sample = state.buffered_frames[target_frame & mask];
        if (!sample.valid || sample.frame != target_frame) {
            const bool reused_future_entity =
                state.entity_present &&
                target_frame < state.frame &&
                client_entity_network_id_version(state.client_entity_network_id) > 1U;
            const bool destroyed_past_entity =
                !state.entity_present &&
                target_frame > state.frame;
            if (!state.local && (reused_future_entity || destroyed_past_entity)) {
                continue;
            }
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
            ++interpolation_checks;
            ++interpolation_starvations;
#endif
#ifdef KAGE_SYNC_ENABLE_TRACING
            if (tracer_ != nullptr && tracer_->enabled()) {
                SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::BufferedStarved, client_id_, target_frame);
                event.server_entity = ecs::Entity{state.client_entity_network_id};
                event.local_entity = state.local;
                event.client_network_id = state.client_entity_network_id;
                event.wire_network_id = state.wire_network_id;
                event.network_version = client_entity_network_id_version(state.client_entity_network_id);
                event.archetype = state.archetype;
                tracer_->trace(event);
            }
#endif
            all_valid = false;
            continue;
        }
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
        ++interpolation_checks;
#endif
        if (!apply_buffered_sample(registry, settings, state, sample)) {
            all_valid = false;
            continue;
        }
        for (const EntityCue& cue : sample.cues) {
            const SyncFrame late_frames = target_frame > cue.frame ? target_frame - cue.frame : 0U;
            (void)play_cue(
                registry,
                settings,
                state,
                cue,
                static_cast<float>(static_cast<double>(late_frames) * options_.fixed_dt_seconds),
                true);
        }
    }

#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    record_interpolation_frame(interpolation_checks, interpolation_starvations);
#endif
    return all_valid;
}

#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
void ReplicationClient::record_interpolation_frame(std::uint64_t checks, std::uint64_t starvations) noexcept {
    interpolation_diagnostics_.record_frame(checks, starvations);
}
#endif

bool ReplicationClient::sample_fractional_tick_target_frame(
    const ecs::Registry& registry,
    double target_frame,
    FractionalTickSampleBuffer& out) const {
    return write_fractional_tick_samples(registry, target_frame, false, false, out);
}

bool ReplicationClient::sample_fractional_tick_frame(
    const ecs::Registry& registry,
    double client_frame,
    FractionalTickSampleBuffer& out) const {
    return sample_fractional_tick_target_frame(
        registry,
        client_frame - static_cast<double>(clock_.interpolation_buffer_frames()),
        out);
}

const FractionalTickSampleBuffer& ReplicationClient::fractional_tick_frame(const ecs::Registry& registry) {
    const double display_accumulator_seconds = clock_.consume_display_accumulator_seconds();
    const float display_dt = display_accumulator_seconds > 0.0 && std::isfinite(display_accumulator_seconds)
        ? static_cast<float>(display_accumulator_seconds)
        : 0.0f;
    if (display_dt > 0.0f) {
        blend_snap_errors(registry.get<SyncSettings>(), display_dt);
    }
    if (write_fractional_tick_samples(registry, clock_.display_target_frame(), true, true, fractional_tick_scratch_)) {
        fractional_tick_frame_.entities.swap(fractional_tick_scratch_.entities);
        fractional_tick_scratch_.clear();
    } else if (fractional_tick_frame_.entities.empty()) {
        fractional_tick_frame_.entities.swap(fractional_tick_scratch_.entities);
        fractional_tick_scratch_.clear();
    }
    return fractional_tick_frame_;
}

std::vector<ecs::BitBuffer> ReplicationClient::drain_packets() {
    std::vector<ecs::BitBuffer> packets;
    drain_connect_packets(packets);
    drain_ping_packets(packets);
    drain_input_packets_into(packets);
    drain_ack_packets_into(packets);
    return packets;
}

std::vector<ecs::BitBuffer> ReplicationClient::drain_ack_packets() {
    std::vector<ecs::BitBuffer> packets;
    drain_ack_packets_into(packets);
    return packets;
}

bool ReplicationClient::process_inbound_packets(ecs::Registry& registry) {
    for (ecs::BitBuffer& packet : inbound_packets_) {
        (void)receive(registry, std::move(packet));
    }
    inbound_packets_.clear();
    return true;
}

void ReplicationClient::send_pending_packets() {
    if (!packet_sender_) {
        return;
    }
    for (const ecs::BitBuffer& packet : drain_packets()) {
        packet_sender_(packet);
    }
}

void ReplicationClient::drain_ack_packets_into(std::vector<ecs::BitBuffer>& packets) {
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    std::vector<client_detail::ClientAckPacketTrace> traces;
    ack_queue_->drain_ack_packets(options_.mtu_bytes, configured_packet_id_bits(options_), packets, &traces);
    for (const client_detail::ClientAckPacketTrace& trace : traces) {
        trace_outgoing_ack_packet(trace.acks);
    }
#else
    ack_queue_->drain_ack_packets(options_.mtu_bytes, configured_packet_id_bits(options_), packets, nullptr);
#endif
}

void ReplicationClient::drain_input_packets_into(std::vector<ecs::BitBuffer>& packets) {
    if (connection_state_ != ReplicationClientConnectionState::Ready || !clock_.bootstrapped()) {
        return;
    }
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    client_detail::ClientInputPacketTrace trace;
    const bool sent = input_->drain_packet(
        options_.mtu_bytes,
        configured_packet_id_bits(options_),
        ack_queue_->pending(),
        packets,
        &trace);
    if (sent && trace.sent) {
        trace_outgoing_input_packet(trace.acks, trace.baseline_frame, trace.first_input_frame, trace.last_input_frame);
    }
#else
    (void)input_->drain_packet(
        options_.mtu_bytes,
        configured_packet_id_bits(options_),
        ack_queue_->pending(),
        packets,
        nullptr);
#endif
}

std::size_t ReplicationClient::pending_ack_count() const noexcept {
    return ack_queue_->size();
}

bool ReplicationClient::set_input_bytes(ecs::Registry& registry, ecs::Entity component, const void* input) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    return input_->set_latest(registry, settings, component, input);
}

bool ReplicationClient::record_input_frame(ecs::Registry& registry, const SyncSettings& settings, SyncFrame frame) {
    client_detail::ClientInputRecord recorded;
    const bool ok = input_->record_frame(settings, options_.input_buffer_capacity_frames, frame, &recorded);
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (ok && recorded.bytes != nullptr) {
        trace_input_components(registry, settings, recorded.frame, recorded.component, recorded.bytes);
    }
#else
    (void)registry;
#endif
    return ok;
}

bool ReplicationClient::fill_input_frames_through(
    ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame) {
    std::vector<client_detail::ClientInputRecord> recorded;
    const bool ok = input_->fill_frames_through(settings, options_.input_buffer_capacity_frames, frame, recorded);
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (ok) {
        for (const client_detail::ClientInputRecord& input_record : recorded) {
            trace_input_components(registry, settings, input_record.frame, input_record.component, input_record.bytes);
        }
    }
#else
    (void)registry;
#endif
    return ok;
}

bool ReplicationClient::apply_input_frame(ecs::Registry& registry, const SyncSettings& settings, SyncFrame frame) {
    return input_->apply_frame(registry, settings, frame);
}

void ReplicationClient::acknowledge_input_frame(SyncFrame frame) {
    input_->acknowledge_frame(frame);
}

ecs::Entity ReplicationClient::local_entity(ClientEntityNetworkId network_id) const {
    const EntityState* state = find_entity_state(network_id);
    return state != nullptr ? state->local : ecs::Entity{};
}

bool ReplicationClient::is_alive_network_id(ClientEntityNetworkId network_id) const noexcept {
    const EntityState* state = find_entity_state(network_id);
    return state != nullptr && state->local;
}

EntityReferenceStatus ReplicationClient::resolve_entity_reference(EntityReference& reference) const noexcept {
    const ClientEntityNetworkId network_id = reference.client_entity_network_id;
    if (network_id == invalid_client_entity_network_id) {
        reference = EntityReference{};
        return EntityReferenceStatus::Invalid;
    }

    const std::uint32_t wire_network_id = client_entity_network_id_wire_id(network_id);
    if (wire_network_id == 0U || wire_network_id > max_client_local_wire_network_id) {
        reference = EntityReference{};
        return EntityReferenceStatus::Invalid;
    }

    const EntityState* state = find_entity_state(network_id);
    if (state != nullptr) {
        if (state->local) {
            reference.entity = state->local;
            return EntityReferenceStatus::Alive;
        }
        reference.entity = ecs::Entity{};
        return EntityReferenceStatus::Pending;
    }

    if (wire_network_id >= wire_network_ids_.size()) {
        reference.entity = ecs::Entity{};
        return EntityReferenceStatus::Pending;
    }

    const WireNetworkIdState& wire_state = wire_network_ids_[wire_network_id];
    if (wire_state.version == 0U) {
        reference.entity = ecs::Entity{};
        return EntityReferenceStatus::Pending;
    }

    if (wire_state.version != client_entity_network_id_version(network_id)) {
        reference = EntityReference{};
        return EntityReferenceStatus::Destroyed;
    }

    reference.entity = ecs::Entity{};
    return EntityReferenceStatus::Pending;
}

ClientEntityNetworkId ReplicationClient::client_entity_network_id_for_wire(std::uint32_t wire_network_id) {
    if (wire_network_id == 0U || wire_network_id > max_client_local_wire_network_id) {
        return invalid_client_entity_network_id;
    }
    if (wire_network_id >= wire_network_ids_.size()) {
        wire_network_ids_.resize(static_cast<std::size_t>(wire_network_id) + 1U);
    }
    WireNetworkIdState& state = wire_network_ids_[wire_network_id];
    if (state.version == 0U) {
        state.version = 1U;
    }
    const ClientId id = client_id_ == invalid_client_id ? 0U : client_id_;
    return make_client_entity_network_id(id, wire_network_id, state.version);
}

void ReplicationClient::advance_wire_network_id_version(std::uint32_t wire_network_id) {
    if (wire_network_id == 0U || wire_network_id > max_client_local_wire_network_id) {
        return;
    }
    if (wire_network_id >= wire_network_ids_.size()) {
        wire_network_ids_.resize(static_cast<std::size_t>(wire_network_id) + 1U);
    }
    WireNetworkIdState& state = wire_network_ids_[wire_network_id];
    if (state.version == 0U) {
        state.version = 1U;
    } else {
        ++state.version;
        if (state.version == 0U) {
            state.version = 1U;
        }
    }
    state.entity_index = invalid_entity_index;
    state.alive = false;
}

bool ReplicationClient::apply_update(
    ecs::Registry& registry,
    ecs::BitBuffer& packet,
    std::uint32_t packet_id,
    SyncFrame frame,
    std::uint16_t record_count) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (client_id_ == invalid_client_id && settings.local_client != invalid_client_id) {
        client_id_ = settings.local_client;
    }
    bool applied_any = false;

    for (std::uint16_t record = 0; record < record_count; ++record) {
        const bool destroy = packet.read_bool();
        std::uint32_t network_id = 0;
        if (!protocol::read_network_entity_id(packet, network_id, options_.protocol.network_entity_id_tier0_bits)) {
            return false;
        }
        if (network_id == 0U) {
            return false;
        }

        const bool applied = destroy
            ? apply_destroy(registry, frame, network_id)
            : apply_upsert(registry, settings, frame, network_id, packet);
        if (!applied) {
            return false;
        }
        applied_any = true;
    }

    if (applied_any) {
        queue_ack(packet_id);
    }
    return applied_any;
}

bool ReplicationClient::apply_upsert(
    ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    std::uint32_t network_id,
    ecs::BitBuffer& packet) {
    const bool full = packet.read_bool();
    const ClientEntityNetworkId client_entity_network_id = client_entity_network_id_for_wire(network_id);
    if (client_entity_network_id == invalid_client_entity_network_id) {
        return false;
    }
    SyncArchetypeId archetype = invalid_sync_archetype_id;
    SyncFrame baseline_frame = 0;

    EntityState* found_state = nullptr;
    if (full) {
        if (destroy_tombstone_blocks(network_id, frame)) {
            return false;
        }
        found_state = find_entity_state(client_entity_network_id);
    } else {
        found_state = find_entity_state(network_id);
    }
    const bool previous_absent = found_state != nullptr &&
        !found_state->entity_present &&
        !found_state->local;
    if (full) {
        archetype = SyncArchetypeId{static_cast<std::uint32_t>(packet.read_bits(32U))};
        if (archetype.value >= settings.archetypes.size()) {
            return false;
        }
        if (previous_absent) {
            EntityState& state = *found_state;
            state.archetype = archetype;
            state.mode = ReplicationClientMode::Snap;
            state.entity_present = false;
            state.mode_selected = false;
            state.baseline.clear();
            state.history.clear();
            state.history_next = 0;
            state.applied_present_mask = 0;
            sync_entity_memberships(state);
        }
    } else {
        if (found_state == nullptr || previous_absent) {
            return false;
        }
        archetype = found_state->archetype;
        if (archetype.value >= settings.archetypes.size()) {
            return false;
        }
        if (frame <= found_state->frame) {
            return false;
        }
        if (!protocol::read_baseline_frame(packet, frame, baseline_frame)) {
            return false;
        }
    }

    const SyncArchetype& definition = settings.archetypes[archetype.value];
    const bool collect_decoded_updates = options_.entity_mode_selector &&
        (found_state == nullptr || previous_absent || !found_state->mode_selected);
    const bool buffered_without_selector = !options_.entity_mode_selector &&
        (found_state != nullptr && !previous_absent && found_state->mode_selected
             ? found_state->mode == ReplicationClientMode::BufferedInterpolation
             : options_.default_entity_mode == ReplicationClientMode::BufferedInterpolation);
    std::vector<ComponentBaseline> decoded_updates;
    QuantizedFrameData merged;
    QuantizedFrameData decoded;
    if (!init_frame_data(definition, merged)) {
        return false;
    }
    if (!buffered_without_selector && !init_frame_data(definition, decoded)) {
        return false;
    }
    QuantizedFrameData& received = buffered_without_selector ? merged : decoded;
    EntityReferenceContext reference_context;
    bool reference_context_initialized = false;
    auto references_for_component = [&]() -> EntityReferenceContext* {
        if (!reference_context_initialized) {
            reference_context.user = this;
            reference_context.network_entity_id_tier0_bits = options_.protocol.network_entity_id_tier0_bits;
            reference_context.client_entity_network_id_for_wire = [](void* user, std::uint32_t wire_network_id) {
                return static_cast<ReplicationClient*>(user)->client_entity_network_id_for_wire(wire_network_id);
            };
            reference_context.client_local_entity = [](void* user, ClientEntityNetworkId network_id) {
                return static_cast<ReplicationClient*>(user)->local_entity(network_id);
            };
            reference_context_initialized = true;
        }
        return &reference_context;
    };
#ifdef KAGE_SYNC_ENABLE_TRACING
    std::vector<SyncTraceEvent> pending_component_received_events;
#endif

    EntityState* previous_state = found_state != nullptr && !previous_absent ? found_state : nullptr;
    const QuantizedFrameData* previous_baseline = nullptr;
    if (!full && previous_state != nullptr) {
        previous_baseline = find_baseline(*previous_state, baseline_frame);
        if (previous_baseline == nullptr) {
            return false;
        }
    }

    if (full) {
        const std::size_t sync_slot_bits = protocol::bits_for_range(sync_slot_count(definition));
        const bool uses_presence_mask = packet.read_bool();
        std::uint64_t presence_mask = 0;
        std::uint16_t component_count = 0;
        if (uses_presence_mask) {
            presence_mask = packet.read_unsigned_bits(sync_slot_count(definition));
            for (std::size_t sync_slot = 0; sync_slot < sync_slot_count(definition); ++sync_slot) {
                if ((presence_mask & sync_slot_bit(sync_slot)) != 0U) {
                    ++component_count;
                }
            }
        } else {
            component_count = static_cast<std::uint16_t>(packet.read_bits(16U));
        }
        if (collect_decoded_updates) {
            decoded_updates.reserve(component_count);
        }

        auto read_full_slot = [&](std::uint16_t sync_slot) {
            if (sync_slot >= sync_slot_count(definition)) {
                return false;
            }
            if (sync_slot == 0U) {
                if (!has_tag_slot(definition)) {
                    return false;
                }
                received.tag_mask = packet.read_unsigned_bits(definition.tags.size());
                return true;
            }

            const std::size_t component_index = static_cast<std::size_t>(sync_slot - 1U);
            const ecs::Entity component_entity = definition.components[component_index].component;
            if (component_index >= definition.component_ops.size()) {
                return false;
            }
            const SyncComponentOps& ops = definition.component_ops[component_index];
            if (ops.deserialize == nullptr || ops.apply == nullptr) {
                return false;
            }

            ComponentBaseline baseline;
            baseline.component = component_entity;
            std::uint8_t* received_bytes = mutable_frame_component_data(definition, received, component_index);
            if (received_bytes == nullptr) {
                return false;
            }
            if (!ops.deserialize(
                    packet,
                    nullptr,
                    received_bytes,
                    ops.references_entities ? references_for_component() : nullptr)) {
                return false;
            }
#ifdef KAGE_SYNC_ENABLE_TRACING
            if (tracer_ != nullptr && tracer_->enabled()) {
                SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::ComponentReceived, client_id_, frame);
                event.server_entity = ecs::Entity{client_entity_network_id};
                event.client_network_id = client_entity_network_id;
                event.wire_network_id = network_id;
                event.network_version = client_entity_network_id_version(client_entity_network_id);
                event.archetype = archetype;
                event.component = component_entity;
                append_trace_component_data(tracer_, definition, component_index, received_bytes, event);
                pending_component_received_events.push_back(std::move(event));
            }
#endif
            if (collect_decoded_updates) {
                baseline.bytes.assign(received_bytes, ops.quantized_size);
            }
            if (collect_decoded_updates) {
                decoded_updates.push_back(std::move(baseline));
            }
            return true;
        };

        if (uses_presence_mask) {
            for (std::size_t sync_slot = 0; sync_slot < sync_slot_count(definition); ++sync_slot) {
                if ((presence_mask & sync_slot_bit(sync_slot)) == 0U) {
                    continue;
                }
                if (!read_full_slot(static_cast<std::uint16_t>(sync_slot))) {
                    return false;
                }
            }
        } else {
            for (std::uint16_t component = 0; component < component_count; ++component) {
                const auto sync_slot = static_cast<std::uint16_t>(packet.read_bits(sync_slot_bits));
                if (!read_full_slot(sync_slot)) {
                    return false;
                }
            }
        }
        if (!buffered_without_selector) {
            merged = decoded;
        }
    } else {
        if (previous_baseline == nullptr) {
            return false;
        }
        const std::uint64_t changed_mask = packet.read_unsigned_bits(sync_slot_count(definition));
        if (collect_decoded_updates) {
            decoded_updates.reserve(definition.components.size());
        }
        merged = *previous_baseline;
        if ((changed_mask & sync_slot_bit(0)) != 0U) {
            if (!has_tag_slot(definition)) {
                return false;
            }
            merged.tag_mask = packet.read_unsigned_bits(definition.tags.size());
            if (!buffered_without_selector) {
                decoded.tag_mask = merged.tag_mask;
            }
        }
        for (std::size_t component_index = 0; component_index < definition.components.size(); ++component_index) {
            if ((changed_mask & sync_slot_bit(component_index + 1U)) == 0U) {
                continue;
            }
            const ecs::Entity component_entity = definition.components[component_index].component;
            if (component_index >= definition.component_ops.size()) {
                return false;
            }
            const SyncComponentOps& ops = definition.component_ops[component_index];
            if (ops.deserialize == nullptr || ops.apply == nullptr) {
                return false;
            }

            ComponentBaseline baseline;
            baseline.component = component_entity;
            const std::uint8_t* previous_bytes =
                frame_component_data(definition, *previous_baseline, component_index);
            std::uint8_t* merged_bytes = mutable_frame_component_data(definition, merged, component_index);
            std::uint8_t* decoded_bytes = buffered_without_selector
                ? nullptr
                : mutable_frame_component_data(definition, decoded, component_index);
            if (previous_bytes == nullptr || merged_bytes == nullptr ||
                (!buffered_without_selector && decoded_bytes == nullptr)) {
                return false;
            }
            if (!ops.deserialize(
                    packet,
                    previous_bytes,
                    merged_bytes,
                    ops.references_entities ? references_for_component() : nullptr)) {
                return false;
            }
#ifdef KAGE_SYNC_ENABLE_TRACING
            if (tracer_ != nullptr && tracer_->enabled()) {
                SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::ComponentReceived, client_id_, frame);
                event.server_entity = ecs::Entity{client_entity_network_id};
                event.client_network_id = client_entity_network_id;
                event.wire_network_id = network_id;
                event.network_version = client_entity_network_id_version(client_entity_network_id);
                event.archetype = archetype;
                event.component = component_entity;
                append_trace_component_data(tracer_, definition, component_index, merged_bytes, event);
                pending_component_received_events.push_back(std::move(event));
            }
#endif
            if (!buffered_without_selector) {
                std::memcpy(decoded_bytes, merged_bytes, ops.quantized_size);
            }
            if (collect_decoded_updates) {
                baseline.bytes.assign(merged_bytes, ops.quantized_size);
            }
            if (collect_decoded_updates) {
                decoded_updates.push_back(std::move(baseline));
            }
        }
        if (!buffered_without_selector) {
            decoded.tag_mask = merged.tag_mask;
        }
    }

#ifdef KAGE_SYNC_ENABLE_TRACING
    if (tracer_ != nullptr && tracer_->enabled() && has_tag_slot(definition)) {
        const bool tags_changed = full || previous_baseline == nullptr || previous_baseline->tag_mask != merged.tag_mask;
        if (tags_changed) {
            for (std::size_t tag_index = 0; tag_index < definition.tags.size(); ++tag_index) {
                SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::TagReceived, client_id_, frame);
                event.server_entity = ecs::Entity{client_entity_network_id};
                event.client_network_id = client_entity_network_id;
                event.wire_network_id = network_id;
                event.network_version = client_entity_network_id_version(client_entity_network_id);
                event.archetype = archetype;
                event.tag = definition.tags[tag_index].tag;
                event.remove = (merged.tag_mask & (std::uint64_t{1} << tag_index)) == 0U;
                tracer_->trace(event);
            }
        }
    }
#endif

    std::vector<EntityCue> received_cues;
    if (!read_cues(packet, received_cues)) {
        return false;
    }
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    if (!received_cues.empty()) {
        current_packet_cue_summaries_.reserve(current_packet_cue_summaries_.size() + received_cues.size());
        for (const EntityCue& cue : received_cues) {
            std::ostringstream out;
            out << "{entity=" << client_entity_network_id
                << ",frame=" << cue.frame
                << ",type=" << cue.type;
#ifdef KAGE_SYNC_TRACE_COMPONENT_DATA
            if (tracer_ != nullptr && tracer_->frame_data_enabled() &&
                cue.type < settings.cue_ops.size() && settings.cue_ops[cue.type].trace != nullptr) {
                SyncTraceStringBuilder builder;
                if (settings.cue_ops[cue.type].trace(cue.payload, builder) && !builder.value.empty()) {
                    out << ",data=" << builder.value;
                }
            }
#endif
            out << "}";
            current_packet_cue_summaries_.push_back(out.str());
        }
    }
#endif
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (tracer_ != nullptr && tracer_->enabled()) {
        for (const EntityCue& cue : received_cues) {
            SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::CueReceived, client_id_, cue.frame);
            event.server_entity = ecs::Entity{client_entity_network_id};
            event.client_network_id = client_entity_network_id;
            event.wire_network_id = network_id;
            event.network_version = client_entity_network_id_version(client_entity_network_id);
            event.archetype = archetype;
            event.cue_type = cue.type;
            append_trace_cue_name(settings, cue.type, event);
            append_trace_cue_data(tracer_, settings, cue.type, cue.payload, event);
            append_trace_data_field(event, "source", "server");
            tracer_->trace(event);
        }
    }
#endif

    if (previous_state != nullptr && frame <= previous_state->frame) {
        return false;
    }

    const bool mode_needs_selection = found_state == nullptr || previous_absent || !found_state->mode_selected;
    ReplicationClientMode selected_mode = options_.default_entity_mode;
    if (mode_needs_selection) {
        if (options_.entity_mode_selector) {
            ReplicatedEntityUpdateView update;
            update.client_entity_network_id = client_entity_network_id;
            update.local_entity = found_state != nullptr ? found_state->local : ecs::Entity{};
            update.archetype = archetype;
            update.frame = frame;
            update.tag_mask = merged.tag_mask;
            update.components = &decoded_updates;
            selected_mode = options_.entity_mode_selector(update);
        }
        if (selected_mode == ReplicationClientMode::Predict) {
            (void)validate_predicted_archetype(settings, archetype);
        }
    }

    EntityState* ensured_state = ensure_entity_state(registry, client_entity_network_id, network_id);
    if (ensured_state == nullptr) {
        return false;
    }
    EntityState& state = *ensured_state;
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (tracer_ != nullptr && tracer_->enabled() && full && (found_state == nullptr || previous_absent)) {
        SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::EntityReceived, client_id_, frame);
        event.server_entity = ecs::Entity{client_entity_network_id};
        event.local_entity = state.local;
        event.client_network_id = client_entity_network_id;
        event.wire_network_id = network_id;
        event.network_version = client_entity_network_id_version(client_entity_network_id);
        event.archetype = archetype;
        tracer_->trace(event);
    }
#endif
    if (mode_needs_selection) {
#ifdef KAGE_SYNC_ENABLE_TRACING
        if (tracer_ != nullptr && tracer_->enabled() && state.mode != selected_mode) {
            SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::ModeChanged, client_id_, frame);
            event.server_entity = ecs::Entity{client_entity_network_id};
            event.local_entity = state.local;
            event.client_network_id = client_entity_network_id;
            event.wire_network_id = network_id;
            event.network_version = client_entity_network_id_version(client_entity_network_id);
            event.archetype = archetype;
            event.previous_mode = state.mode;
            event.mode = selected_mode;
            tracer_->trace(event);
        }
#endif
        state.mode = selected_mode;
        state.mode_selected = true;
    }

#ifdef KAGE_SYNC_ENABLE_TRACING
    if (tracer_ != nullptr && tracer_->enabled()) {
        for (SyncTraceEvent& event : pending_component_received_events) {
            event.local_entity = state.local;
            event.mode = state.mode;
            tracer_->trace(event);
        }
    }
#endif

    if (state.mode == ReplicationClientMode::BufferedInterpolation) {
        const bool applied =
            apply_buffered_upsert(registry, settings, frame, client_entity_network_id, archetype, merged);
        if (applied && full) {
            destroy_tombstones_.erase(network_id);
        }
        if (applied) {
            store_buffered_cues(state, frame, received_cues);
            if (has_applied_buffered_frame_) {
                for (const EntityCue& cue : received_cues) {
                    if (cue.frame > last_applied_buffered_frame_) {
                        continue;
                    }
                    const SyncFrame late_frames = last_applied_buffered_frame_ - cue.frame;
                    (void)play_cue(
                        registry,
                        settings,
                        state,
                        cue,
                        static_cast<float>(static_cast<double>(late_frames) * options_.fixed_dt_seconds),
                        true);
                }
            }
        }
        return applied;
    }

    if (state.mode == ReplicationClientMode::Predict) {
        const bool applied =
            apply_predicted_upsert(registry, settings, frame, client_entity_network_id, archetype, merged, full);
        if (applied && full) {
            destroy_tombstones_.erase(network_id);
        }
        if (applied) {
            reconcile_authoritative_predicted_cues(registry, settings, state, received_cues, frame);
        }
        return applied;
    }

    if (full && state.local && registry.alive(state.local) &&
        state.archetype != archetype &&
        state.archetype.value < settings.archetypes.size()) {
        remove_archetype_tags(registry, state.local, settings.archetypes[state.archetype.value]);
    }
    state.archetype = archetype;
    if (!apply_snap_sample(registry, settings, state, decoded, full)) {
        return false;
    }
    play_snap_cues(registry, settings, state, received_cues);
    if (!full) {
        state.baseline = std::move(merged);
        state.applied_present_mask = state.baseline.present_mask;
    }

    state.frame = frame;
    state.entity_present = true;
    if (full) {
        destroy_tombstones_.erase(network_id);
    }
    remember_baseline(state);
    return true;
}

bool ReplicationClient::read_cues(ecs::BitBuffer& packet, std::vector<EntityCue>& out) {
    out.clear();
    const bool has_cues = packet.read_bool();
    if (!has_cues) {
        return true;
    }
    const auto cue_count = static_cast<std::uint16_t>(packet.read_bits(16U));
    out.reserve(cue_count);
    for (std::uint16_t index = 0; index < cue_count; ++index) {
        EntityCue cue;
        cue.frame = static_cast<SyncFrame>(packet.read_bits(32U));
        cue.type = static_cast<SyncCueTypeId>(packet.read_bits(16U));
        packet.read_bytes(reinterpret_cast<char*>(&cue.relevance_seconds), sizeof(cue.relevance_seconds));
        const auto payload_bits = static_cast<std::uint16_t>(packet.read_bits(16U));
        packet.read_buffer_bits(cue.payload, payload_bits);
        out.push_back(std::move(cue));
    }
    return true;
}

bool ReplicationClient::play_cue(
    ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const EntityCue& cue,
    float late_seconds,
    bool confirmed) {
    if (cue.type >= settings.cue_ops.size() || settings.cue_ops[cue.type].play == nullptr) {
        return false;
    }
    EntityReferenceContext reference_context;
    reference_context.user = this;
    reference_context.network_entity_id_tier0_bits = options_.protocol.network_entity_id_tier0_bits;
    reference_context.client_entity_network_id_for_wire = [](void* user, std::uint32_t wire_network_id) {
        return static_cast<ReplicationClient*>(user)->client_entity_network_id_for_wire(wire_network_id);
    };
    reference_context.client_local_entity = [](void* user, ClientEntityNetworkId network_id) {
        return static_cast<ReplicationClient*>(user)->local_entity(network_id);
    };
    EntityReferenceContext* references = settings.cue_ops[cue.type].references_entities ? &reference_context : nullptr;
    auto existing = std::find_if(
        state.played_cues.begin(),
        state.played_cues.end(),
        [&](const EntityPlayedCue& played) {
            if (played.frame != cue.frame || played.type != cue.type ||
                settings.cue_ops[cue.type].equals == nullptr) {
                return false;
            }
            return settings.cue_ops[cue.type].equals(played.payload, cue.payload, references);
        });
    if (existing != state.played_cues.end()) {
#ifdef KAGE_SYNC_ENABLE_TRACING
        const bool newly_confirmed = confirmed && !existing->confirmed;
#endif
        existing->confirmed = existing->confirmed || confirmed;
        existing->seen_in_resim = true;
#ifdef KAGE_SYNC_ENABLE_TRACING
        if (newly_confirmed) {
            trace_cue_event(SyncTraceEventType::CueConfirmed, settings, state, *existing, nullptr, "server");
        }
#endif
        return true;
    }
    if (!state.local || !registry.alive(state.local)) {
        return false;
    }
    if (!settings.cue_ops[cue.type].play(registry, state.local, cue.payload, late_seconds, cue.frame, references)) {
        return false;
    }
#ifdef KAGE_SYNC_ENABLE_TRACING
    trace_cue_event(
        SyncTraceEventType::CuePlayed,
        settings,
        state,
        cue,
        nullptr,
        confirmed ? "server" : "local_prediction");
#endif
    state.played_cues.push_back(EntityPlayedCue{
        cue.frame,
        cue.type,
        cue.payload,
        confirmed,
        true});
    return true;
}

bool ReplicationClient::rollback_played_cue(
    ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const EntityPlayedCue& cue,
    const char* rollback_reason) {
#ifndef KAGE_SYNC_ENABLE_TRACING
    (void)rollback_reason;
#endif
    if (cue.type >= settings.cue_ops.size() || settings.cue_ops[cue.type].rollback == nullptr) {
        return false;
    }
    if (!state.local || !registry.alive(state.local)) {
        return true;
    }
    EntityReferenceContext reference_context;
    reference_context.user = this;
    reference_context.network_entity_id_tier0_bits = options_.protocol.network_entity_id_tier0_bits;
    reference_context.client_entity_network_id_for_wire = [](void* user, std::uint32_t wire_network_id) {
        return static_cast<ReplicationClient*>(user)->client_entity_network_id_for_wire(wire_network_id);
    };
    reference_context.client_local_entity = [](void* user, ClientEntityNetworkId network_id) {
        return static_cast<ReplicationClient*>(user)->local_entity(network_id);
    };
    EntityReferenceContext* references = settings.cue_ops[cue.type].references_entities ? &reference_context : nullptr;
    const bool rolled_back = settings.cue_ops[cue.type].rollback(registry, state.local, cue.payload, references);
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (rolled_back) {
        trace_cue_event(SyncTraceEventType::CueRolledBack, settings, state, cue, rollback_reason, "local_prediction");
    }
#endif
    return rolled_back;
}

void ReplicationClient::play_snap_cues(
    ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const std::vector<EntityCue>& cues) {
    for (const EntityCue& cue : cues) {
        const SyncFrame receive_frame = clock_.receive_frame();
        const SyncFrame late_frames = receive_frame > cue.frame ? receive_frame - cue.frame : 0U;
        (void)play_cue(
            registry,
            settings,
            state,
            cue,
            static_cast<float>(static_cast<double>(late_frames) * options_.fixed_dt_seconds),
            true);
    }
}

void ReplicationClient::store_buffered_cues(
    EntityState& state,
    SyncFrame frame,
    const std::vector<EntityCue>& cues) {
    if (state.buffered_frames.empty() || cues.empty()) {
        return;
    }
    EntityBufferedFrame& sample = state.buffered_frames[frame & (state.buffered_frames.size() - 1U)];
    if (!sample.valid || sample.frame != frame) {
        return;
    }
    sample.cues.insert(sample.cues.end(), cues.begin(), cues.end());
}

void ReplicationClient::reconcile_authoritative_predicted_cues(
    ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const std::vector<EntityCue>& cues,
    SyncFrame frame) {
    for (const EntityCue& cue : cues) {
        auto found = std::find_if(
            state.played_cues.begin(),
            state.played_cues.end(),
            [&](const EntityPlayedCue& played) {
                if (played.frame != cue.frame || played.type != cue.type ||
                    cue.type >= settings.cue_ops.size() || settings.cue_ops[cue.type].equals == nullptr) {
                    return false;
                }
                return settings.cue_ops[cue.type].equals(played.payload, cue.payload, nullptr);
        });
        if (found != state.played_cues.end()) {
#ifdef KAGE_SYNC_ENABLE_TRACING
            const bool newly_confirmed = !found->confirmed;
#endif
            found->confirmed = true;
#ifdef KAGE_SYNC_ENABLE_TRACING
            if (newly_confirmed) {
                trace_cue_event(SyncTraceEventType::CueConfirmed, settings, state, *found, nullptr, "server");
            }
#endif
            continue;
        }
        const SyncFrame receive_frame = clock_.receive_frame();
        const SyncFrame late_frames = receive_frame > cue.frame ? receive_frame - cue.frame : 0U;
        (void)play_cue(
            registry,
            settings,
            state,
            cue,
            static_cast<float>(static_cast<double>(late_frames) * options_.fixed_dt_seconds),
            true);
    }

    for (auto played = state.played_cues.begin(); played != state.played_cues.end();) {
        if (played->confirmed || played->frame != frame) {
            ++played;
            continue;
        }
        const bool authoritative_match = std::any_of(
            cues.begin(),
            cues.end(),
            [&](const EntityCue& cue) {
                if (cue.frame != played->frame || cue.type != played->type ||
                    cue.type >= settings.cue_ops.size() || settings.cue_ops[cue.type].equals == nullptr) {
                    return false;
                }
                return settings.cue_ops[cue.type].equals(played->payload, cue.payload, nullptr);
            });
        if (authoritative_match) {
#ifdef KAGE_SYNC_ENABLE_TRACING
            const bool newly_confirmed = !played->confirmed;
#endif
            played->confirmed = true;
#ifdef KAGE_SYNC_ENABLE_TRACING
            if (newly_confirmed) {
                trace_cue_event(SyncTraceEventType::CueConfirmed, settings, state, *played, nullptr, "server");
            }
#endif
            ++played;
            continue;
        }
        (void)rollback_played_cue(registry, settings, state, *played, "server_mismatch");
        played = state.played_cues.erase(played);
    }
}

void ReplicationClient::drain_emitted_prediction_cues(
    ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    bool play) {
    if (!settings.cue_queue) {
        return;
    }
    std::vector<QueuedSyncCue> cues;
    {
        std::lock_guard<std::mutex> lock(settings.cue_queue->mutex);
        cues.swap(settings.cue_queue->cues);
    }

    for (const QueuedSyncCue& emitted : cues) {
        EntityState* state = find_entity_state_for_local(emitted.entity);
        if (state == nullptr || state->mode != ReplicationClientMode::Predict) {
            continue;
        }
        EntityCue cue;
        cue.frame = emitted.frame != 0 ? emitted.frame : frame;
        cue.type = emitted.type;
        cue.relevance_seconds = emitted.relevance_seconds;
        cue.payload = emitted.payload;
#ifdef KAGE_SYNC_ENABLE_TRACING
        trace_cue_event(SyncTraceEventType::CueEmitted, settings, *state, cue, nullptr, "local_prediction");
#endif

        auto found = std::find_if(
            state->played_cues.begin(),
            state->played_cues.end(),
            [&](const EntityPlayedCue& played) {
                if (played.frame != cue.frame || played.type != cue.type ||
                    cue.type >= settings.cue_ops.size() || settings.cue_ops[cue.type].equals == nullptr) {
                    return false;
                }
                return settings.cue_ops[cue.type].equals(played.payload, cue.payload, nullptr);
            });
        if (found != state->played_cues.end()) {
            found->seen_in_resim = true;
            continue;
        }
        if (!play) {
            continue;
        }
        const SyncFrame late_frames = last_predicted_frame_ > cue.frame ? last_predicted_frame_ - cue.frame : 0U;
        (void)play_cue(
            registry,
            settings,
            *state,
            cue,
            static_cast<float>(static_cast<double>(late_frames) * options_.fixed_dt_seconds),
            false);
    }
}

void ReplicationClient::begin_cue_resimulation() {
    for (EntityState& state : entities_) {
        for (EntityPlayedCue& cue : state.played_cues) {
            cue.seen_in_resim = false;
        }
    }
}

bool ReplicationClient::finish_cue_resimulation(ecs::Registry& registry, const SyncSettings& settings) {
    bool all_valid = true;
    for (EntityState& state : entities_) {
        for (auto cue = state.played_cues.begin(); cue != state.played_cues.end();) {
            if (cue->confirmed || cue->seen_in_resim) {
                ++cue;
                continue;
            }
            all_valid = rollback_played_cue(registry, settings, state, *cue, "resim_not_replayed") && all_valid;
            cue = state.played_cues.erase(cue);
        }
    }
    return all_valid;
}

bool ReplicationClient::apply_destroy(ecs::Registry& registry, SyncFrame frame, std::uint32_t network_id) {
    EntityState* state = find_entity_state(network_id);
    if (state == nullptr) {
        const auto found_tombstone = destroy_tombstones_.find(network_id);
        if (found_tombstone != destroy_tombstones_.end()) {
            if (frame <= found_tombstone->second.frame) {
                return false;
            }
            found_tombstone->second.frame = frame;
            ++found_tombstone->second.generation;
            found_tombstone->second.age_order = destroy_tombstone_next_age_order_++;
            destroy_tombstone_ages_.push_back(
                DestroyTombstoneAgeEntry{
                    network_id,
                    found_tombstone->second.generation,
                    found_tombstone->second.age_order});
            compact_destroy_tombstone_ages();
            return true;
        }
        record_destroy_tombstone(network_id, frame);
        advance_wire_network_id_version(network_id);
        return true;
    }
    const ClientEntityNetworkId client_entity_network_id = state->client_entity_network_id;
#ifdef KAGE_SYNC_ENABLE_TRACING
    const ecs::Entity local_entity = state->local;
    const SyncArchetypeId archetype = state->archetype;
    auto trace_destroy = [&]() {
        if (tracer_ == nullptr || !tracer_->enabled()) {
            return;
        }
        SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::EntityDestroyed, client_id_, frame);
        event.server_entity = ecs::Entity{client_entity_network_id};
        event.local_entity = local_entity;
        event.client_network_id = client_entity_network_id;
        event.wire_network_id = network_id;
        event.network_version = client_entity_network_id_version(client_entity_network_id);
        event.archetype = archetype;
        event.remove = true;
        tracer_->trace(event);
    };
#endif
    if (state->mode == ReplicationClientMode::BufferedInterpolation) {
        const bool applied = apply_buffered_destroy(registry, frame, client_entity_network_id);
        if (applied) {
            record_destroy_tombstone(network_id, frame);
            advance_wire_network_id_version(network_id);
#ifdef KAGE_SYNC_ENABLE_TRACING
            trace_destroy();
#endif
        }
        return applied;
    }
    if (state->mode == ReplicationClientMode::Predict) {
        const bool applied = apply_predicted_destroy(registry, frame, client_entity_network_id);
        if (applied) {
            record_destroy_tombstone(network_id, frame);
            advance_wire_network_id_version(network_id);
#ifdef KAGE_SYNC_ENABLE_TRACING
            trace_destroy();
#endif
        }
        return applied;
    }
    if (frame <= state->frame) {
        return false;
    }
    record_destroy_tombstone(network_id, frame);
    const auto found = network_entity_indices_.find(client_entity_network_id);
    if (found != network_entity_indices_.end()) {
        erase_entity_state(registry, found->second, true);
    }
#ifdef KAGE_SYNC_ENABLE_TRACING
    trace_destroy();
#endif
    advance_wire_network_id_version(network_id);
    return true;
}

bool ReplicationClient::apply_buffered_upsert(
    ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    ClientEntityNetworkId network_id,
    SyncArchetypeId archetype,
    QuantizedFrameData& decoded) {
    if (!validate_buffered_archetype(settings, archetype)) {
        return false;
    }

    EntityState* ensured_state = find_entity_state(network_id);
    if (ensured_state == nullptr) {
        return false;
    }
    EntityState& state = *ensured_state;
    state.mode = ReplicationClientMode::BufferedInterpolation;
    state.mode_selected = true;
    state.archetype = archetype;
    state.snap_errors.clear();
    if (state.buffered_frames.empty()) {
        state.buffered_frames.resize(options_.interpolation_buffer_capacity_frames);
    }

    if (!fill_buffered_frames(settings, state, frame, true, decoded)) {
        return false;
    }

    state.baseline = decoded;
    state.frame = frame;
    state.entity_present = true;
    remember_baseline(state);
    sync_entity_memberships(state);
    (void)registry;
    return true;
}

bool ReplicationClient::apply_buffered_destroy(
    ecs::Registry& registry,
    SyncFrame frame,
    ClientEntityNetworkId network_id) {
    EntityState* state_ptr = find_entity_state(network_id);
    if (state_ptr == nullptr) {
        return false;
    }
    EntityState& state = *state_ptr;
    state.mode = ReplicationClientMode::BufferedInterpolation;
    state.mode_selected = true;
    state.snap_errors.clear();
    if (state.buffered_frames.empty()) {
        state.buffered_frames.resize(options_.interpolation_buffer_capacity_frames);
    }
    QuantizedFrameData empty;
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (state.archetype.value < settings.archetypes.size()) {
        (void)init_frame_data(settings.archetypes[state.archetype.value], empty);
    }
    if (!fill_buffered_frames(settings, state, frame, false, empty)) {
        return false;
    }
    state.baseline.clear();
    state.frame = frame;
    state.entity_present = false;
    remember_baseline(state);
    if (has_applied_buffered_frame_ && frame <= last_applied_buffered_frame_) {
        const std::size_t mask = state.buffered_frames.size() - 1U;
        const EntityBufferedFrame& sample = state.buffered_frames[frame & mask];
        if (!sample.valid || sample.frame != frame || !apply_buffered_sample(registry, settings, state, sample)) {
            return false;
        }
    }
    sync_entity_memberships(state);
    return true;
}

bool ReplicationClient::validate_buffered_archetype(const SyncSettings& settings, SyncArchetypeId archetype) const {
    if (archetype.value >= settings.archetypes.size()) {
        return false;
    }
    const SyncArchetype& definition = settings.archetypes[archetype.value];
    for (std::size_t index = 0; index < definition.components.size(); ++index) {
        const ComponentReplication& replication = definition.components[index];
        if (replication.interpolation != ComponentInterpolation::Interpolate) {
            continue;
        }
        if (index >= definition.component_ops.size() || definition.component_ops[index].interpolate == nullptr) {
            return false;
        }
    }
    return true;
}

bool ReplicationClient::validate_predicted_archetype(const SyncSettings& settings, SyncArchetypeId archetype) const {
    if (archetype.value >= settings.archetypes.size()) {
        return false;
    }
    const SyncArchetype& definition = settings.archetypes[archetype.value];
    for (std::size_t index = 0; index < definition.components.size(); ++index) {
        if (index >= definition.component_ops.size() ||
            definition.component_ops[index].should_roll_back == nullptr) {
            throw ClientError(
                ClientStatus::MissingPredictionRollbackTrait,
                "predicted replicated components must define SyncComponentTraits<T>::should_roll_back");
        }
    }
    return true;
}

bool ReplicationClient::apply_predicted_upsert(
    ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    ClientEntityNetworkId network_id,
    SyncArchetypeId archetype,
    QuantizedFrameData& authoritative,
    bool full) {
    (void)full;
    if (!validate_predicted_archetype(settings, archetype)) {
        return false;
    }

    EntityState* state_ptr = find_entity_state(network_id);
    if (state_ptr == nullptr) {
        return false;
    }
    EntityState& state = *state_ptr;
    if (state.predicted_frames.empty()) {
        state.predicted_frames.resize(options_.prediction_buffer_capacity_frames);
    }
    if (state.archetype.value < settings.archetypes.size() &&
        state.local && registry.alive(state.local) && state.archetype != archetype) {
        remove_archetype_tags(registry, state.local, settings.archetypes[state.archetype.value]);
    }
    state.mode = ReplicationClientMode::Predict;
    state.mode_selected = true;
    state.archetype = archetype;
    state.entity_present = true;

    const bool first_authoritative = state.frame == 0 || !state.local || !registry.alive(state.local);
    const bool has_prediction = !state.predicted_frames.empty() &&
        state.predicted_frames[frame & (state.predicted_frames.size() - 1U)].valid &&
        state.predicted_frames[frame & (state.predicted_frames.size() - 1U)].frame == frame;
    if (has_prediction && compare_predicted_frame(settings, state, frame, authoritative)) {
        queue_prediction_rollback(state, frame);
    }

    state.baseline = authoritative;
    state.applied_present_mask = state.baseline.present_mask;
    state.frame = frame;
    remember_baseline(state);

    if (first_authoritative) {
        if (!apply_frame_data(registry, settings, state, frame, true, authoritative)) {
            return false;
        }
    }
    if (!has_predicted_frame_) {
        if (!fill_input_frames_through(registry, settings, frame)) {
            return false;
        }
        clock_.advance_input_frame_to(frame);
        last_predicted_frame_ = frame != 0U ? frame - 1U : 0U;
        has_predicted_frame_ = true;
    }
    sync_entity_memberships(state);
    return true;
}

bool ReplicationClient::apply_predicted_destroy(
    ecs::Registry& registry,
    SyncFrame frame,
    ClientEntityNetworkId network_id) {
    EntityState* state_ptr = find_entity_state(network_id);
    if (state_ptr == nullptr) {
        return false;
    }
    EntityState& state = *state_ptr;
    if (frame <= state.frame) {
        return false;
    }
    const std::uint32_t entity_index = static_cast<std::uint32_t>(state_ptr - entities_.data());
    erase_entity_state(registry, entity_index, true);
    return true;
}

bool ReplicationClient::fill_buffered_frames(
    const SyncSettings& settings,
    EntityState& state,
    SyncFrame frame,
    bool entity_present,
    QuantizedFrameData& decoded) {
    if (state.frame != 0 && frame <= state.frame) {
        return false;
    }

    const QuantizedFrameData* from = state.frame != 0 ? &state.baseline : nullptr;
    const bool from_entity_present = state.local || (from != nullptr && from->present_mask != 0U);
    const SyncFrame from_frame = state.frame;
    const SyncFrame begin = state.frame != 0 ? state.frame + 1U : frame;
    for (SyncFrame current = begin; current <= frame; ++current) {
        const bool current_present = current == frame ? entity_present : from_entity_present;
        const QuantizedFrameData* to = entity_present ? &decoded : nullptr;
        const bool final_absent = current == frame && !entity_present;
        if (!write_buffered_frame(
                settings,
                state,
                current,
                final_absent ? false : current_present,
                from,
                to,
                from_frame,
                frame)) {
            return false;
        }
        if (current == frame) {
            break;
        }
    }
    return true;
}

bool ReplicationClient::write_buffered_frame(
    const SyncSettings& settings,
    EntityState& state,
    SyncFrame frame,
    bool entity_present,
    const QuantizedFrameData* from,
    const QuantizedFrameData* to,
    SyncFrame from_frame,
    SyncFrame to_frame) {
    const std::size_t mask = state.buffered_frames.size() - 1U;
    EntityBufferedFrame& sample = state.buffered_frames[frame & mask];
    sample.frame = frame;
    sample.valid = true;
    sample.entity_present = entity_present;
    sample.archetype = state.archetype;
    sample.cues.clear();
    const SyncArchetype& archetype = settings.archetypes[state.archetype.value];
    if (!init_frame_data(archetype, sample.baseline)) {
        return false;
    }

    if (!entity_present) {
        return true;
    }

    const bool final_frame = frame == to_frame;
    if (final_frame && to != nullptr) {
        sample.baseline = *to;
        return true;
    }
    if (from == nullptr) {
        return true;
    }
    sample.baseline.tag_mask = from->tag_mask;

    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (!frame_has_component(*from, component_index)) {
            continue;
        }
        const SyncComponentOps& ops = archetype.component_ops[component_index];
        const std::uint8_t* previous = frame_component_data(archetype, *from, component_index);
        std::uint8_t* value = mutable_frame_component_data(archetype, sample.baseline, component_index);
        if (previous == nullptr || value == nullptr) {
            return false;
        }
        std::memcpy(value, previous, ops.quantized_size);
        if (to != nullptr &&
            frame_has_component(*to, component_index) &&
            archetype.components[component_index].interpolation == ComponentInterpolation::Interpolate) {
            if (component_index >= archetype.component_ops.size() ||
                ops.interpolate == nullptr ||
                to_frame == from_frame) {
                return false;
            }
            const std::uint8_t* next = frame_component_data(archetype, *to, component_index);
            if (next == nullptr) {
                return false;
            }
            const float alpha =
                static_cast<float>(frame - from_frame) / static_cast<float>(to_frame - from_frame);
            if (!ops.interpolate(previous, next, alpha, value)) {
                return false;
            }
        }
    }

    return true;
}

bool ReplicationClient::apply_buffered_sample(
    ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const EntityBufferedFrame& sample) {
    if (!sample.entity_present) {
        if (state.local && registry.alive(state.local)) {
            registry.destroy(state.local);
        }
        state.local = ecs::Entity{};
        state.applied_present_mask = 0;
        state.snap_errors.clear();
        return true;
    }

    if (!state.local || !registry.alive(state.local)) {
        state.local = registry.create();
    }

    const SyncArchetype& archetype = settings.archetypes[sample.archetype.value];
    if (state.archetype != sample.archetype && state.archetype.value < settings.archetypes.size()) {
        remove_archetype_tags(registry, state.local, settings.archetypes[state.archetype.value]);
    }
#ifdef KAGE_SYNC_ENABLE_TRACING
    std::uint64_t previous_tag_mask = 0;
    if (tracer_ != nullptr && tracer_->enabled()) {
        for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
            if (registry.has(state.local, archetype.tags[tag_index].tag)) {
                previous_tag_mask |= std::uint64_t{1} << tag_index;
            }
        }
    }
#endif
    if (!apply_archetype_tags(registry, state.local, archetype, sample.baseline.tag_mask)) {
        return false;
    }
    if (!archetype.tags.empty()) {
        std::uint64_t after_tag_mask = 0;
        for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
            if (registry.has(state.local, archetype.tags[tag_index].tag)) {
                after_tag_mask |= std::uint64_t{1} << tag_index;
            }
        }
        if (after_tag_mask != sample.baseline.tag_mask) {
            std::cerr << "sync client buffered tag apply frame=" << sample.frame
                      << " client=" << client_id_
                      << " network=" << state.client_entity_network_id
                      << " local=" << state.local.value
                      << " archetype=" << sample.archetype.value
                      << " sample_mask=0x" << std::hex << sample.baseline.tag_mask
                      << " registry_mask=0x" << after_tag_mask << std::dec
                      << " tag_count=" << archetype.tags.size()
                      << '\n';
        }
    }
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (tracer_ != nullptr && tracer_->enabled() && previous_tag_mask != sample.baseline.tag_mask) {
        for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
            const std::uint64_t bit = std::uint64_t{1} << tag_index;
            if ((previous_tag_mask & bit) == (sample.baseline.tag_mask & bit)) {
                continue;
            }
            SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::TagApplied, client_id_, sample.frame);
            event.server_entity = ecs::Entity{state.client_entity_network_id};
            event.local_entity = state.local;
            event.client_network_id = state.client_entity_network_id;
            event.wire_network_id = state.wire_network_id;
            event.network_version = client_entity_network_id_version(state.client_entity_network_id);
            event.archetype = sample.archetype;
            event.tag = archetype.tags[tag_index].tag;
            event.remove = (sample.baseline.tag_mask & bit) == 0U;
            tracer_->trace(event);
        }
    }
#endif
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        const std::uint64_t bit = std::uint64_t{1} << component_index;
        if ((state.applied_present_mask & bit) != 0U &&
            (sample.baseline.present_mask & bit) == 0U) {
            registry.remove(state.local, archetype.components[component_index].component);
#ifdef KAGE_SYNC_ENABLE_TRACING
            if (tracer_ != nullptr && tracer_->enabled()) {
                SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::ComponentRemoved, client_id_, sample.frame);
                event.server_entity = ecs::Entity{state.client_entity_network_id};
                event.local_entity = state.local;
                event.client_network_id = state.client_entity_network_id;
                event.wire_network_id = state.wire_network_id;
                event.network_version = client_entity_network_id_version(state.client_entity_network_id);
                event.archetype = sample.archetype;
                event.component = archetype.components[component_index].component;
                event.remove = true;
                append_trace_component_name(archetype, component_index, event);
                tracer_->trace(event);
            }
#endif
        }
    }

    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (!frame_has_component(sample.baseline, component_index) ||
            component_index >= archetype.component_ops.size()) {
            continue;
        }
        const SyncComponentOps& ops = archetype.component_ops[component_index];
        const std::uint8_t* bytes = frame_component_data(archetype, sample.baseline, component_index);
        if (bytes == nullptr) {
            return false;
        }
        if (ops.apply == nullptr || !ops.apply(registry, state.local, bytes)) {
            return false;
        }
    }

    state.applied_present_mask = sample.baseline.present_mask;
    state.archetype = sample.archetype;
    return true;
}

bool ReplicationClient::apply_frame_data(
    ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    SyncFrame frame,
    bool entity_present,
    const QuantizedFrameData& baseline) {
    EntityBufferedFrame sample;
    sample.frame = frame;
    sample.valid = true;
    sample.entity_present = entity_present;
    sample.archetype = state.archetype;
    sample.baseline = baseline;
    return apply_buffered_sample(registry, settings, state, sample);
}

bool ReplicationClient::quantize_predicted_entity(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    SyncFrame frame) {
    if (state.archetype.value >= settings.archetypes.size()) {
        return false;
    }
    if (state.predicted_frames.empty()) {
        state.predicted_frames.resize(options_.prediction_buffer_capacity_frames);
    }

    const SyncArchetype& archetype = settings.archetypes[state.archetype.value];
    EntityBufferedFrame& sample = state.predicted_frames[frame & (state.predicted_frames.size() - 1U)];
    sample.frame = frame;
    sample.valid = true;
    sample.entity_present = state.local && registry.alive(state.local);
    sample.archetype = state.archetype;
    if (!init_frame_data(archetype, sample.baseline)) {
        return false;
    }
    if (!sample.entity_present) {
        return true;
    }

    for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
        if (registry.has(state.local, archetype.tags[tag_index].tag)) {
            sample.baseline.tag_mask |= std::uint64_t{1} << tag_index;
        }
    }

    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        const ComponentReplication& replication = archetype.components[component_index];
        const void* value = registry.get(state.local, replication.component);
        if (value == nullptr) {
            continue;
        }
        if (component_index >= archetype.component_ops.size()) {
            return false;
        }
        const SyncComponentOps& ops = archetype.component_ops[component_index];
        if (ops.quantize == nullptr) {
            return false;
        }
        std::uint8_t* out = unchecked_mutable_frame_component_data(archetype, sample.baseline, component_index);
        ops.quantize(value, out);
    }
    return true;
}

bool ReplicationClient::compare_predicted_frame(
    const SyncSettings& settings,
    EntityState& state,
    SyncFrame frame,
    const QuantizedFrameData& authoritative) const {
#ifdef KAGE_SYNC_ENABLE_TRACING
#define TRACE_ROLLBACK_CONFLICT_IF(condition)                                                          \
    do {                                                                                               \
        if (condition) {                                                                               \
            if (tracer_ != nullptr && tracer_->enabled()) {                                            \
                SyncTraceEvent event =                                                                 \
                    make_client_trace_event(SyncTraceEventType::PredictionRollbackConflict, client_id_, frame); \
                event.server_entity = ecs::Entity{state.client_entity_network_id};                     \
                event.local_entity = state.local;                                                      \
                event.client_network_id = state.client_entity_network_id;                              \
                event.wire_network_id = state.wire_network_id;                                         \
                event.network_version = client_entity_network_id_version(state.client_entity_network_id); \
                event.archetype = state.archetype;                                                     \
                event.component = rollback_trace_component;                                            \
                if (rollback_trace_component_index != invalid_component_index &&                        \
                    rollback_trace_component_bytes != nullptr) {                                        \
                    append_trace_component_data(                                                       \
                        tracer_,                                                                       \
                        archetype,                                                                     \
                        rollback_trace_component_index,                                                \
                        rollback_trace_component_bytes,                                                \
                        event);                                                                        \
                }                                                                                      \
                tracer_->trace(event);                                                                 \
            }                                                                                          \
            return true;                                                                               \
        }                                                                                              \
    } while (false)
#else
#define TRACE_ROLLBACK_CONFLICT_IF(condition) \
    do {                                     \
        if (condition) {                     \
            return true;                     \
        }                                    \
    } while (false)
#endif
    if (state.archetype.value >= settings.archetypes.size() || state.predicted_frames.empty()) {
        return false;
    }
    const EntityBufferedFrame& predicted =
        state.predicted_frames[frame & (state.predicted_frames.size() - 1U)];
    if (!predicted.valid || predicted.frame != frame) {
        return false;
    }
    if (!predicted.entity_present) {
        return true;
    }
    const SyncArchetype& archetype = settings.archetypes[state.archetype.value];
#ifdef KAGE_SYNC_ENABLE_TRACING
    constexpr std::size_t invalid_component_index = static_cast<std::size_t>(-1);
    std::size_t rollback_trace_component_index = invalid_component_index;
    ecs::Entity rollback_trace_component{};
    const std::uint8_t* rollback_trace_component_bytes = nullptr;
#endif
    TRACE_ROLLBACK_CONFLICT_IF(predicted.baseline.tag_mask != authoritative.tag_mask);
    TRACE_ROLLBACK_CONFLICT_IF(predicted.baseline.present_mask != authoritative.present_mask);
    TRACE_ROLLBACK_CONFLICT_IF(predicted.baseline.bytes.size() < archetype.total_quantized_bytes);
    TRACE_ROLLBACK_CONFLICT_IF(authoritative.bytes.size() < archetype.total_quantized_bytes);
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (!frame_has_component(authoritative, component_index)) {
            continue;
        }
        TRACE_ROLLBACK_CONFLICT_IF(component_index >= archetype.component_ops.size());
        const SyncComponentOps& ops = archetype.component_ops[component_index];
        if (ops.should_roll_back == nullptr) {
            throw ClientError(
                ClientStatus::MissingPredictionRollbackTrait,
                "predicted replicated components must define SyncComponentTraits<T>::should_roll_back");
        }
        const std::uint8_t* predicted_bytes =
            unchecked_frame_component_data(archetype, predicted.baseline, component_index);
        const std::uint8_t* authoritative_bytes =
            unchecked_frame_component_data(archetype, authoritative, component_index);
#ifdef KAGE_SYNC_ENABLE_TRACING
        rollback_trace_component_index = component_index;
        rollback_trace_component = archetype.components[component_index].component;
        rollback_trace_component_bytes = authoritative_bytes;
        RollbackReasonTraceContext rollback_reason_context;
        rollback_reason_context.tracer = tracer_;
        rollback_reason_context.client = client_id_;
        rollback_reason_context.frame = frame;
        rollback_reason_context.server_entity = ecs::Entity{state.client_entity_network_id};
        rollback_reason_context.local_entity = state.local;
        rollback_reason_context.client_network_id = state.client_entity_network_id;
        rollback_reason_context.wire_network_id = state.wire_network_id;
        rollback_reason_context.network_version = client_entity_network_id_version(state.client_entity_network_id);
        rollback_reason_context.archetype = state.archetype;
        rollback_reason_context.component = rollback_trace_component;
        rollback_reason_context.component_name = component_index < archetype.component_ops.size()
            ? archetype.component_ops[component_index].name
            : std::string{};
        ScopedRollbackReasonTraceContext scoped_rollback_reason_context(rollback_reason_context);
#endif
        TRACE_ROLLBACK_CONFLICT_IF(ops.should_roll_back(predicted_bytes, authoritative_bytes));
    }
#undef TRACE_ROLLBACK_CONFLICT_IF
    return false;
}

void ReplicationClient::queue_prediction_rollback(EntityState& state, SyncFrame frame) {
    const std::uint32_t entity_index = static_cast<std::uint32_t>(&state - entities_.data());
    if (!state.prediction_rollback_pending) {
        set_prediction_rollback_membership(entity_index, true);
    }
    state.prediction_rollback_pending = true;
    state.prediction_rollback_frame = state.prediction_rollback_frame == 0
        ? frame
        : std::min(state.prediction_rollback_frame, frame);
    if (!has_pending_prediction_rollback_ || frame < pending_prediction_rollback_frame_) {
        pending_prediction_rollback_frame_ = frame;
        has_pending_prediction_rollback_ = true;
    }
}

void ReplicationClient::collect_resimulated_prediction_entities(std::vector<std::uint32_t>& out) const {
    out.clear();
    const bool all = options_.rollback_policy == ReplicationRollbackPolicy::All;
    if (!all) {
        out.reserve(prediction_rollback_entities_.size());
        for (const std::uint32_t entity_index : prediction_rollback_entities_) {
            if (entity_index < entities_.size()) {
                const EntityState& state = entities_[entity_index];
                if (state.mode == ReplicationClientMode::Predict && state.prediction_rollback_pending) {
                    out.push_back(entity_index);
                }
            }
        }
        return;
    }

    out.reserve(active_entities_.size());
    for (const std::uint32_t entity_index : active_entities_) {
        if (entity_index >= entities_.size()) {
            continue;
        }
        const EntityState& state = entities_[entity_index];
        if (state.mode == ReplicationClientMode::Predict && (all || state.prediction_rollback_pending)) {
            out.push_back(entity_index);
        }
    }
}

void ReplicationClient::capture_original_current_predictions(
    SyncFrame current_frame,
    const std::vector<std::uint32_t>& entity_indices,
    std::vector<OriginalPredictionCapture>& out) const {
    out.clear();
    out.reserve(entity_indices.size());
    for (const std::uint32_t entity_index : entity_indices) {
        if (entity_index >= entities_.size()) {
            continue;
        }
        const EntityState& state = entities_[entity_index];
        if (state.mode != ReplicationClientMode::Predict || state.predicted_frames.empty()) {
            continue;
        }
        const EntityBufferedFrame& sample =
            state.predicted_frames[current_frame & (state.predicted_frames.size() - 1U)];
        if (sample.valid && sample.frame == current_frame && sample.entity_present) {
            out.push_back(OriginalPredictionCapture{entity_index, sample.baseline});
        }
    }
}

bool ReplicationClient::blend_resim_errors(
    const ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame current_frame,
    const std::vector<OriginalPredictionCapture>& original) {
    for (const OriginalPredictionCapture& capture : original) {
        const std::uint32_t entity_index = capture.entity_index;
        if (entity_index >= entities_.size()) {
            continue;
        }
        EntityState& state = entities_[entity_index];
        if (state.mode != ReplicationClientMode::Predict || state.predicted_frames.empty() ||
            state.archetype.value >= settings.archetypes.size()) {
            continue;
        }
        const EntityBufferedFrame& resimmed =
            state.predicted_frames[current_frame & (state.predicted_frames.size() - 1U)];
        if (!resimmed.valid || resimmed.frame != current_frame || !resimmed.entity_present ||
            capture.baseline.bytes.empty()) {
            continue;
        }

        const SyncArchetype& archetype = settings.archetypes[state.archetype.value];
        if (resimmed.baseline.bytes.size() < archetype.total_quantized_bytes ||
            capture.baseline.bytes.size() < archetype.total_quantized_bytes) {
            return false;
        }
        state.snap_errors.clear();
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            const ComponentReplication& replication = archetype.components[component_index];
            if (!registry.has<FractionalTickSampled>(replication.component) ||
                component_index >= archetype.component_ops.size()) {
                continue;
            }
            const SyncComponentOps& ops = archetype.component_ops[component_index];
            if (ops.compute_error == nullptr || ops.apply_error == nullptr || ops.blend_out_error == nullptr ||
                !frame_has_component(capture.baseline, component_index) ||
                !frame_has_component(resimmed.baseline, component_index)) {
                continue;
            }
            const std::uint8_t* resimmed_bytes =
                unchecked_frame_component_data(archetype, resimmed.baseline, component_index);
            const std::uint8_t* original_bytes =
                unchecked_frame_component_data(archetype, capture.baseline, component_index);
            SyncComponentOps::QuantizedBytes error;
            if (!ops.compute_error(resimmed_bytes, original_bytes, error)) {
                return false;
            }
            if (error.size() != ops.error_size) {
                return false;
            }
            if (!all_zero(error)) {
                state.snap_errors.push_back(client_detail::EntityComponentError{replication.component, std::move(error)});
            }
        }
        sync_entity_memberships(state);
    }
    return true;
}

bool ReplicationClient::apply_snap_sample(
    ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const QuantizedFrameData& decoded,
    bool full) {
    if (!state.local || !registry.alive(state.local)) {
        state.local = registry.create();
    }

    const SyncArchetype& archetype = settings.archetypes[state.archetype.value];
    if (state.baseline.bytes.size() != archetype.total_quantized_bytes &&
        !init_frame_data(archetype, state.baseline)) {
        return false;
    }
#ifdef KAGE_SYNC_ENABLE_TRACING
    std::uint64_t previous_tag_mask = 0;
    if (tracer_ != nullptr && tracer_->enabled()) {
        for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
            if (registry.has(state.local, archetype.tags[tag_index].tag)) {
                previous_tag_mask |= std::uint64_t{1} << tag_index;
            }
        }
    }
#endif
    if (!apply_archetype_tags(registry, state.local, archetype, decoded.tag_mask)) {
        return false;
    }
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (tracer_ != nullptr && tracer_->enabled() && previous_tag_mask != decoded.tag_mask) {
        for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
            const std::uint64_t bit = std::uint64_t{1} << tag_index;
            if ((previous_tag_mask & bit) == (decoded.tag_mask & bit)) {
                continue;
            }
            SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::TagApplied, client_id_, state.frame);
            event.server_entity = ecs::Entity{state.client_entity_network_id};
            event.local_entity = state.local;
            event.client_network_id = state.client_entity_network_id;
            event.wire_network_id = state.wire_network_id;
            event.network_version = client_entity_network_id_version(state.client_entity_network_id);
            event.archetype = state.archetype;
            event.tag = archetype.tags[tag_index].tag;
            event.remove = (decoded.tag_mask & bit) == 0U;
            tracer_->trace(event);
        }
    }
#endif
    if (full) {
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            const std::uint64_t bit = std::uint64_t{1} << component_index;
            if ((state.baseline.present_mask & bit) != 0U &&
                (decoded.present_mask & bit) == 0U) {
                registry.remove(state.local, archetype.components[component_index].component);
#ifdef KAGE_SYNC_ENABLE_TRACING
                if (tracer_ != nullptr && tracer_->enabled()) {
                    SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::ComponentRemoved, client_id_, state.frame);
                    event.server_entity = ecs::Entity{state.client_entity_network_id};
                    event.local_entity = state.local;
                    event.client_network_id = state.client_entity_network_id;
                    event.wire_network_id = state.wire_network_id;
                    event.network_version = client_entity_network_id_version(state.client_entity_network_id);
                    event.archetype = state.archetype;
                    event.component = archetype.components[component_index].component;
                    event.remove = true;
                    append_trace_component_name(archetype, component_index, event);
                    tracer_->trace(event);
                }
#endif
            }
        }

        state.snap_errors.erase(
            std::remove_if(
                state.snap_errors.begin(),
                state.snap_errors.end(),
                [&](const client_detail::EntityComponentError& existing) {
                    const auto found_component = std::find_if(
                        archetype.components.begin(),
                        archetype.components.end(),
                        [&](const ComponentReplication& replication) {
                            return replication.component == existing.component;
                        });
                    if (found_component == archetype.components.end()) {
                        return true;
                    }
                    const std::size_t component_index =
                        static_cast<std::size_t>(found_component - archetype.components.begin());
                    return (decoded.present_mask & (std::uint64_t{1} << component_index)) == 0U;
                }),
            state.snap_errors.end());
    }

    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (!frame_has_component(decoded, component_index) ||
            component_index >= archetype.component_ops.size()) {
            continue;
        }
        const ComponentReplication& replication = archetype.components[component_index];
        const SyncComponentOps& ops = archetype.component_ops[component_index];
        const std::uint8_t* bytes = frame_component_data(archetype, decoded, component_index);
        if (bytes == nullptr) {
            return false;
        }
        if (ops.apply == nullptr || !ops.apply(registry, state.local, bytes)) {
            return false;
        }
        const std::uint8_t* previous_bytes = frame_component_data(archetype, state.baseline, component_index);
        const bool had_baseline = previous_bytes != nullptr;
        if (had_baseline &&
            ops.compute_error != nullptr &&
            ops.apply_error != nullptr &&
            ops.blend_out_error != nullptr) {
            SyncComponentOps::QuantizedBytes error;
            if (!ops.compute_error(bytes, previous_bytes, error)) {
                return false;
            }

            auto found_error = std::find_if(
                state.snap_errors.begin(),
                state.snap_errors.end(),
                [&](const client_detail::EntityComponentError& existing) {
                    return existing.component == replication.component;
                });
            if (all_zero(error)) {
                if (found_error != state.snap_errors.end()) {
                    state.snap_errors.erase(found_error);
                }
            } else if (found_error == state.snap_errors.end()) {
                state.snap_errors.push_back(client_detail::EntityComponentError{replication.component, std::move(error)});
            } else {
                found_error->bytes = std::move(error);
            }
        }
        std::memcpy(
            unchecked_mutable_frame_component_data(archetype, state.baseline, component_index),
            bytes,
            ops.quantized_size);
    }

    state.entity_present = true;
    state.baseline.tag_mask = decoded.tag_mask;
    state.applied_present_mask = state.baseline.present_mask;
    sync_entity_memberships(state);
    return true;
}

bool ReplicationClient::apply_latest_snap(
    ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state) {
    if (!state.entity_present) {
        if (state.local && registry.alive(state.local)) {
            registry.destroy(state.local);
        }
        state.local = ecs::Entity{};
        state.applied_present_mask = 0;
        state.snap_errors.clear();
        sync_entity_memberships(state);
        return true;
    }

    return apply_snap_sample(registry, settings, state, state.baseline, true);
}

bool ReplicationClient::switch_entity_mode(
    ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    ReplicationClientMode mode) {
    if (state.mode == mode) {
        return true;
    }

    if (mode == ReplicationClientMode::BufferedInterpolation) {
        if (!validate_buffered_archetype(settings, state.archetype)) {
            return false;
        }
        state.mode = ReplicationClientMode::BufferedInterpolation;
        state.mode_selected = true;
        state.snap_errors.clear();
        if (state.buffered_frames.empty()) {
            state.buffered_frames.resize(options_.interpolation_buffer_capacity_frames);
        }
        if (state.frame != 0) {
            if (!write_buffered_frame(
                    settings,
                    state,
                    state.frame,
                    state.entity_present,
                    nullptr,
                    state.entity_present ? &state.baseline : nullptr,
                    state.frame,
                    state.frame)) {
                return false;
            }
        }
        state.applied_present_mask = state.baseline.present_mask;
        sync_entity_memberships(state);
        return true;
    }

    if (mode == ReplicationClientMode::Predict) {
        if (!validate_predicted_archetype(settings, state.archetype)) {
            return false;
        }
        state.mode = ReplicationClientMode::Predict;
        state.mode_selected = true;
        state.snap_errors.clear();
        if (state.predicted_frames.empty()) {
            state.predicted_frames.resize(options_.prediction_buffer_capacity_frames);
        }
        if (state.frame != 0) {
            if (!apply_frame_data(registry, settings, state, state.frame, state.entity_present, state.baseline)) {
                return false;
            }
            EntityBufferedFrame& sample =
                state.predicted_frames[state.frame & (state.predicted_frames.size() - 1U)];
            sample.frame = state.frame;
            sample.valid = true;
            sample.entity_present = state.entity_present;
            sample.archetype = state.archetype;
            sample.baseline = state.baseline;
            if (state.entity_present && !has_predicted_frame_) {
                if (!fill_input_frames_through(registry, settings, state.frame)) {
                    return false;
                }
                clock_.advance_input_frame_to(state.frame);
                last_predicted_frame_ = state.frame;
                has_predicted_frame_ = true;
            }
        }
        state.buffered_frames.clear();
        sync_entity_memberships(state);
        return true;
    }

    const ReplicationClientMode previous = state.mode;
    state.mode = ReplicationClientMode::Snap;
    state.mode_selected = true;
    if (!apply_latest_snap(registry, settings, state)) {
        state.mode = previous;
        return false;
    }
    state.buffered_frames.clear();
    sync_entity_memberships(state);
    return true;
}

bool ReplicationClient::has_buffered_entities() const noexcept {
    return !buffered_entities_.empty();
}

bool ReplicationClient::has_predicted_entities() const noexcept {
    for (const std::uint32_t entity_index : active_entities_) {
        if (entity_index < entities_.size() && entities_[entity_index].mode == ReplicationClientMode::Predict) {
            return true;
        }
    }
    return false;
}

bool ReplicationClient::apply_pending_prediction_rollback(ecs::Registry& registry, ecs::RunJobsOptions options) {
    if (!has_pending_prediction_rollback_) {
        return true;
    }
    const SyncFrame begin_frame = pending_prediction_rollback_frame_;
    const SyncFrame current_frame = has_predicted_frame_ ? last_predicted_frame_ : begin_frame;
    collect_resimulated_prediction_entities(rollback_entity_indices_scratch_);
    capture_original_current_predictions(
        current_frame,
        rollback_entity_indices_scratch_,
        rollback_original_current_scratch_);
    begin_cue_resimulation();

    const bool resimmed = options_.rollback_policy == ReplicationRollbackPolicy::OnlyAffected
        ? resimulate_affected_predicted(registry, begin_frame, current_frame, options)
        : resimulate_all_predicted(registry, begin_frame, current_frame, options);
    if (!resimmed) {
        return false;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!finish_cue_resimulation(registry, settings)) {
        return false;
    }
    if (!blend_resim_errors(
            registry,
            settings,
            current_frame,
            rollback_original_current_scratch_)) {
        return false;
    }

    has_pending_prediction_rollback_ = false;
    pending_prediction_rollback_frame_ = 0;
    for (const std::uint32_t entity_index : prediction_rollback_entities_) {
        if (entity_index >= entities_.size()) {
            continue;
        }
        EntityState& state = entities_[entity_index];
        state.prediction_rollback_pending = false;
        state.prediction_rollback_frame = 0;
        state.prediction_rollback_index = invalid_ack_index;
    }
    prediction_rollback_entities_.clear();
    return true;
}

bool ReplicationClient::resimulate_all_predicted(
    ecs::Registry& registry,
    SyncFrame begin_frame,
    SyncFrame current_frame,
    ecs::RunJobsOptions options) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    for (std::uint32_t entity_index : active_entities_) {
        if (entity_index >= entities_.size()) {
            continue;
        }
        EntityState& state = entities_[entity_index];
        if (state.mode != ReplicationClientMode::Predict) {
            continue;
        }
        const QuantizedFrameData* baseline = find_baseline(state, begin_frame);
        if (baseline != nullptr) {
            if (!apply_frame_data(registry, settings, state, begin_frame, true, *baseline)) {
                return false;
            }
        }
    }
    if (current_frame <= begin_frame) {
        for (std::uint32_t entity_index : active_entities_) {
            if (entity_index >= entities_.size()) {
                continue;
            }
            EntityState& state = entities_[entity_index];
            if (state.mode == ReplicationClientMode::Predict &&
                !quantize_predicted_entity(registry, settings, state, current_frame)) {
                return false;
            }
        }
#ifdef KAGE_SYNC_ENABLE_TRACING
        trace_frame_components(registry, settings, current_frame, true, false, TraceFrameComponentScope::Predicted);
#endif
        return true;
    }

    for (SyncFrame frame = begin_frame + 1U; frame <= current_frame; ++frame) {
        if (!apply_frame(registry, frame)) {
            return false;
        }
        if (!apply_input_frame(registry, settings, frame)) {
            return false;
        }
        resim_job_graph(registry).tick(registry, options);
        drain_emitted_prediction_cues(registry, settings, frame, true);
        for (std::uint32_t entity_index : active_entities_) {
            if (entity_index >= entities_.size()) {
                continue;
            }
            EntityState& state = entities_[entity_index];
            if (state.mode == ReplicationClientMode::Predict &&
                !quantize_predicted_entity(registry, settings, state, frame)) {
                return false;
            }
        }
#ifdef KAGE_SYNC_ENABLE_TRACING
        trace_frame_components(registry, settings, frame, true, false, TraceFrameComponentScope::Predicted);
#endif
        if (frame == current_frame) {
            break;
        }
    }
    return true;
}

bool ReplicationClient::resimulate_affected_predicted(
    ecs::Registry& registry,
    SyncFrame begin_frame,
    SyncFrame current_frame,
    ecs::RunJobsOptions options) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    rollback_affected_entities_scratch_.clear();
    rollback_affected_entities_scratch_.reserve(prediction_rollback_entities_.size());
    for (std::uint32_t entity_index : active_entities_) {
        if (entity_index >= entities_.size()) {
            continue;
        }
        EntityState& state = entities_[entity_index];
        if (state.mode != ReplicationClientMode::Predict || !state.prediction_rollback_pending) {
            continue;
        }
        const QuantizedFrameData* baseline = find_baseline(state, state.prediction_rollback_frame);
        if (baseline == nullptr) {
            baseline = find_baseline(state, begin_frame);
        }
        if (baseline != nullptr &&
            !apply_frame_data(registry, settings, state, state.prediction_rollback_frame, true, *baseline)) {
            return false;
        }
        if (state.local && registry.alive(state.local)) {
            rollback_affected_entities_scratch_.push_back(state.local);
        }
    }
    if (rollback_affected_entities_scratch_.empty()) {
        return true;
    }
    if (current_frame <= begin_frame) {
        for (std::uint32_t entity_index : active_entities_) {
            if (entity_index >= entities_.size()) {
                continue;
            }
            EntityState& state = entities_[entity_index];
            if (state.mode == ReplicationClientMode::Predict && state.prediction_rollback_pending &&
                !quantize_predicted_entity(registry, settings, state, current_frame)) {
                return false;
            }
        }
#ifdef KAGE_SYNC_ENABLE_TRACING
        trace_frame_components(registry, settings, current_frame, true, true, TraceFrameComponentScope::Predicted);
#endif
        return true;
    }

    for (SyncFrame frame = begin_frame + 1U; frame <= current_frame; ++frame) {
        if (!apply_input_frame(registry, settings, frame)) {
            return false;
        }
        resim_job_graph(registry).tick_for_entities(registry, rollback_affected_entities_scratch_, options);
        drain_emitted_prediction_cues(registry, settings, frame, true);
        for (std::uint32_t entity_index : active_entities_) {
            if (entity_index >= entities_.size()) {
                continue;
            }
            EntityState& state = entities_[entity_index];
            if (state.mode == ReplicationClientMode::Predict && state.prediction_rollback_pending &&
                !quantize_predicted_entity(registry, settings, state, frame)) {
                return false;
            }
        }
#ifdef KAGE_SYNC_ENABLE_TRACING
        trace_frame_components(registry, settings, frame, true, true, TraceFrameComponentScope::Predicted);
#endif
        if (frame == current_frame) {
            break;
        }
    }
    return true;
}

const ecs::JobGraph& ReplicationClient::resim_job_graph(ecs::Registry& registry) {
    if (!resim_job_graph_valid_) {
        resim_job_graph_ = registry.compile_job_graph(simulation_jobs_);
        resim_job_graph_valid_ = true;
    }
    return resim_job_graph_;
}

void ReplicationClient::blend_snap_errors(const SyncSettings& settings, float dt_seconds) {
    for (std::size_t list_index = 0; list_index < snap_error_entities_.size();) {
        const std::uint32_t entity_index = snap_error_entities_[list_index];
        if (entity_index >= entities_.size()) {
            ++list_index;
            continue;
        }
        EntityState& state = entities_[entity_index];

        for (client_detail::EntityComponentError& error : state.snap_errors) {
            const auto found_ops = settings.component_ops.find(error.component.value);
            if (found_ops == settings.component_ops.end() || found_ops->second.blend_out_error == nullptr ||
                !found_ops->second.blend_out_error(error.bytes, dt_seconds, error.bytes)) {
                error.bytes.clear();
            }
        }

        state.snap_errors.erase(
            std::remove_if(
                state.snap_errors.begin(),
                state.snap_errors.end(),
                [](const client_detail::EntityComponentError& error) {
                    return error.bytes.empty() || all_zero(error.bytes);
                }),
            state.snap_errors.end());
        if ((state.mode != ReplicationClientMode::Snap && state.mode != ReplicationClientMode::Predict) ||
            state.snap_errors.empty()) {
            set_snap_error_membership(entity_index, false);
        } else {
            ++list_index;
        }
    }
}

bool ReplicationClient::write_fractional_tick_samples(
    const ecs::Registry& registry,
    double target_frame,
    bool include_snap,
    bool include_empty_buffered,
    FractionalTickSampleBuffer& out) const {
    out.clear();
    const bool target_valid = target_frame >= 0.0 &&
        std::isfinite(target_frame) &&
        target_frame <= static_cast<double>(std::numeric_limits<SyncFrame>::max());
    const double floor_value = target_valid ? std::floor(target_frame) : 0.0;
    const SyncFrame floor_frame = static_cast<SyncFrame>(floor_value);
    const float alpha = target_valid ? static_cast<float>(target_frame - floor_value) : 0.0f;
    const SyncSettings& settings = registry.get<SyncSettings>();
    bool all_valid = target_valid || !has_buffered_entities();
    std::size_t sampled_count = 0;
    auto previous_display = [&](ClientEntityNetworkId network_id) -> const FractionalTickSample* {
        const auto found = std::find_if(
            fractional_tick_frame_.entities.begin(),
            fractional_tick_frame_.entities.end(),
            [network_id](const FractionalTickSample& sample) {
                return sample.client_entity_network_id == network_id;
            });
        return found != fractional_tick_frame_.entities.end() ? &*found : nullptr;
    };
    auto append_previous_display = [&](ClientEntityNetworkId network_id) {
        const FractionalTickSample* previous = previous_display(network_id);
        if (previous == nullptr) {
            return false;
        }
        if (sampled_count == out.entities.size()) {
            out.entities.emplace_back();
        }
        out.entities[sampled_count++] = *previous;
        return true;
    };

    const std::vector<std::uint32_t>& entity_indices = include_snap ? active_entities_ : buffered_entities_;
    for (const std::uint32_t entity_index : entity_indices) {
        if (entity_index >= entities_.size()) {
            continue;
        }
        const EntityState& state = entities_[entity_index];
        if (state.client_entity_network_id == invalid_client_entity_network_id) {
            continue;
        }
        if (state.mode != ReplicationClientMode::BufferedInterpolation) {
            if (!include_snap || !state.local || !registry.alive(state.local)) {
                continue;
            }

            if (state.mode == ReplicationClientMode::Predict && has_predicted_frame_ && !state.predicted_frames.empty()) {
                const std::size_t mask = state.predicted_frames.size() - 1U;
                const EntityBufferedFrame& current_sample = state.predicted_frames[last_predicted_frame_ & mask];
                if (current_sample.valid && current_sample.frame == last_predicted_frame_ && current_sample.entity_present) {
                    const EntityBufferedFrame* floor_sample = &current_sample;
                    const EntityBufferedFrame* next_sample = nullptr;
                    float predicted_alpha = 0.0f;
                    if (last_predicted_frame_ != 0U) {
                        const SyncFrame previous_frame = last_predicted_frame_ - 1U;
                        const EntityBufferedFrame& candidate = state.predicted_frames[previous_frame & mask];
                        if (candidate.valid && candidate.frame == previous_frame && candidate.entity_present) {
                            floor_sample = &candidate;
                            next_sample = &current_sample;
                            predicted_alpha = static_cast<float>(
                                clock_.input_accumulator_seconds() / options_.fixed_dt_seconds);
                        }
                    }
                    if (sampled_count == out.entities.size()) {
                        out.entities.emplace_back();
                    }
                    FractionalTickSample& display = out.entities[sampled_count];
                    display.client_entity_network_id = state.client_entity_network_id;
                    display.local_entity = state.local;
                    display.frame = floor_sample->frame;
                    display.alpha = next_sample != nullptr ? predicted_alpha : 0.0f;
                    display.components_.clear();

                    const SyncArchetype& display_archetype = settings.archetypes[floor_sample->archetype.value];
                    display.components_.reserve(display_archetype.components.size());
                    for (std::size_t component_index = 0; component_index < display_archetype.components.size(); ++component_index) {
                        const ecs::Entity component = display_archetype.components[component_index].component;
                        if (!frame_has_component(floor_sample->baseline, component_index) ||
                            !registry.has<FractionalTickSampled>(component)) {
                            continue;
                        }

                        ReplicatedComponentUpdate value;
                        value.component = component;
                        const SyncComponentOps& ops = display_archetype.component_ops[component_index];
                        const std::uint8_t* floor_bytes =
                            frame_component_data(display_archetype, floor_sample->baseline, component_index);
                        if (floor_bytes == nullptr) {
                            return false;
                        }

                        if (next_sample != nullptr &&
                            next_sample->archetype == floor_sample->archetype &&
                            frame_has_component(next_sample->baseline, component_index) &&
                            display_archetype.components[component_index].interpolation == ComponentInterpolation::Interpolate) {
                            if (component_index >= display_archetype.component_ops.size() ||
                                ops.interpolate == nullptr) {
                                return false;
                            }
                            const std::uint8_t* next_bytes =
                                frame_component_data(display_archetype, next_sample->baseline, component_index);
                            value.bytes.resize(ops.quantized_size);
                            if (next_bytes == nullptr ||
                                !ops.interpolate(floor_bytes, next_bytes, predicted_alpha, value.bytes.data())) {
                                return false;
                            }
                        } else {
                            value.bytes.assign(floor_bytes, ops.quantized_size);
                        }

                        auto found_error = std::find_if(
                            state.snap_errors.begin(),
                            state.snap_errors.end(),
                            [component](const client_detail::EntityComponentError& error) {
                                return error.component == component;
                            });
                        if (found_error != state.snap_errors.end()) {
                            if (component_index >= display_archetype.component_ops.size() ||
                                ops.apply_error == nullptr ||
                                !ops.apply_error(value.bytes.data(), found_error->bytes, value.bytes)) {
                                return false;
                            }
                        }

                        display.components_.push_back(std::move(value));
                    }

                    if (include_empty_buffered || !display.components_.empty()) {
                        ++sampled_count;
                    }
                    continue;
                }
            }

            if (sampled_count == out.entities.size()) {
                out.entities.emplace_back();
            }
            FractionalTickSample& display = out.entities[sampled_count++];
            display.client_entity_network_id = state.client_entity_network_id;
            display.local_entity = state.local;
            display.frame = state.frame;
            display.alpha = 0.0f;
            display.components_.clear();
            const SyncArchetype& display_archetype = settings.archetypes[state.archetype.value];
            display.components_.reserve(display_archetype.components.size());
            for (std::size_t component_index = 0; component_index < display_archetype.components.size(); ++component_index) {
                const ecs::Entity component = display_archetype.components[component_index].component;
                if (!registry.has<FractionalTickSampled>(component)) {
                    continue;
                }
                if (component_index >= display_archetype.component_ops.size()) {
                    return false;
                }
                const SyncComponentOps& ops = display_archetype.component_ops[component_index];
                if (ops.quantize == nullptr) {
                    return false;
                }
                const void* current = registry.get(state.local, component);
                if (current == nullptr) {
                    continue;
                }

                ReplicatedComponentUpdate value;
                value.component = component;
                value.bytes.resize(ops.quantized_size);
                ops.quantize(current, value.bytes.data());
                const auto found_error = std::find_if(
                    state.snap_errors.begin(),
                    state.snap_errors.end(),
                    [component](const client_detail::EntityComponentError& error) {
                        return error.component == component;
                    });
                if (found_error != state.snap_errors.end()) {
                    if (ops.apply_error == nullptr ||
                        !ops.apply_error(value.bytes.data(), found_error->bytes, value.bytes)) {
                        return false;
                    }
                }
                display.components_.push_back(std::move(value));
            }
            continue;
        }

        if (state.buffered_frames.empty() || !target_valid) {
            if (!include_empty_buffered || !append_previous_display(state.client_entity_network_id)) {
                all_valid = false;
            }
            continue;
        }

        const std::size_t mask = state.buffered_frames.size() - 1U;
        const EntityBufferedFrame& floor_sample = state.buffered_frames[floor_frame & mask];
        if (!floor_sample.valid || floor_sample.frame != floor_frame) {
            if (!include_empty_buffered || !append_previous_display(state.client_entity_network_id)) {
                all_valid = false;
            }
            continue;
        }
        if (!floor_sample.entity_present) {
            continue;
        }

        const EntityBufferedFrame* next_sample = nullptr;
        if (alpha > 0.0f && floor_frame != std::numeric_limits<SyncFrame>::max()) {
            const SyncFrame next_frame = floor_frame + 1U;
            const EntityBufferedFrame& candidate = state.buffered_frames[next_frame & mask];
            if (candidate.valid && candidate.frame == next_frame && candidate.entity_present) {
                next_sample = &candidate;
            }
        }

        if (sampled_count == out.entities.size()) {
            out.entities.emplace_back();
        }
        FractionalTickSample& display = out.entities[sampled_count];
        display.client_entity_network_id = state.client_entity_network_id;
        display.local_entity = state.local;
        display.frame = floor_frame;
        display.alpha = alpha;
        display.components_.clear();
        const SyncArchetype& display_archetype = settings.archetypes[floor_sample.archetype.value];
        display.components_.reserve(display_archetype.components.size());

        for (std::size_t component_index = 0; component_index < display_archetype.components.size(); ++component_index) {
            const ecs::Entity component = display_archetype.components[component_index].component;
            if (!frame_has_component(floor_sample.baseline, component_index) ||
                !registry.has<FractionalTickSampled>(component)) {
                continue;
            }

            ReplicatedComponentUpdate value;
            value.component = component;
            const SyncComponentOps& ops = display_archetype.component_ops[component_index];
            const std::uint8_t* baseline_bytes =
                frame_component_data(display_archetype, floor_sample.baseline, component_index);
            if (baseline_bytes == nullptr) {
                return false;
            }

            if (next_sample != nullptr &&
                frame_has_component(next_sample->baseline, component_index) &&
                display_archetype.components[component_index].interpolation == ComponentInterpolation::Interpolate) {
                if (component_index >= display_archetype.component_ops.size() ||
                    ops.interpolate == nullptr) {
                    return false;
                }
                const std::uint8_t* next_bytes =
                    frame_component_data(display_archetype, next_sample->baseline, component_index);
                value.bytes.resize(ops.quantized_size);
                if (next_bytes == nullptr || !ops.interpolate(baseline_bytes, next_bytes, alpha, value.bytes.data())) {
                    return false;
                }
            } else {
                value.bytes.assign(baseline_bytes, ops.quantized_size);
            }
            display.components_.push_back(std::move(value));
        }

        if (include_empty_buffered || !display.components_.empty()) {
            ++sampled_count;
        }
    }

    out.entities.resize(sampled_count);
    return include_empty_buffered || all_valid;
}

ComponentInterpolation ReplicationClient::interpolation_for(
    const SyncSettings& settings,
    SyncArchetypeId archetype,
    ecs::Entity component) const {
    if (archetype.value >= settings.archetypes.size()) {
        return ComponentInterpolation::Step;
    }
    const SyncArchetype& definition = settings.archetypes[archetype.value];
    const auto found = std::find_if(
        definition.components.begin(),
        definition.components.end(),
        [component](const ComponentReplication& replication) {
            return replication.component == component;
        });
    return found != definition.components.end() ? found->interpolation : ComponentInterpolation::Step;
}

void ReplicationClient::remember_baseline(EntityState& state) {
    if (state.history.size() != max_baseline_history_per_entity) {
        state.history.clear();
        state.history.resize(max_baseline_history_per_entity);
        state.history_next = 0;
    }

    client_detail::EntityFrameBaseline& baseline = state.history[state.frame & (max_baseline_history_per_entity - 1U)];
    baseline.frame = state.frame;
    baseline.valid = true;
    baseline.baseline = state.baseline;
}

void ReplicationClient::queue_ack(std::uint32_t packet_id) {
    ack_queue_->push(packet_id);
}

void ReplicationClient::record_server_packet_sequence(std::uint32_t packet_id) noexcept {
    client_detail::record_packet_window(
        packet_id,
        options_.protocol.max_pending_packet_acks_per_client,
        clock_.mutable_stats(),
        highest_server_update_packet_id_,
        server_update_packet_window_mask_,
        server_update_packet_window_span_,
        has_server_update_packet_window_);
}

bool ReplicationClient::receive_connect_response(ecs::Registry& registry, ecs::BitBuffer& packet) {
    if (packet.remaining_bits() < 1U) {
        return false;
    }
    const bool accepted = packet.read_bool();
    if (accepted) {
        if (packet.remaining_bits() < 64U) {
            return false;
        }
        client_id_ = static_cast<ClientId>(packet.read_unsigned_bits(64U));
        if (client_id_ == invalid_client_id) {
            return false;
        }
        connect_error_.clear();
        connection_state_ = ReplicationClientConnectionState::Accepted;
        connect_resend_accumulator_seconds_ = options_.connect_resend_interval_seconds;
        configure_client(registry, client_id_);
#ifdef KAGE_SYNC_ENABLE_TRACING
        if (tracer_ != nullptr && tracer_->enabled()) {
            SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::ClientConnected, client_id_, clock_.receive_frame());
            tracer_->trace(event);
        }
#endif
        return true;
    }

    std::string error;
    if (!protocol::read_string(packet, error)) {
        return false;
    }
    client_id_ = invalid_client_id;
    connect_error_ = std::move(error);
    connection_state_ = ReplicationClientConnectionState::Rejected;
#ifdef KAGE_SYNC_ENABLE_TRACING
    if (tracer_ != nullptr && tracer_->enabled()) {
        SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::ClientDisconnected, invalid_client_id, clock_.receive_frame());
        event.data = connect_error_;
        tracer_->trace(event);
    }
#endif
    return true;
}

bool ReplicationClient::receive_pong(ecs::Registry& registry, ecs::BitBuffer& packet, SyncFrame receive_frame) {
    if (packet.remaining_bits() < 64U) {
        return false;
    }
    const auto sequence = static_cast<std::uint32_t>(packet.read_bits(32U));
    const auto send_frame = static_cast<SyncFrame>(packet.read_bits(32U));
    std::uint16_t send_subframe = 0;
    if (packet.remaining_bits() >= protocol::frame_subframe_bits) {
        send_subframe = static_cast<std::uint16_t>(packet.read_bits(protocol::frame_subframe_bits));
    }
    SyncFrame server_frame = 0;
    std::uint16_t server_subframe = 0;
    if (packet.remaining_bits() >= 32U + protocol::frame_subframe_bits) {
        server_frame = static_cast<SyncFrame>(packet.read_bits(32U));
        server_subframe = static_cast<std::uint16_t>(packet.read_bits(protocol::frame_subframe_bits));
    }
    const auto found = pending_pings_.find(sequence);
    if (found == pending_pings_.end() ||
        found->second.frame != send_frame ||
        found->second.subframe != send_subframe ||
        receive_frame < send_frame) {
        return false;
    }
    pending_pings_.erase(found);
    (void)server_frame;
    (void)server_subframe;
    const double sent_continuous_frame = decode_continuous_frame(send_frame, send_subframe);
    const double received_continuous_frame = receive_frame == clock_.receive_frame()
        ? clock_.continuous_receive_frame()
        : static_cast<double>(receive_frame);
    const float sample = static_cast<float>((received_continuous_frame - sent_continuous_frame) * 0.5);
    if (options_.adaptive_ping_interval) {
        if (clock_.stats().sample_count == 0U) {
            stable_ping_samples_ = 1U;
        } else {
            const float delta = std::fabs(sample - clock_.stats().latency_frames);
            if (delta >= options_.adaptive_ping_jump_threshold_frames) {
                adaptive_ping_active_ = true;
                stable_ping_samples_ = 0U;
            } else if (delta <= options_.adaptive_ping_stable_threshold_frames) {
                ++stable_ping_samples_;
                if (stable_ping_samples_ >= options_.adaptive_ping_stable_samples) {
                    adaptive_ping_active_ = false;
                }
            } else {
                stable_ping_samples_ = 0U;
            }
        }
    }
    const ReplicationClientTimingStats before = clock_.stats();
    clock_.record_continuous_pong(sent_continuous_frame, received_continuous_frame);
    const ReplicationClientTimingStats& after = clock_.stats();
    if (options_.auto_timing_fast_recovery &&
        options_.auto_prediction_lead_frames &&
        has_received_server_update_ &&
        after.target_prediction_lead_frames > before.current_prediction_lead_frames &&
        after.target_prediction_lead_frames - before.current_prediction_lead_frames >=
            options_.auto_timing_fast_recovery_min_frame_gap) {
        const SyncFrame prefill_input_frame = last_server_update_frame_ + after.target_prediction_lead_frames;
#ifdef KAGE_SYNC_ENABLE_TRACING
        trace_clock_skew(
            "pong_requested_prefill",
            receive_frame,
            last_server_update_frame_,
            receive_frame,
            clock_.playback_frame(),
            input_->last_recorded_frame(),
            prefill_input_frame);
#endif
        if (prefill_input_frame > input_->last_recorded_frame()) {
            const SyncSettings& settings = registry.get<SyncSettings>();
            (void)fill_input_frames_through(registry, settings, prefill_input_frame);
            active_prediction_snap_lead_frames_ = after.target_prediction_lead_frames;
            if (prefill_input_frame > pending_prediction_catchup_frame_) {
                pending_prediction_catchup_frame_ = prefill_input_frame;
                pending_prediction_catchup_server_frame_ = last_server_update_frame_;
            }
            if (!has_predicted_entities()) {
                clock_.advance_input_frame_to(prefill_input_frame);
            }
            clock_.record_prediction_lead(last_server_update_frame_, prefill_input_frame);
#ifdef KAGE_SYNC_ENABLE_TRACING
            trace_clock_skew(
                "pong_prefill_applied",
                receive_frame,
                last_server_update_frame_,
                receive_frame,
                clock_.playback_frame(),
                input_->last_recorded_frame(),
                prefill_input_frame);
#endif
        }
    }
    return true;
}

void ReplicationClient::drain_connect_packets(std::vector<ecs::BitBuffer>& packets) {
    if (connection_state_ == ReplicationClientConnectionState::Connecting) {
        if (sent_initial_connect_request_ &&
            connect_resend_accumulator_seconds_ < options_.connect_resend_interval_seconds) {
            return;
        }

        ecs::BitBuffer packet;
        packet.reserve_bytes(options_.mtu_bytes);
        packet.push_bits(protocol::client_connect_request_message, 8U);
        protocol::write_string(packet, options_.connect_token);
        if (packet.byte_size() <= options_.mtu_bytes) {
            packets.push_back(std::move(packet));
            sent_initial_connect_request_ = true;
            connect_resend_accumulator_seconds_ = 0.0;
        }
        return;
    }

    if (connection_state_ == ReplicationClientConnectionState::Accepted && client_id_ != invalid_client_id) {
        if (connect_resend_accumulator_seconds_ < options_.connect_resend_interval_seconds) {
            return;
        }

        ecs::BitBuffer packet;
        packet.reserve_bytes(options_.mtu_bytes);
        packet.push_bits(protocol::client_connect_ack_message, 8U);
        packet.push_unsigned_bits(client_id_, 64U);
        packets.push_back(std::move(packet));
        connect_resend_accumulator_seconds_ = 0.0;
    }
}

void ReplicationClient::drain_ping_packets(std::vector<ecs::BitBuffer>& packets) {
    if (connection_state_ != ReplicationClientConnectionState::Accepted &&
        connection_state_ != ReplicationClientConnectionState::Ready) {
        return;
    }
    const double interval_seconds =
        options_.adaptive_ping_interval && adaptive_ping_active_
            ? options_.adaptive_ping_interval_seconds
            : options_.ping_interval_seconds;
    if (sent_initial_ping_ && ping_accumulator_seconds_ < interval_seconds) {
        return;
    }

    const std::uint32_t sequence = next_ping_sequence_++;
    const SyncFrame send_frame = clock_.receive_frame();
    const std::uint16_t send_subframe = encode_subframe(clock_.receive_subframe());
    pending_pings_[sequence] = PendingPing{send_frame, send_subframe};

    ecs::BitBuffer packet;
    packet.reserve_bytes(options_.mtu_bytes);
    packet.push_bits(protocol::client_ping_message, 8U);
    packet.push_bits(sequence, 32U);
    packet.push_bits(send_frame, 32U);
    packet.push_bits(send_subframe, protocol::frame_subframe_bits);
    packets.push_back(std::move(packet));
    sent_initial_ping_ = true;
    ping_accumulator_seconds_ = 0.0;
}

}  // namespace kage::sync
