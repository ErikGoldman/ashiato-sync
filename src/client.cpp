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

bool is_power_of_two(std::size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

bool all_zero(const SyncComponentOps::QuantizedBytes& bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) {
        return byte == 0U;
    });
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
        const auto record_count = static_cast<std::uint16_t>(packet.read_bits(16U));
        const bool applied = apply_update(registry, packet, frame, record_count);
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
        const auto record_count = static_cast<std::uint16_t>(packet.read_bits(16U));
        const bool applied = apply_update(registry, packet, frame, record_count);
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
    const auto found = entities_.find(server_entity.value);
    if (found == entities_.end()) {
        return false;
    }

    EntityState& state = found->second;
    if (!state.entity_present && !state.local) {
        return false;
    }
    if (state.mode == mode) {
        return true;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    return switch_entity_mode(registry, settings, state, mode);
}

ReplicationClientMode ReplicationClient::entity_mode(ecs::Entity server_entity) const noexcept {
    const auto found = entities_.find(server_entity.value);
    return found != entities_.end() ? found->second.mode : options_.default_entity_mode;
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
    for (auto& entry : entities_) {
        EntityState& state = entry.second;
        if (state.mode != ReplicationClientMode::BufferedInterpolation || state.buffered_frames.empty()) {
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
        return packets;
    }

    BitBuffer packet;
    packet.reserve_bytes(options_.mtu_bytes);
    std::uint16_t packet_acks = 0;
    std::vector<AckRecord> retained_acks;

    auto flush = [&]() {
        if (packet_acks == 0) {
            return;
        }
        packets.push_back(packet);
        packet.clear();
        packet.reserve_bytes(options_.mtu_bytes);
        packet_acks = 0;
    };

    for (const AckRecord& ack : pending_acks_) {
        if (protocol::bytes_for_bits(protocol::client_ack_header_bits + protocol::client_ack_record_bits) >
            options_.mtu_bytes) {
            retained_acks.push_back(ack);
            continue;
        }

        if (packet_acks == 0) {
            packet.push_bits(protocol::client_ack_message, 8U);
            packet.push_bits(0, 16U);
        }

        const std::size_t next_bits = packet.bit_size() + protocol::client_ack_record_bits;
        if (packet_acks != 0 && protocol::bytes_for_bits(next_bits) > options_.mtu_bytes) {
            flush();
            packet.push_bits(protocol::client_ack_message, 8U);
            packet.push_bits(0, 16U);
        }

        if (protocol::bytes_for_bits(packet.bit_size() + protocol::client_ack_record_bits) > options_.mtu_bytes) {
            retained_acks.push_back(ack);
            continue;
        }

        packet.push_bool(ack.destroy);
        packet.push_bits(ack.frame, 32U);
        packet.push_unsigned_bits(ack.entity.value, 64U);
        ++packet_acks;
    }
    flush();

    for (BitBuffer& ack_packet : packets) {
        BitBuffer rewritten;
        rewritten.reserve_bytes(ack_packet.byte_size());
        rewritten.push_bits(protocol::client_ack_message, 8U);

        BitBuffer read = ack_packet;
        read.read_bits(8U);
        read.read_bits(16U);
        const std::uint16_t count =
            static_cast<std::uint16_t>(read.remaining_bits() / protocol::client_ack_record_bits);
        rewritten.push_bits(count, 16U);
        while (read.remaining_bits() >= protocol::client_ack_record_bits) {
            rewritten.push_bool(read.read_bool());
            rewritten.push_bits(read.read_bits(32U), 32U);
            rewritten.push_unsigned_bits(read.read_unsigned_bits(64U), 64U);
        }
        ack_packet = std::move(rewritten);
    }

    pending_acks_ = std::move(retained_acks);
    return packets;
}

std::size_t ReplicationClient::pending_ack_count() const noexcept {
    return pending_acks_.size();
}

ecs::Entity ReplicationClient::local_entity(ecs::Entity server_entity) const {
    const auto found = entities_.find(server_entity.value);
    return found != entities_.end() ? found->second.local : ecs::Entity{};
}

bool ReplicationClient::apply_update(
    ecs::Registry& registry,
    BitBuffer& packet,
    SyncFrame frame,
    std::uint16_t record_count) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    bool applied_any = false;

    for (std::uint16_t record = 0; record < record_count; ++record) {
        const bool destroy = packet.read_bool();
        const ecs::Entity server_entity{packet.read_unsigned_bits(64U)};
        if (!server_entity) {
            return false;
        }

        const bool applied = destroy
            ? apply_destroy(registry, frame, server_entity)
            : apply_upsert(registry, settings, frame, server_entity, packet);
        if (!applied) {
            return false;
        }
        applied_any = true;
    }

    return applied_any;
}

