#include "kage/sync/client.hpp"

#include "kage/sync/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace kage::sync {
namespace {

constexpr std::size_t max_baseline_history_per_entity = 64;

std::size_t configured_packet_id_bits(const ReplicationClientOptions& options) noexcept {
    return protocol::packet_id_bits_for_max_pending(options.max_pending_packet_acks_per_client);
}

bool is_power_of_two(std::size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

bool all_zero(const SyncComponentOps::QuantizedBytes& bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) {
        return byte == 0U;
    });
}

bool init_frame_data(const SyncArchetype& archetype, QuantizedFrameData& frame) {
    if (archetype.tags.size() > 64U ||
        archetype.components.size() > 63U ||
        archetype.component_offsets.size() != archetype.components.size() ||
        archetype.component_ops.size() != archetype.components.size()) {
        return false;
    }
    frame.tag_mask = 0;
    frame.present_mask = 0;
    frame.bytes.assign(archetype.total_quantized_bytes, 0U);
    return true;
}

std::size_t sync_slot_count(const SyncArchetype& archetype) noexcept {
    return archetype.components.size() + 1U;
}

bool has_tag_slot(const SyncArchetype& archetype) noexcept {
    return !archetype.tags.empty();
}

std::uint64_t sync_slot_bit(std::size_t slot) noexcept {
    return std::uint64_t{1} << slot;
}

bool frame_has_component(const QuantizedFrameData& frame, std::size_t component_index) {
    return component_index < 64U && (frame.present_mask & (std::uint64_t{1} << component_index)) != 0U;
}

bool frame_component_bytes(
    const SyncArchetype& archetype,
    const QuantizedFrameData& frame,
    std::size_t component_index,
    SyncComponentOps::QuantizedBytes& out) {
    if (!frame_has_component(frame, component_index) ||
        component_index >= archetype.component_offsets.size() ||
        component_index >= archetype.component_ops.size()) {
        return false;
    }
    const std::size_t offset = archetype.component_offsets[component_index];
    const std::size_t size = archetype.component_ops[component_index].quantized_size;
    if (offset + size > frame.bytes.size()) {
        return false;
    }
    out.assign(frame.bytes.data() + offset, size);
    return true;
}

const std::uint8_t* frame_component_data(
    const SyncArchetype& archetype,
    const QuantizedFrameData& frame,
    std::size_t component_index) {
    if (!frame_has_component(frame, component_index) ||
        component_index >= archetype.component_offsets.size() ||
        component_index >= archetype.component_ops.size()) {
        return nullptr;
    }
    const std::size_t offset = archetype.component_offsets[component_index];
    const std::size_t size = archetype.component_ops[component_index].quantized_size;
    if (offset + size > frame.bytes.size()) {
        return nullptr;
    }
    return frame.bytes.data() + offset;
}

std::uint8_t* mutable_frame_component_data(
    const SyncArchetype& archetype,
    QuantizedFrameData& frame,
    std::size_t component_index) {
    if (component_index >= 64U ||
        component_index >= archetype.component_offsets.size() ||
        component_index >= archetype.component_ops.size()) {
        return nullptr;
    }
    const std::size_t offset = archetype.component_offsets[component_index];
    const std::size_t size = archetype.component_ops[component_index].quantized_size;
    if (offset + size > frame.bytes.size()) {
        return nullptr;
    }
    frame.present_mask |= (std::uint64_t{1} << component_index);
    return frame.bytes.data() + offset;
}

bool set_frame_component_bytes(
    const SyncArchetype& archetype,
    QuantizedFrameData& frame,
    std::size_t component_index,
    const SyncComponentOps::QuantizedBytes& bytes) {
    if (component_index >= 64U ||
        component_index >= archetype.component_offsets.size() ||
        component_index >= archetype.component_ops.size() ||
        bytes.size() != archetype.component_ops[component_index].quantized_size) {
        return false;
    }
    const std::size_t offset = archetype.component_offsets[component_index];
    if (offset + bytes.size() > frame.bytes.size()) {
        return false;
    }
    std::memcpy(frame.bytes.data() + offset, bytes.data(), bytes.size());
    frame.present_mask |= (std::uint64_t{1} << component_index);
    return true;
}

bool tag_bit_set(std::uint64_t tag_mask, std::size_t tag_index) noexcept {
    return tag_index < 64U && (tag_mask & (std::uint64_t{1} << tag_index)) != 0U;
}

bool apply_archetype_tags(
    ecs::Registry& registry,
    ecs::Entity entity,
    const SyncArchetype& archetype,
    std::uint64_t tag_mask) {
    for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
        const ecs::Entity tag = archetype.tags[tag_index].tag;
        if (tag_bit_set(tag_mask, tag_index)) {
            if (!registry.add_tag(entity, tag)) {
                return false;
            }
        } else {
            registry.remove_tag(entity, tag);
        }
    }
    return true;
}

void remove_archetype_tags(ecs::Registry& registry, ecs::Entity entity, const SyncArchetype& archetype) {
    for (const SyncTagReplication& replication : archetype.tags) {
        registry.remove_tag(entity, replication.tag);
    }
}

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

    found_ops->second.dequantize(found->bytes, out);
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