bool ReplicationClient::apply_upsert(
    ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    ecs::Entity server_entity,
    BitBuffer& packet) {
    const bool full = packet.read_bool();
    SyncArchetypeId archetype = invalid_sync_archetype_id;
    SyncFrame baseline_frame = 0;

    auto found_state = entities_.find(server_entity.value);
    const bool previous_absent = found_state != entities_.end() &&
        !found_state->second.entity_present &&
        !found_state->second.local;
    if (full) {
        archetype = SyncArchetypeId{static_cast<std::uint32_t>(packet.read_bits(32U))};
        if (archetype.value >= settings.archetypes.size()) {
            return false;
        }
        if (previous_absent) {
            EntityState& state = found_state->second;
            state.archetype = archetype;
            state.mode = ReplicationClientMode::Snap;
            state.entity_present = false;
            state.mode_selected = false;
            state.baselines.clear();
            state.history.clear();
            state.applied_baselines.clear();
        }
    } else {
        if (found_state == entities_.end() || previous_absent) {
            return false;
        }
        archetype = found_state->second.archetype;
        if (archetype.value >= settings.archetypes.size()) {
            return false;
        }
        if (frame <= found_state->second.frame) {
            return false;
        }
        if (!protocol::read_baseline_frame(packet, frame, baseline_frame)) {
            return false;
        }
    }

    const SyncArchetype& definition = settings.archetypes[archetype.value];
    const auto component_count = static_cast<std::uint16_t>(packet.read_bits(16U));
    std::vector<ComponentBaseline> decoded;
    decoded.reserve(component_count);

    EntityState* previous_state = found_state != entities_.end() && !previous_absent ? &found_state->second : nullptr;
    const std::vector<ComponentBaseline>* previous_baselines = nullptr;
    if (!full && previous_state != nullptr) {
        const auto found_baseline = std::find_if(
            previous_state->history.begin(),
            previous_state->history.end(),
            [baseline_frame](const EntityState::FrameBaseline& candidate) {
                return candidate.frame == baseline_frame;
            });
        if (found_baseline == previous_state->history.end()) {
            return false;
        }
        previous_baselines = &found_baseline->baselines;
    }

    for (std::uint16_t component = 0; component < component_count; ++component) {
        const auto component_index = static_cast<std::uint16_t>(packet.read_bits(16U));
        const auto payload_bits = static_cast<std::uint32_t>(packet.read_bits(32U));
        if (component_index >= definition.components.size()) {
            return false;
        }

        BitBuffer payload;
        payload.reserve_bytes(protocol::bytes_for_bits(payload_bits));
        for (std::uint32_t bit = 0; bit < payload_bits; ++bit) {
            payload.push_bool(packet.read_bool());
        }

        const ecs::Entity component_entity = definition.components[component_index].component;
        const auto found_ops = settings.component_ops.find(component_entity.value);
        if (found_ops == settings.component_ops.end() || found_ops->second.deserialize == nullptr ||
            found_ops->second.apply == nullptr) {
            return false;
        }

        ComponentBaseline baseline;
        baseline.component = component_entity;
        const SyncComponentOps::QuantizedBytes* previous =
            previous_baselines != nullptr ? baseline_for(*previous_baselines, component_entity) : nullptr;
        if (!found_ops->second.deserialize(payload, previous, baseline.bytes)) {
            return false;
        }
        decoded.push_back(std::move(baseline));
    }

    if (previous_state != nullptr && frame <= previous_state->frame) {
        return false;
    }

    EntityState& state = entities_[server_entity.value];
    if (!state.mode_selected) {
        ReplicationClientMode selected = options_.default_entity_mode;
        if (options_.entity_mode_selector) {
            ReplicatedEntityUpdateView update;
            update.server_entity = server_entity;
            update.local_entity = state.local;
            update.archetype = archetype;
            update.frame = frame;
            update.components = &decoded;
            selected = options_.entity_mode_selector(update);
        }
        state.mode = selected;
        state.mode_selected = true;
    }

    if (state.mode == ReplicationClientMode::BufferedInterpolation) {
        return apply_buffered_upsert(registry, settings, frame, server_entity, archetype, decoded);
    }

    state.archetype = archetype;
    if (!apply_snap_sample(registry, settings, state, decoded, full)) {
        return false;
    }

    state.frame = frame;
    state.entity_present = true;
    remember_baseline(state);
    queue_ack(server_entity, frame, false);
    return true;
}

bool ReplicationClient::apply_destroy(ecs::Registry& registry, SyncFrame frame, ecs::Entity server_entity) {
    const auto found = entities_.find(server_entity.value);
    if (found == entities_.end()) {
        queue_ack(server_entity, frame, true);
        return true;
    }
    if (found->second.mode == ReplicationClientMode::BufferedInterpolation) {
        return apply_buffered_destroy(registry, frame, server_entity);
    }
    if (frame <= found->second.frame) {
        return false;
    }
    if (found->second.local) {
        registry.destroy(found->second.local);
    }
    entities_.erase(found);
    queue_ack(server_entity, frame, true);
    return true;
}

bool ReplicationClient::apply_buffered_upsert(
    ecs::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    ecs::Entity server_entity,
    SyncArchetypeId archetype,
    std::vector<ComponentBaseline>& decoded) {
    if (!validate_buffered_archetype(settings, archetype)) {
        return false;
    }

    EntityState& state = entities_[server_entity.value];
    state.mode = ReplicationClientMode::BufferedInterpolation;
    state.mode_selected = true;
    state.archetype = archetype;
    state.snap_errors.clear();
    if (state.buffered_frames.empty()) {
        state.buffered_frames.resize(options_.interpolation_buffer_capacity_frames);
        for (EntityState::BufferedFrame& sample : state.buffered_frames) {
            sample.baselines.reserve(settings.archetypes[archetype.value].components.size());
        }
    }

    if (!fill_buffered_frames(settings, state, frame, true, decoded)) {
        return false;
    }

    state.baselines = decoded;
    state.frame = frame;
    state.entity_present = true;
    remember_baseline(state);
    queue_ack(server_entity, frame, false);
    (void)registry;
    return true;
}

bool ReplicationClient::apply_buffered_destroy(ecs::Registry& registry, SyncFrame frame, ecs::Entity server_entity) {
    EntityState& state = entities_[server_entity.value];
    state.mode = ReplicationClientMode::BufferedInterpolation;
    state.mode_selected = true;
    state.snap_errors.clear();
    if (state.buffered_frames.empty()) {
        state.buffered_frames.resize(options_.interpolation_buffer_capacity_frames);
    }
    std::vector<ComponentBaseline> empty;
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!fill_buffered_frames(settings, state, frame, false, empty)) {
        return false;
    }
    state.baselines.clear();
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
    queue_ack(server_entity, frame, true);
    return true;
}