bool DisplayEntitySample::try_get(
    const ecs::Registry& registry,
    ecs::Entity component,
    void* out) const {
    if (out == nullptr) {
        return false;
    }

    const auto found = std::find_if(
        components.begin(),
        components.end(),
        [component](const ReplicatedComponentUpdate& update) {
            return update.component == component;
        });
    if (found == components.end()) {
        if (!local_entity || !registry.alive(local_entity)) {
            return false;
        }
        const void* value = registry.get(local_entity, component);
        const ecs::ComponentInfo* info = registry.component_info(component);
        if (value == nullptr || info == nullptr || info->tag) {
            return false;
        }
        std::memcpy(out, value, info->size);
        return true;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    const auto found_ops = settings.component_ops.find(component.value);
    if (found_ops == settings.component_ops.end() || found_ops->second.dequantize == nullptr) {
        return false;
    }

    found_ops->second.dequantize(found->bytes, out);
    return true;
}

bool DisplayEntitySample::has_tag(const ecs::Registry& registry, ecs::Entity tag) const {
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

ReplicationClient::ReplicationClient(ReplicationClientOptions options)
    : options_(options) {
    if (!is_power_of_two(options_.interpolation_buffer_capacity_frames)) {
        throw std::invalid_argument("interpolation buffer capacity must be a nonzero power of two");
    }
    if (options_.interpolation_buffer_frames >= options_.interpolation_buffer_capacity_frames) {
        throw std::invalid_argument("interpolation buffer amount must be smaller than capacity");
    }
    if (options_.auto_interpolation_min_frames >= options_.interpolation_buffer_capacity_frames) {
        throw std::invalid_argument("auto interpolation buffer minimum must be smaller than capacity");
    }
    if (options_.auto_interpolation_jitter_multiplier < 0.0f) {
        throw std::invalid_argument("auto interpolation jitter multiplier must be non-negative");
    }
    if (options_.auto_interpolation_smoothing <= 0.0f || options_.auto_interpolation_smoothing > 1.0f) {
        throw std::invalid_argument("auto interpolation smoothing must be in the range (0, 1]");
    }
    if (options_.auto_interpolation_time_dilation_min <= 0.0f ||
        options_.auto_interpolation_time_dilation_min > 1.0f) {
        throw std::invalid_argument("auto interpolation time dilation minimum must be in the range (0, 1]");
    }
    if (options_.auto_interpolation_time_dilation_max < 1.0f) {
        throw std::invalid_argument("auto interpolation time dilation maximum must be at least 1");
    }
    if (options_.auto_interpolation_time_dilation_gain < 0.0f) {
        throw std::invalid_argument("auto interpolation time dilation gain must be non-negative");
    }
    if (options_.fixed_dt_seconds <= 0.0 || !std::isfinite(options_.fixed_dt_seconds)) {
        throw std::invalid_argument("fixed dt seconds must be finite and positive");
    }
    timing_stats_.desired_interpolation_buffer_frames = options_.interpolation_buffer_frames;
    timing_stats_.target_interpolation_buffer_frames = options_.interpolation_buffer_frames;
    timing_stats_.current_interpolation_buffer_frames = options_.interpolation_buffer_frames;
}

ReplicationClient::EntityState* ReplicationClient::find_entity_state(ecs::Entity server_entity) noexcept {
    const std::uint32_t entity_index = ecs::Registry::entity_index(server_entity);
    if (entity_index >= entities_.size()) {
        return nullptr;
    }
    EntityState& state = entities_[entity_index];
    return state.server_entity == server_entity.value ? &state : nullptr;
}

const ReplicationClient::EntityState* ReplicationClient::find_entity_state(ecs::Entity server_entity) const noexcept {
    const std::uint32_t entity_index = ecs::Registry::entity_index(server_entity);
    if (entity_index >= entities_.size()) {
        return nullptr;
    }
    const EntityState& state = entities_[entity_index];
    return state.server_entity == server_entity.value ? &state : nullptr;
}

ReplicationClient::EntityState* ReplicationClient::find_entity_state(std::uint32_t network_id) noexcept {
    if (network_id == 0U || network_id >= network_entity_indices_.size()) {
        return nullptr;
    }
    const std::uint32_t entity_index = network_entity_indices_[network_id];
    if (entity_index >= entities_.size()) {
        return nullptr;
    }
    EntityState& state = entities_[entity_index];
    return state.network_id == network_id ? &state : nullptr;
}

const ReplicationClient::EntityState* ReplicationClient::find_entity_state(std::uint32_t network_id) const noexcept {
    if (network_id == 0U || network_id >= network_entity_indices_.size()) {
        return nullptr;
    }
    const std::uint32_t entity_index = network_entity_indices_[network_id];
    if (entity_index >= entities_.size()) {
        return nullptr;
    }
    const EntityState& state = entities_[entity_index];
    return state.network_id == network_id ? &state : nullptr;
}

ReplicationClient::EntityState* ReplicationClient::ensure_entity_state(
    ecs::Registry& registry,
    ecs::Entity server_entity,
    std::uint32_t network_id) {
    if (!server_entity || network_id == 0U) {
        return nullptr;
    }

    const std::uint32_t entity_index = ecs::Registry::entity_index(server_entity);
    if (entity_index >= entities_.size()) {
        entities_.resize(static_cast<std::size_t>(entity_index) + 1U);
    }

    EntityState& state = entities_[entity_index];
    if (state.server_entity == server_entity.value) {
        return &state;
    }

    if (state.server_entity != 0U) {
        const auto stored_version =
            ecs::Registry::entity_version(ecs::Entity{state.server_entity});
        const auto incoming_version = ecs::Registry::entity_version(server_entity);
        if (incoming_version < stored_version) {
            return nullptr;
        }
        erase_entity_state(registry, entity_index, true);
    }

    EntityState& fresh = entities_[entity_index];
    fresh = EntityState{};
    fresh.server_entity = server_entity.value;
    fresh.network_id = network_id;
    fresh.active_index = active_entities_.size();
    active_entities_.push_back(entity_index);
    if (network_id >= network_entity_indices_.size()) {
        network_entity_indices_.resize(static_cast<std::size_t>(network_id) + 1U, invalid_entity_index);
    }
    network_entity_indices_[network_id] = entity_index;
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
    if (state.server_entity == 0U) {
        return;
    }
    if (state.network_id < network_entity_indices_.size() &&
        network_entity_indices_[state.network_id] == entity_index) {
        network_entity_indices_[state.network_id] = invalid_entity_index;
    }

    if (destroy_local && state.local && registry.alive(state.local)) {
        registry.destroy(state.local);
    }

    set_buffered_membership(entity_index, false);
    set_snap_error_membership(entity_index, false);
    if (state.active_index != invalid_ack_index && state.active_index < active_entities_.size()) {
        const std::uint32_t moved = active_entities_.back();
        active_entities_[state.active_index] = moved;
        entities_[moved].active_index = state.active_index;
        active_entities_.pop_back();
    }

    state = EntityState{};
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

void ReplicationClient::sync_entity_memberships(EntityState& state) {
    if (state.server_entity == 0U) {
        return;
    }
    const std::uint32_t entity_index = ecs::Registry::entity_index(ecs::Entity{state.server_entity});
    set_buffered_membership(entity_index, state.mode == ReplicationClientMode::BufferedInterpolation);
    set_snap_error_membership(
        entity_index,
        state.mode == ReplicationClientMode::Snap && !state.snap_errors.empty());
}

const QuantizedFrameData* ReplicationClient::find_baseline(
    const EntityState& state,
    SyncFrame frame) const noexcept {
    if (state.history.empty()) {
        return nullptr;
    }

    const std::size_t count = state.history.size();
    for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t index = (state.history_next + count - 1U - offset) % count;
        const EntityState::FrameBaseline& baseline = state.history[index];
        if (baseline.frame == frame) {
            return &baseline.baseline;
        }
    }
    return nullptr;
}

bool ReplicationClient::receive(ecs::Registry& registry, BitBuffer packet) {
    try {
        if (packet.remaining_bits() < 8U) {
            return false;
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message != protocol::server_update_message) {
            return false;
        }
        const auto frame = static_cast<SyncFrame>(packet.read_bits(32U));
        const auto packet_id = static_cast<std::uint32_t>(packet.read_bits(configured_packet_id_bits(options_)));
        const auto record_count = static_cast<std::uint16_t>(packet.read_bits(16U));
        const bool applied = apply_update(registry, packet, packet_id, frame, record_count);
        if (applied) {
            record_timing_sample(frame, receive_frame_, playback_frame_);
        }
        return applied;
    } catch (const std::exception&) {
        return false;
    }
}

bool ReplicationClient::receive(ecs::Registry& registry, BitBuffer packet, SyncFrame client_frame) {
    return receive(registry, std::move(packet), client_frame, client_frame);
}

bool ReplicationClient::receive(
    ecs::Registry& registry,
    BitBuffer packet,
    SyncFrame receive_frame,
    SyncFrame playback_frame) {
    try {
        if (packet.remaining_bits() < 8U) {
            return false;
        }
        const auto message = static_cast<std::uint8_t>(packet.read_bits(8U));
        if (message != protocol::server_update_message) {
            return false;
        }
        const auto frame = static_cast<SyncFrame>(packet.read_bits(32U));
        const auto packet_id = static_cast<std::uint32_t>(packet.read_bits(configured_packet_id_bits(options_)));
        const auto record_count = static_cast<std::uint16_t>(packet.read_bits(16U));
        const bool applied = apply_update(registry, packet, packet_id, frame, record_count);
        if (applied) {
            record_timing_sample(frame, receive_frame, playback_frame);
        }
        return applied;
    } catch (const std::exception&) {
        return false;
    }
}

bool ReplicationClient::set_default_entity_mode(ReplicationClientMode mode) noexcept {
    options_.default_entity_mode = mode;
    return true;
}

bool ReplicationClient::set_entity_mode(
    ecs::Registry& registry,
    ecs::Entity server_entity,
    ReplicationClientMode mode) {
    EntityState* state = find_entity_state(server_entity);
    if (state == nullptr) {
        return false;
    }

    if (!state->entity_present && !state->local) {
        return false;
    }
    if (state->mode == mode) {
        return true;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    const bool switched = switch_entity_mode(registry, settings, *state, mode);
    if (switched) {
        sync_entity_memberships(*state);
    }
    return switched;
}

ReplicationClientMode ReplicationClient::entity_mode(ecs::Entity server_entity) const noexcept {
    const EntityState* state = find_entity_state(server_entity);
    return state != nullptr ? state->mode : options_.default_entity_mode;
}

bool ReplicationClient::set_interpolation_buffer_frames(SyncFrame frames) noexcept {
    if (frames >= options_.interpolation_buffer_capacity_frames) {
        return false;
    }
    options_.interpolation_buffer_frames = frames;
    timing_stats_.desired_interpolation_buffer_frames = frames;
    timing_stats_.target_interpolation_buffer_frames = frames;
    timing_stats_.current_interpolation_buffer_frames = frames;
    timing_stats_.time_dilation = 1.0f;
    return true;
}

bool ReplicationClient::tick(ecs::Registry& registry, double dt_seconds) {
    if (dt_seconds < 0.0 || !std::isfinite(dt_seconds)) {
        return false;
    }

    receive_accumulator_seconds_ += dt_seconds;
    while (receive_accumulator_seconds_ >= options_.fixed_dt_seconds) {
        receive_accumulator_seconds_ -= options_.fixed_dt_seconds;
        ++receive_frame_;
    }

    playback_accumulator_seconds_ += dt_seconds * static_cast<double>(timing_stats_.time_dilation);
    display_accumulator_seconds_ += dt_seconds;
    while (playback_accumulator_seconds_ >= options_.fixed_dt_seconds) {
        playback_accumulator_seconds_ -= options_.fixed_dt_seconds;
        ++playback_frame_;
        (void)apply_frame(registry, playback_frame_);
    }

    update_display_target(dt_seconds);
    return true;
}

bool ReplicationClient::apply_frame(ecs::Registry& registry, SyncFrame client_frame) {
    if (!has_buffered_entities()) {
        return true;
    }
    if (client_frame < options_.interpolation_buffer_frames) {
        return false;
    }

    const SyncFrame target_frame = client_frame - options_.interpolation_buffer_frames;
    last_applied_buffered_frame_ = target_frame;
    has_applied_buffered_frame_ = true;
    const SyncSettings& settings = registry.get<SyncSettings>();
    bool all_valid = true;
    for (const std::uint32_t entity_index : buffered_entities_) {
        if (entity_index >= entities_.size()) {
            continue;
        }
        EntityState& state = entities_[entity_index];
        if (state.buffered_frames.empty()) {
            continue;
        }

        const std::size_t mask = state.buffered_frames.size() - 1U;
        const EntityState::BufferedFrame& sample = state.buffered_frames[target_frame & mask];
        if (!sample.valid || sample.frame != target_frame) {
            all_valid = false;
            continue;
        }
        if (!apply_buffered_sample(registry, settings, state, sample)) {
            all_valid = false;
        }
    }

    return all_valid;
}

bool ReplicationClient::sample_display_target_frame(
    const ecs::Registry& registry,
    double target_frame,
    DisplaySampleBuffer& out) const {
    return write_display_samples(registry, target_frame, false, false, out);
}

bool ReplicationClient::sample_display_frame(
    const ecs::Registry& registry,
    double client_frame,
    DisplaySampleBuffer& out) const {
    return sample_display_target_frame(
        registry,
        client_frame - static_cast<double>(options_.interpolation_buffer_frames),
        out);
}

const DisplaySampleBuffer& ReplicationClient::display_frame(const ecs::Registry& registry) {
    const float display_dt = display_accumulator_seconds_ > 0.0 && std::isfinite(display_accumulator_seconds_)
        ? static_cast<float>(display_accumulator_seconds_)
        : 0.0f;
    display_accumulator_seconds_ = 0.0;
    if (display_dt > 0.0f) {
        blend_snap_errors(registry.get<SyncSettings>(), display_dt);
    }
    if (write_display_samples(registry, display_target_frame_, true, true, display_scratch_)) {
        display_frame_.entities.swap(display_scratch_.entities);
        display_scratch_.clear();
    } else if (display_frame_.entities.empty()) {
        display_frame_.entities.swap(display_scratch_.entities);
        display_scratch_.clear();
    }
    return display_frame_;
}

std::vector<BitBuffer> ReplicationClient::drain_ack_packets() {
    std::vector<BitBuffer> packets;
    if (pending_acks_.empty()) {
        pending_acks_.clear();
        return packets;
    }

    const std::size_t packet_id_bits = configured_packet_id_bits(options_);
    const std::size_t one_ack_bytes = protocol::bytes_for_bits(protocol::client_ack_header_bits + packet_id_bits);
    if (one_ack_bytes > options_.mtu_bytes) {
        return packets;
    }

    const std::size_t mtu_bits = options_.mtu_bytes * 8U;
    const std::size_t max_acks_per_packet =
        std::min<std::size_t>(
            std::numeric_limits<std::uint16_t>::max(),
            (mtu_bits - protocol::client_ack_header_bits) / packet_id_bits);
    if (max_acks_per_packet == 0U) {
        return packets;
    }

    packets.reserve((pending_acks_.size() + max_acks_per_packet - 1U) / max_acks_per_packet);
    BitBuffer packet;
    std::uint16_t packet_ack_count = 0;
    std::size_t packet_count_offset = 0;
    auto begin_packet = [&]() {
        packet.clear();
        packet.reserve_bytes(options_.mtu_bytes);
        packet.push_bits(protocol::client_ack_message, 8U);
        packet_count_offset = packet.bit_size();
        packet.push_bits(0, 16U);
        packet_ack_count = 0;
    };
    auto finish_packet = [&]() {
        if (packet_ack_count == 0U) {
            return;
        }
        packet.overwrite_unsigned_bits(packet_count_offset, packet_ack_count, 16U);
        packets.push_back(std::move(packet));
        packet = BitBuffer{};
    };

    begin_packet();
    for (const std::uint32_t packet_id : pending_acks_) {
        if (packet_ack_count == max_acks_per_packet) {
            finish_packet();
            begin_packet();
        }
        packet.push_bits(packet_id, packet_id_bits);
        ++packet_ack_count;
    }
    finish_packet();

    pending_acks_.clear();
    return packets;
}

std::size_t ReplicationClient::pending_ack_count() const noexcept {
    return pending_acks_.size();
}

ecs::Entity ReplicationClient::local_entity(ecs::Entity server_entity) const {
    const EntityState* state = find_entity_state(server_entity);
    return state != nullptr ? state->local : ecs::Entity{};
}

bool ReplicationClient::apply_update(
    ecs::Registry& registry,
    BitBuffer& packet,
    std::uint32_t packet_id,
    SyncFrame frame,
    std::uint16_t record_count) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    bool applied_any = false;

    for (std::uint16_t record = 0; record < record_count; ++record) {
        const bool destroy = packet.read_bool();
        std::uint32_t network_id = 0;
        if (!protocol::read_network_entity_id(packet, network_id)) {
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
    BitBuffer& packet) {
    const bool full = packet.read_bool();
    ecs::Entity server_entity;
    SyncArchetypeId archetype = invalid_sync_archetype_id;
    SyncFrame baseline_frame = 0;

    EntityState* found_state = nullptr;
    if (full) {
        server_entity = ecs::Entity{packet.read_unsigned_bits(64U)};
        if (!server_entity) {
            return false;
        }
        found_state = find_entity_state(server_entity);
    } else {
        found_state = find_entity_state(network_id);
        if (found_state != nullptr) {
            server_entity = ecs::Entity{found_state->server_entity};
        }
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
            if (ops.deserialize_bytes != nullptr) {
                if (!ops.deserialize_bytes(packet, nullptr, received_bytes)) {
                    return false;
                }
                if (collect_decoded_updates) {
                    baseline.bytes.assign(received_bytes, ops.quantized_size);
                }
            } else {
                if (!ops.deserialize(packet, nullptr, baseline.bytes)) {
                    return false;
                }
                if (!set_frame_component_bytes(definition, received, component_index, baseline.bytes)) {
                    return false;
                }
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
            if (ops.deserialize_bytes != nullptr) {
                if (!ops.deserialize_bytes(packet, previous_bytes, merged_bytes)) {
                    return false;
                }
                if (!buffered_without_selector) {
                    std::memcpy(decoded_bytes, merged_bytes, ops.quantized_size);
                }
                if (collect_decoded_updates) {
                    baseline.bytes.assign(merged_bytes, ops.quantized_size);
                }
            } else {
                SyncComponentOps::QuantizedBytes previous_quantized;
                previous_quantized.assign(previous_bytes, ops.quantized_size);
                if (!ops.deserialize(packet, &previous_quantized, baseline.bytes)) {
                    return false;
                }
                if (!set_frame_component_bytes(definition, merged, component_index, baseline.bytes)) {
                    return false;
                }
                if (!buffered_without_selector &&
                    !set_frame_component_bytes(definition, decoded, component_index, baseline.bytes)) {
                    return false;
                }
            }
            if (collect_decoded_updates) {
                decoded_updates.push_back(std::move(baseline));
            }
        }
        if (!buffered_without_selector) {
            decoded.tag_mask = merged.tag_mask;
        }
    }

    if (previous_state != nullptr && frame <= previous_state->frame) {
        return false;
    }

    EntityState* ensured_state = ensure_entity_state(registry, server_entity, network_id);
    if (ensured_state == nullptr) {
        return false;
    }
    EntityState& state = *ensured_state;
    if (!state.mode_selected) {
        ReplicationClientMode selected = options_.default_entity_mode;
        if (options_.entity_mode_selector) {
            ReplicatedEntityUpdateView update;
            update.server_entity = server_entity;
            update.local_entity = state.local;
            update.archetype = archetype;
            update.frame = frame;
            update.tag_mask = merged.tag_mask;
            update.components = &decoded_updates;
            selected = options_.entity_mode_selector(update);
        }
        state.mode = selected;
        state.mode_selected = true;
    }

    if (state.mode == ReplicationClientMode::BufferedInterpolation) {
        return apply_buffered_upsert(registry, settings, frame, server_entity, archetype, merged);
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
    if (!full) {
        state.baseline = std::move(merged);
        state.applied_present_mask = state.baseline.present_mask;
    }

    state.frame = frame;
    state.entity_present = true;
    remember_baseline(state);
    return true;
}

bool ReplicationClient::apply_destroy(ecs::Registry& registry, SyncFrame frame, std::uint32_t network_id) {
    EntityState* state = find_entity_state(network_id);
    if (state == nullptr) {
        return true;
    }
    const ecs::Entity server_entity{state->server_entity};
    if (state->mode == ReplicationClientMode::BufferedInterpolation) {
        return apply_buffered_destroy(registry, frame, server_entity);
    }
    if (frame <= state->frame) {
        return false;
    }
    erase_entity_state(registry, ecs::Registry::entity_index(server_entity), true);
    return true;
}

bool ReplicationClient::apply_buffered_upsert(
    ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    ecs::Entity server_entity,
    SyncArchetypeId archetype,
    QuantizedFrameData& decoded) {
    if (!validate_buffered_archetype(settings, archetype)) {
        return false;
    }

    EntityState* ensured_state = find_entity_state(server_entity);
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

bool ReplicationClient::apply_buffered_destroy(ecs::Registry& registry, SyncFrame frame, ecs::Entity server_entity) {
    EntityState* state_ptr = find_entity_state(server_entity);
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
        const EntityState::BufferedFrame& sample = state.buffered_frames[frame & mask];
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
    EntityState::BufferedFrame& sample = state.buffered_frames[frame & mask];
    sample.frame = frame;
    sample.valid = true;
    sample.entity_present = entity_present;
    sample.archetype = state.archetype;
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
                (ops.interpolate == nullptr && ops.interpolate_bytes == nullptr) ||
                to_frame == from_frame) {
                return false;
            }
            const std::uint8_t* next = frame_component_data(archetype, *to, component_index);
            if (next == nullptr) {
                return false;
            }
            const float alpha =
                static_cast<float>(frame - from_frame) / static_cast<float>(to_frame - from_frame);
            if (ops.interpolate_bytes != nullptr) {
                if (!ops.interpolate_bytes(previous, next, alpha, value)) {
                    return false;
                }
            } else {
                SyncComponentOps::QuantizedBytes previous_quantized;
                SyncComponentOps::QuantizedBytes next_quantized;
                SyncComponentOps::QuantizedBytes interpolated;
                previous_quantized.assign(previous, ops.quantized_size);
                next_quantized.assign(next, ops.quantized_size);
                if (!ops.interpolate(previous_quantized, next_quantized, alpha, interpolated) ||
                    !set_frame_component_bytes(archetype, sample.baseline, component_index, interpolated)) {
                    return false;
                }
            }
        }
    }

    return true;
}

bool ReplicationClient::apply_buffered_sample(
    ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const EntityState::BufferedFrame& sample) {
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
    if (!apply_archetype_tags(registry, state.local, archetype, sample.baseline.tag_mask)) {
        return false;
    }
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        const std::uint64_t bit = std::uint64_t{1} << component_index;
        if ((state.applied_present_mask & bit) != 0U &&
            (sample.baseline.present_mask & bit) == 0U) {
            registry.remove(state.local, archetype.components[component_index].component);
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
        if (ops.apply_bytes != nullptr) {
            if (!ops.apply_bytes(registry, state.local, bytes)) {
                return false;
            }
        } else {
            SyncComponentOps::QuantizedBytes quantized;
            quantized.assign(bytes, ops.quantized_size);
            if (!ops.apply(registry, state.local, quantized)) {
                return false;
            }
        }
    }

    state.applied_present_mask = sample.baseline.present_mask;
    state.archetype = sample.archetype;
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
    if (!apply_archetype_tags(registry, state.local, archetype, decoded.tag_mask)) {
        return false;
    }
    if (full) {
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            const std::uint64_t bit = std::uint64_t{1} << component_index;
            if ((state.baseline.present_mask & bit) != 0U &&
                (decoded.present_mask & bit) == 0U) {
                registry.remove(state.local, archetype.components[component_index].component);
            }
        }

        state.snap_errors.erase(
            std::remove_if(
                state.snap_errors.begin(),
                state.snap_errors.end(),
                [&](const EntityState::ComponentError& existing) {
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
        if (ops.apply_bytes != nullptr) {
            if (!ops.apply_bytes(registry, state.local, bytes)) {
                return false;
            }
        } else {
            SyncComponentOps::QuantizedBytes quantized;
            quantized.assign(bytes, ops.quantized_size);
            if (!ops.apply(registry, state.local, quantized)) {
                return false;
            }
        }

        SyncComponentOps::QuantizedBytes previous_bytes;
        const bool had_baseline = frame_component_bytes(archetype, state.baseline, component_index, previous_bytes);
        if (had_baseline &&
            ops.compute_error != nullptr &&
            ops.apply_error != nullptr &&
            ops.blend_out_error != nullptr) {
            SyncComponentOps::QuantizedBytes current_bytes;
            current_bytes.assign(bytes, ops.quantized_size);
            SyncComponentOps::QuantizedBytes error;
            if (!ops.compute_error(current_bytes, previous_bytes, error)) {
                return false;
            }

            auto found_error = std::find_if(
                state.snap_errors.begin(),
                state.snap_errors.end(),
                [&](const EntityState::ComponentError& existing) {
                    return existing.component == replication.component;
                });
            if (all_zero(error)) {
                if (found_error != state.snap_errors.end()) {
                    state.snap_errors.erase(found_error);
                }
            } else if (found_error == state.snap_errors.end()) {
                state.snap_errors.push_back(EntityState::ComponentError{replication.component, std::move(error)});
            } else {
                found_error->bytes = std::move(error);
            }
        }
        SyncComponentOps::QuantizedBytes current_bytes;
        current_bytes.assign(bytes, ops.quantized_size);
        if (!set_frame_component_bytes(archetype, state.baseline, component_index, current_bytes)) {
            return false;
        }
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

void ReplicationClient::blend_snap_errors(const SyncSettings& settings, float dt_seconds) {
    for (std::size_t list_index = 0; list_index < snap_error_entities_.size();) {
        const std::uint32_t entity_index = snap_error_entities_[list_index];
        if (entity_index >= entities_.size()) {
            ++list_index;
            continue;
        }
        EntityState& state = entities_[entity_index];

        for (EntityState::ComponentError& error : state.snap_errors) {
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
                [](const EntityState::ComponentError& error) {
                    return error.bytes.empty() || all_zero(error.bytes);
                }),
            state.snap_errors.end());
        if (state.mode != ReplicationClientMode::Snap || state.snap_errors.empty()) {
            set_snap_error_membership(entity_index, false);
        } else {
            ++list_index;
        }
    }
}

bool ReplicationClient::write_display_samples(
    const ecs::Registry& registry,
    double target_frame,
    bool include_snap,
    bool include_empty_buffered,
    DisplaySampleBuffer& out) const {
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
    auto previous_display = [&](std::uint64_t server_entity) -> const DisplayEntitySample* {
        const auto found = std::find_if(
            display_frame_.entities.begin(),
            display_frame_.entities.end(),
            [server_entity](const DisplayEntitySample& sample) {
                return sample.server_entity.value == server_entity;
            });
        return found != display_frame_.entities.end() ? &*found : nullptr;
    };
    auto append_previous_display = [&](std::uint64_t server_entity) {
        const DisplayEntitySample* previous = previous_display(server_entity);
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
        if (state.server_entity == 0U) {
            continue;
        }
        if (state.mode != ReplicationClientMode::BufferedInterpolation) {
            if (!include_snap || !state.local || !registry.alive(state.local)) {
                continue;
            }

            if (sampled_count == out.entities.size()) {
                out.entities.emplace_back();
            }
            DisplayEntitySample& display = out.entities[sampled_count++];
            display.server_entity = ecs::Entity{state.server_entity};
            display.local_entity = state.local;
            display.archetype = state.archetype;
            display.frame = state.frame;
            display.alpha = 0.0f;
            display.tag_mask = state.baseline.tag_mask;
            display.components.clear();
            display.components.reserve(state.snap_errors.size());
            for (const EntityState::ComponentError& error : state.snap_errors) {
                const auto found_ops = settings.component_ops.find(error.component.value);
                if (found_ops == settings.component_ops.end() ||
                    found_ops->second.quantize == nullptr ||
                    found_ops->second.apply_error == nullptr) {
                    return false;
                }
                const void* current = registry.get(state.local, error.component);
                if (current == nullptr) {
                    return false;
                }

                ReplicatedComponentUpdate value;
                value.component = error.component;
                SyncComponentOps::QuantizedBytes current_bytes;
                found_ops->second.quantize(current, current_bytes);
                if (!found_ops->second.apply_error(current_bytes, error.bytes, value.bytes)) {
                    return false;
                }
                display.components.push_back(std::move(value));
            }
            continue;
        }

        if (state.buffered_frames.empty() || !target_valid) {
            if (!include_empty_buffered || !append_previous_display(state.server_entity)) {
                all_valid = false;
            }
            continue;
        }

        const std::size_t mask = state.buffered_frames.size() - 1U;
        const EntityState::BufferedFrame& floor_sample = state.buffered_frames[floor_frame & mask];
        if (!floor_sample.valid || floor_sample.frame != floor_frame) {
            if (!include_empty_buffered || !append_previous_display(state.server_entity)) {
                all_valid = false;
            }
            continue;
        }
        if (!floor_sample.entity_present) {
            continue;
        }

        const EntityState::BufferedFrame* next_sample = nullptr;
        if (alpha > 0.0f && floor_frame != std::numeric_limits<SyncFrame>::max()) {
            const SyncFrame next_frame = floor_frame + 1U;
            const EntityState::BufferedFrame& candidate = state.buffered_frames[next_frame & mask];
            if (candidate.valid && candidate.frame == next_frame && candidate.entity_present) {
                next_sample = &candidate;
            }
        }

        if (sampled_count == out.entities.size()) {
            out.entities.emplace_back();
        }
        DisplayEntitySample& display = out.entities[sampled_count];
        display.server_entity = ecs::Entity{state.server_entity};
        display.local_entity = state.local;
        display.archetype = floor_sample.archetype;
        display.frame = floor_frame;
        display.alpha = alpha;
        display.tag_mask = floor_sample.baseline.tag_mask;
        display.components.clear();
        const SyncArchetype& display_archetype = settings.archetypes[floor_sample.archetype.value];
        display.components.reserve(display_archetype.components.size());

        for (std::size_t component_index = 0; component_index < display_archetype.components.size(); ++component_index) {
            const ecs::Entity component = display_archetype.components[component_index].component;
            if (!frame_has_component(floor_sample.baseline, component_index) ||
                !registry.has<DisplayInterpolated>(component)) {
                continue;
            }

            ReplicatedComponentUpdate value;
            value.component = component;
            SyncComponentOps::QuantizedBytes baseline_bytes;
            if (!frame_component_bytes(display_archetype, floor_sample.baseline, component_index, baseline_bytes)) {
                return false;
            }

            if (next_sample != nullptr && frame_has_component(next_sample->baseline, component_index)) {
                if (component_index >= display_archetype.component_ops.size() ||
                    display_archetype.component_ops[component_index].interpolate == nullptr) {
                    return false;
                }
                SyncComponentOps::QuantizedBytes next_bytes;
                if (!frame_component_bytes(display_archetype, next_sample->baseline, component_index, next_bytes) ||
                    !display_archetype.component_ops[component_index].interpolate(
                        baseline_bytes,
                        next_bytes,
                        alpha,
                        value.bytes)) {
                    return false;
                }
            } else {
                value.bytes = std::move(baseline_bytes);
            }
            display.components.push_back(std::move(value));
        }

        if (include_empty_buffered || !display.components.empty()) {
            ++sampled_count;
        }
    }

    out.entities.resize(sampled_count);
    return include_empty_buffered || all_valid;
}

void ReplicationClient::update_display_target(double dt_seconds) noexcept {
    const double playback_frame =
        static_cast<double>(playback_frame_) + playback_accumulator_seconds_ / options_.fixed_dt_seconds;
    const double desired = playback_frame - static_cast<double>(options_.interpolation_buffer_frames);
    if (!has_display_target_frame_ || desired < 0.0) {
        display_target_frame_ = desired;
        has_display_target_frame_ = true;
        return;
    }
    if (desired <= display_target_frame_) {
        return;
    }

    const double max_advance_frames =
        dt_seconds / options_.fixed_dt_seconds * static_cast<double>(options_.auto_interpolation_time_dilation_max);
    display_target_frame_ = std::min(desired, display_target_frame_ + max_advance_frames);
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
    for (EntityState::FrameBaseline& baseline : state.history) {
        if (baseline.frame == state.frame) {
            baseline.baseline = state.baseline;
            return;
        }
    }

    if (state.history.size() < max_baseline_history_per_entity) {
        state.history.push_back(EntityState::FrameBaseline{state.frame, state.baseline});
        state.history_next = state.history.size() % max_baseline_history_per_entity;
        return;
    }

    state.history[state.history_next] = EntityState::FrameBaseline{state.frame, state.baseline};
    state.history_next = (state.history_next + 1U) % max_baseline_history_per_entity;
}

void ReplicationClient::queue_ack(std::uint32_t packet_id) {
    pending_acks_.push_back(packet_id);
}

void ReplicationClient::record_timing_sample(
    SyncFrame server_frame,
    SyncFrame receive_frame,
    SyncFrame playback_frame) noexcept {
    const float sample = receive_frame >= server_frame
        ? static_cast<float>(receive_frame - server_frame)
        : 0.0f;

    if (timing_stats_.sample_count == 0) {
        timing_stats_.latency_frames = sample;
        timing_stats_.jitter_frames = 0.0f;
    } else {
        const float smoothing = options_.auto_interpolation_smoothing;
        const float previous_latency = timing_stats_.latency_frames;
        timing_stats_.latency_frames += (sample - timing_stats_.latency_frames) * smoothing;
        const float deviation = std::fabs(sample - previous_latency);
        timing_stats_.jitter_frames += (deviation - timing_stats_.jitter_frames) * smoothing;
    }
    ++timing_stats_.sample_count;

    const float wanted = timing_stats_.latency_frames +
        options_.auto_interpolation_jitter_multiplier * timing_stats_.jitter_frames;
    SyncFrame target = static_cast<SyncFrame>(std::ceil(std::max(0.0f, wanted)));
    target = std::max(target, options_.auto_interpolation_min_frames);
    const SyncFrame max_frames = static_cast<SyncFrame>(options_.interpolation_buffer_capacity_frames - 1U);
    target = std::min(target, max_frames);
    timing_stats_.desired_interpolation_buffer_frames = target;
    timing_stats_.target_interpolation_buffer_frames = target;

    const SyncFrame current = options_.interpolation_buffer_frames;
    const SyncFrame applied_frame = playback_frame >= current ? playback_frame - current : 0U;
    const float measured = server_frame >= applied_frame
        ? static_cast<float>(server_frame - applied_frame)
        : 0.0f;
    timing_stats_.measured_interpolation_buffer_frames = measured;

    const bool internal_clock_started = receive_frame != 0 || playback_frame != 0;
    if (internal_clock_started &&
        has_buffered_entities() &&
        options_.auto_interpolation_buffer_frames) {
        const float error = measured - static_cast<float>(target);
        const float unclamped = 1.0f + error * options_.auto_interpolation_time_dilation_gain;
        timing_stats_.time_dilation = std::min(
            options_.auto_interpolation_time_dilation_max,
            std::max(options_.auto_interpolation_time_dilation_min, unclamped));

        if (current < target && measured >= static_cast<float>(current)) {
            options_.interpolation_buffer_frames = current + 1U;
        } else if (current > target && measured <= static_cast<float>(current)) {
            options_.interpolation_buffer_frames = current - 1U;
        }
        timing_stats_.current_interpolation_buffer_frames = options_.interpolation_buffer_frames;
    } else {
        timing_stats_.current_interpolation_buffer_frames = options_.interpolation_buffer_frames;
        timing_stats_.time_dilation = 1.0f;
    }
}

}  // namespace kage::sync