bool ReplicationClient::validate_buffered_archetype(const SyncSettings& settings, SyncArchetypeId archetype) const {
    if (archetype.value >= settings.archetypes.size()) {
        return false;
    }
    const SyncArchetype& definition = settings.archetypes[archetype.value];
    for (const ComponentReplication& replication : definition.components) {
        if (replication.interpolation != ComponentInterpolation::Interpolate) {
            continue;
        }
        const auto found_ops = settings.component_ops.find(replication.component.value);
        if (found_ops == settings.component_ops.end() || found_ops->second.interpolate == nullptr) {
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
    std::vector<ComponentBaseline>& decoded) {
    if (state.frame != 0 && frame <= state.frame) {
        return false;
    }

    const std::vector<ComponentBaseline>* from = state.frame != 0 ? &state.baselines : nullptr;
    const bool from_entity_present = state.local || (from != nullptr && !from->empty());
    const SyncFrame from_frame = state.frame;
    const SyncFrame begin = state.frame != 0 ? state.frame + 1U : frame;
    for (SyncFrame current = begin; current <= frame; ++current) {
        const bool current_present = current == frame ? entity_present : from_entity_present;
        const std::vector<ComponentBaseline>* to = entity_present ? &decoded : nullptr;
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
    const std::vector<ComponentBaseline>* from,
    const std::vector<ComponentBaseline>* to,
    SyncFrame from_frame,
    SyncFrame to_frame) {
    const std::size_t mask = state.buffered_frames.size() - 1U;
    EntityState::BufferedFrame& sample = state.buffered_frames[frame & mask];
    sample.frame = frame;
    sample.valid = true;
    sample.entity_present = entity_present;
    sample.archetype = state.archetype;
    sample.baselines.clear();

    if (!entity_present) {
        return true;
    }

    const bool final_frame = frame == to_frame;
    if (final_frame && to != nullptr) {
        sample.baselines = *to;
        return true;
    }
    if (from == nullptr) {
        return true;
    }

    sample.baselines.reserve(from->size());
    for (const ComponentBaseline& previous : *from) {
        const ComponentBaseline* next = nullptr;
        if (to != nullptr) {
            const auto found_next = std::find_if(
                to->begin(),
                to->end(),
                [&](const ComponentBaseline& candidate) {
                    return candidate.component == previous.component;
                });
            if (found_next != to->end()) {
                next = &*found_next;
            }
        }

        ComponentBaseline value;
        value.component = previous.component;
        if (next != nullptr &&
            interpolation_for(settings, state.archetype, previous.component) == ComponentInterpolation::Interpolate) {
            const auto found_ops = settings.component_ops.find(previous.component.value);
            if (found_ops == settings.component_ops.end() || found_ops->second.interpolate == nullptr ||
                to_frame == from_frame) {
                return false;
            }
            const float alpha =
                static_cast<float>(frame - from_frame) / static_cast<float>(to_frame - from_frame);
            if (!found_ops->second.interpolate(previous.bytes, next->bytes, alpha, value.bytes)) {
                return false;
            }
        } else {
            value.bytes = previous.bytes;
        }
        sample.baselines.push_back(std::move(value));
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
        state.applied_baselines.clear();
        state.snap_errors.clear();
        return true;
    }

    if (!state.local || !registry.alive(state.local)) {
        state.local = registry.create();
    }

    for (const ComponentBaseline& existing : state.applied_baselines) {
        const auto still_present = std::find_if(
            sample.baselines.begin(),
            sample.baselines.end(),
            [&](const ComponentBaseline& baseline) {
                return baseline.component == existing.component;
            });
        if (still_present == sample.baselines.end()) {
            registry.remove(state.local, existing.component);
        }
    }

    for (const ComponentBaseline& baseline : sample.baselines) {
        const auto found_ops = settings.component_ops.find(baseline.component.value);
        if (found_ops == settings.component_ops.end() ||
            !found_ops->second.apply(registry, state.local, baseline.bytes)) {
            return false;
        }
    }

    state.applied_baselines = sample.baselines;
    state.archetype = sample.archetype;
    return true;
}

bool ReplicationClient::apply_snap_sample(
    ecs::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const std::vector<ComponentBaseline>& decoded,
    bool full) {
    if (!state.local || !registry.alive(state.local)) {
        state.local = registry.create();
    }

    if (full) {
        for (const ComponentBaseline& existing : state.baselines) {
            const auto still_visible = std::find_if(
                decoded.begin(),
                decoded.end(),
                [&](const ComponentBaseline& baseline) {
                    return baseline.component == existing.component;
                });
            if (still_visible == decoded.end()) {
                registry.remove(state.local, existing.component);
            }
        }

        state.baselines.erase(
            std::remove_if(
                state.baselines.begin(),
                state.baselines.end(),
                [&](const ComponentBaseline& existing) {
                    return std::find_if(
                        decoded.begin(),
                        decoded.end(),
                        [&](const ComponentBaseline& baseline) {
                            return baseline.component == existing.component;
                        }) == decoded.end();
                }),
            state.baselines.end());

        state.snap_errors.erase(
            std::remove_if(
                state.snap_errors.begin(),
                state.snap_errors.end(),
                [&](const EntityState::ComponentError& existing) {
                    return std::find_if(
                        decoded.begin(),
                        decoded.end(),
                        [&](const ComponentBaseline& baseline) {
                            return baseline.component == existing.component;
                        }) == decoded.end();
                }),
            state.snap_errors.end());
    }

    for (const ComponentBaseline& baseline : decoded) {
        const auto found_ops = settings.component_ops.find(baseline.component.value);
        if (found_ops == settings.component_ops.end() ||
            !found_ops->second.apply(registry, state.local, baseline.bytes)) {
            return false;
        }

        auto found_baseline = std::find_if(
            state.baselines.begin(),
            state.baselines.end(),
            [&](const ComponentBaseline& existing) {
                return existing.component == baseline.component;
            });
        if (found_baseline != state.baselines.end() &&
            found_ops->second.compute_error != nullptr &&
            found_ops->second.apply_error != nullptr &&
            found_ops->second.blend_out_error != nullptr) {
            SyncComponentOps::QuantizedBytes error;
            if (!found_ops->second.compute_error(baseline.bytes, found_baseline->bytes, error)) {
                return false;
            }

            auto found_error = std::find_if(
                state.snap_errors.begin(),
                state.snap_errors.end(),
                [&](const EntityState::ComponentError& existing) {
                    return existing.component == baseline.component;
                });
            if (all_zero(error)) {
                if (found_error != state.snap_errors.end()) {
                    state.snap_errors.erase(found_error);
                }
            } else if (found_error == state.snap_errors.end()) {
                state.snap_errors.push_back(EntityState::ComponentError{baseline.component, std::move(error)});
            } else {
                found_error->bytes = std::move(error);
            }
        }
        if (found_baseline == state.baselines.end()) {
            state.baselines.push_back(baseline);
        } else {
            found_baseline->bytes = baseline.bytes;
        }
    }

    state.entity_present = true;
    state.applied_baselines = state.baselines;
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
        state.applied_baselines.clear();
        state.snap_errors.clear();
        return true;
    }

    return apply_snap_sample(registry, settings, state, state.baselines, true);
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
            for (EntityState::BufferedFrame& sample : state.buffered_frames) {
                sample.baselines.reserve(settings.archetypes[state.archetype.value].components.size());
            }
        }
        if (state.frame != 0) {
            if (!write_buffered_frame(
                    settings,
                    state,
                    state.frame,
                    state.entity_present,
                    nullptr,
                    state.entity_present ? &state.baselines : nullptr,
                    state.frame,
                    state.frame)) {
                return false;
            }
        }
        state.applied_baselines = state.baselines;
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
    return true;
}

bool ReplicationClient::has_buffered_entities() const noexcept {
    return std::any_of(
        entities_.begin(),
        entities_.end(),
        [](const auto& entry) {
            return entry.second.mode == ReplicationClientMode::BufferedInterpolation;
        });
}

void ReplicationClient::blend_snap_errors(const SyncSettings& settings, float dt_seconds) {
    for (auto& entry : entities_) {
        EntityState& state = entry.second;
        if (state.mode != ReplicationClientMode::Snap || state.snap_errors.empty()) {
            continue;
        }

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

    for (const auto& entry : entities_) {
        const EntityState& state = entry.second;
        if (state.mode != ReplicationClientMode::BufferedInterpolation) {
            if (!include_snap || !state.local || !registry.alive(state.local)) {
                continue;
            }

            if (sampled_count == out.entities.size()) {
                out.entities.emplace_back();
            }
            DisplayEntitySample& display = out.entities[sampled_count++];
            display.server_entity = ecs::Entity{entry.first};
            display.local_entity = state.local;
            display.archetype = state.archetype;
            display.frame = state.frame;
            display.alpha = 0.0f;
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
            if (!include_empty_buffered || !append_previous_display(entry.first)) {
                all_valid = false;
            }
            continue;
        }

        const std::size_t mask = state.buffered_frames.size() - 1U;
        const EntityState::BufferedFrame& floor_sample = state.buffered_frames[floor_frame & mask];
        if (!floor_sample.valid || floor_sample.frame != floor_frame) {
            if (!include_empty_buffered || !append_previous_display(entry.first)) {
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
        display.server_entity = ecs::Entity{entry.first};
        display.local_entity = state.local;
        display.archetype = floor_sample.archetype;
        display.frame = floor_frame;
        display.alpha = alpha;
        display.components.clear();
        display.components.reserve(floor_sample.baselines.size());

        for (const ComponentBaseline& baseline : floor_sample.baselines) {
            if (!registry.has<DisplayInterpolated>(baseline.component)) {
                continue;
            }

            ReplicatedComponentUpdate value;
            value.component = baseline.component;
            const ComponentBaseline* next = nullptr;
            if (next_sample != nullptr) {
                const auto found_next = std::find_if(
                    next_sample->baselines.begin(),
                    next_sample->baselines.end(),
                    [&](const ComponentBaseline& candidate) {
                        return candidate.component == baseline.component;
                    });
                if (found_next != next_sample->baselines.end()) {
                    next = &*found_next;
                }
            }

            if (next != nullptr) {
                const auto found_ops = settings.component_ops.find(baseline.component.value);
                if (found_ops == settings.component_ops.end() || found_ops->second.interpolate == nullptr ||
                    !found_ops->second.interpolate(baseline.bytes, next->bytes, alpha, value.bytes)) {
                    return false;
                }
            } else {
                value.bytes = baseline.bytes;
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

const SyncComponentOps::QuantizedBytes* ReplicationClient::baseline_for(
    const std::vector<ComponentBaseline>& baselines,
    ecs::Entity component) const {
    const auto found = std::find_if(
        baselines.begin(),
        baselines.end(),
        [component](const ComponentBaseline& baseline) {
            return baseline.component == component;
        });
    return found != baselines.end() ? &found->bytes : nullptr;
}

void ReplicationClient::remember_baseline(EntityState& state) {
    state.history.erase(
        std::remove_if(
            state.history.begin(),
            state.history.end(),
            [frame = state.frame](const EntityState::FrameBaseline& baseline) {
                return baseline.frame == frame;
            }),
        state.history.end());
    state.history.push_back(EntityState::FrameBaseline{state.frame, state.baselines});
    while (state.history.size() > max_baseline_history_per_entity) {
        state.history.erase(state.history.begin());
    }
}

void ReplicationClient::queue_ack(ecs::Entity entity, SyncFrame frame, bool destroy) {
    pending_acks_.erase(
        std::remove_if(
            pending_acks_.begin(),
            pending_acks_.end(),
            [&](const AckRecord& ack) {
                return ack.entity == entity && ack.destroy == destroy;
            }),
        pending_acks_.end());
    pending_acks_.push_back(AckRecord{entity, frame, destroy});
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
