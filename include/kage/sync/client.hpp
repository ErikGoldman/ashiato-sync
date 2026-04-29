#pragma once

#include "kage/sync/component_traits.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace kage::sync {

struct ReplicatedComponentUpdate {
    ecs::Entity component;
    SyncComponentOps::QuantizedBytes bytes;
};

struct ReplicatedEntityUpdateView {
    ecs::Entity server_entity;
    ecs::Entity local_entity;
    SyncArchetypeId archetype;
    SyncFrame frame = 0;
    std::uint64_t tag_mask = 0;

    template <typename T>
    bool try_get(const ecs::Registry& registry, T& out) const {
        const ecs::Entity component = registry.component<T>();
        return try_get(registry, component, &out);
    }

    bool try_get(const ecs::Registry& registry, ecs::Entity component, void* out) const;
    bool has_tag(const ecs::Registry& registry, ecs::Entity tag) const;

private:
    friend class ReplicationClient;

    const std::vector<ReplicatedComponentUpdate>* components = nullptr;
};

struct DisplayEntitySample {
    ecs::Entity server_entity;
    ecs::Entity local_entity;
    SyncArchetypeId archetype;
    SyncFrame frame = 0;
    float alpha = 0.0f;
    std::uint64_t tag_mask = 0;
    std::vector<ReplicatedComponentUpdate> components;

    template <typename T>
    bool try_get(const ecs::Registry& registry, T& out) const {
        const ecs::Entity component = registry.component<T>();
        return try_get(registry, component, &out);
    }

    bool try_get(const ecs::Registry& registry, ecs::Entity component, void* out) const;
    bool has_tag(const ecs::Registry& registry, ecs::Entity tag) const;
};

struct DisplaySampleBuffer {
    std::vector<DisplayEntitySample> entities;

    void clear() {
        entities.clear();
    }
};

using EntityModeSelector = std::function<ReplicationClientMode(const ReplicatedEntityUpdateView&)>;

enum class ReplicationClientConnectionState {
    Disconnected,
    Connecting,
    Accepted,
    Ready,
    Rejected
};

struct ReplicationClientOptions {
    std::size_t mtu_bytes = 1200;
    ReplicationClientMode default_entity_mode = ReplicationClientMode::Snap;
    SyncFrame interpolation_buffer_frames = 2;
    std::size_t interpolation_buffer_capacity_frames = 64;
    bool auto_interpolation_buffer_frames = true;
    SyncFrame auto_interpolation_min_frames = 1;
    float auto_interpolation_jitter_multiplier = 2.0f;
    float auto_interpolation_smoothing = 0.1f;
    float auto_interpolation_time_dilation_min = 0.95f;
    float auto_interpolation_time_dilation_max = 1.05f;
    float auto_interpolation_time_dilation_gain = 0.05f;
    EntityModeSelector entity_mode_selector;
    double fixed_dt_seconds = 1.0 / 60.0;
    std::size_t max_pending_packet_acks_per_client = protocol::default_max_pending_packet_acks_per_client;
    std::string connect_token;
    double connect_resend_interval_seconds = 0.25;
    double ping_interval_seconds = 3.0;
};

struct ReplicationClientTimingStats {
    std::uint64_t sample_count = 0;
    float latency_frames = 0.0f;
    float jitter_frames = 0.0f;
    float measured_interpolation_buffer_frames = 0.0f;
    SyncFrame desired_interpolation_buffer_frames = 0;
    SyncFrame target_interpolation_buffer_frames = 0;
    SyncFrame current_interpolation_buffer_frames = 0;
    float time_dilation = 1.0f;
};

class ReplicationClient {
public:
    explicit ReplicationClient(ReplicationClientOptions options = {});

    const ReplicationClientOptions& options() const noexcept {
        return options_;
    }

    bool set_default_entity_mode(ReplicationClientMode mode) noexcept;
    bool set_entity_mode(ecs::Registry& registry, ecs::Entity server_entity, ReplicationClientMode mode);
    ReplicationClientMode entity_mode(ecs::Entity server_entity) const noexcept;
    bool set_interpolation_buffer_frames(SyncFrame frames) noexcept;
    bool tick(ecs::Registry& registry, double dt_seconds);
    bool receive(ecs::Registry& registry, BitBuffer packet);
    bool receive(ecs::Registry& registry, BitBuffer packet, SyncFrame client_frame);
    bool receive(
        ecs::Registry& registry,
        BitBuffer packet,
        SyncFrame receive_frame,
        SyncFrame playback_frame);
    bool apply_frame(ecs::Registry& registry, SyncFrame client_frame);
    bool sample_display_target_frame(
        const ecs::Registry& registry,
        double target_frame,
        DisplaySampleBuffer& out) const;
    bool sample_display_frame(
        const ecs::Registry& registry,
        double client_frame,
        DisplaySampleBuffer& out) const;
    const DisplaySampleBuffer& display_frame(const ecs::Registry& registry);
    std::vector<BitBuffer> drain_packets();
    std::vector<BitBuffer> drain_ack_packets();
    std::size_t pending_ack_count() const noexcept;
    ClientId client_id() const noexcept {
        return client_id_;
    }
    ReplicationClientConnectionState connection_state() const noexcept {
        return connection_state_;
    }
    const std::string& connect_error() const noexcept {
        return connect_error_;
    }
    ecs::Entity local_entity(ecs::Entity server_entity) const;
    SyncFrame receive_frame() const noexcept {
        return receive_frame_;
    }
    SyncFrame playback_frame() const noexcept {
        return playback_frame_;
    }
    const ReplicationClientTimingStats& timing_stats() const noexcept {
        return timing_stats_;
    }

private:
    using ComponentBaseline = ReplicatedComponentUpdate;
    static constexpr std::size_t invalid_ack_index = std::numeric_limits<std::size_t>::max();
    static constexpr std::uint32_t invalid_entity_index = std::numeric_limits<std::uint32_t>::max();

    struct EntityState {
        struct FrameBaseline {
            SyncFrame frame = 0;
            QuantizedFrameData baseline;
        };

        struct ComponentError {
            ecs::Entity component;
            SyncComponentOps::QuantizedBytes bytes;
        };

        ecs::Entity local;
        SyncArchetypeId archetype;
        ReplicationClientMode mode = ReplicationClientMode::Snap;
        SyncFrame frame = 0;
        bool entity_present = true;
        bool mode_selected = false;
        QuantizedFrameData baseline;
        std::vector<FrameBaseline> history;
        std::size_t history_next = 0;

        struct BufferedFrame {
            SyncFrame frame = 0;
            bool valid = false;
            bool entity_present = false;
            SyncArchetypeId archetype;
            QuantizedFrameData baseline;
        };

        std::vector<BufferedFrame> buffered_frames;
        std::uint64_t applied_present_mask = 0;
        std::vector<ComponentError> snap_errors;
        std::uint64_t server_entity = 0;
        std::uint32_t network_id = 0;
        std::size_t active_index = invalid_ack_index;
        std::size_t buffered_index = invalid_ack_index;
        std::size_t snap_error_index = invalid_ack_index;
    };

    EntityState* find_entity_state(ecs::Entity server_entity) noexcept;
    const EntityState* find_entity_state(ecs::Entity server_entity) const noexcept;
    EntityState* find_entity_state(std::uint32_t network_id) noexcept;
    const EntityState* find_entity_state(std::uint32_t network_id) const noexcept;
    EntityState* ensure_entity_state(ecs::Registry& registry, ecs::Entity server_entity, std::uint32_t network_id);
    void erase_entity_state(ecs::Registry& registry, std::uint32_t entity_index, bool destroy_local);
    void set_buffered_membership(std::uint32_t entity_index, bool active);
    void set_snap_error_membership(std::uint32_t entity_index, bool active);
    void sync_entity_memberships(EntityState& state);
    bool destroy_tombstone_blocks(ecs::Entity server_entity, SyncFrame frame) const;
    void record_destroy_tombstone(ecs::Entity server_entity, SyncFrame frame);
    const QuantizedFrameData* find_baseline(const EntityState& state, SyncFrame frame) const noexcept;
    bool apply_update(
        ecs::Registry& registry,
        BitBuffer& packet,
        std::uint32_t packet_id,
        SyncFrame frame,
        std::uint16_t record_count);
    bool apply_upsert(
        ecs::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        std::uint32_t network_id,
        BitBuffer& packet);
    bool apply_destroy(ecs::Registry& registry, SyncFrame frame, std::uint32_t network_id);
    bool apply_buffered_upsert(
        ecs::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        ecs::Entity server_entity,
        SyncArchetypeId archetype,
        QuantizedFrameData& decoded);
    bool apply_buffered_destroy(ecs::Registry& registry, SyncFrame frame, ecs::Entity server_entity);
    bool validate_buffered_archetype(const SyncSettings& settings, SyncArchetypeId archetype) const;
    bool fill_buffered_frames(
        const SyncSettings& settings,
        EntityState& state,
        SyncFrame frame,
        bool entity_present,
        QuantizedFrameData& decoded);
    bool write_buffered_frame(
        const SyncSettings& settings,
        EntityState& state,
        SyncFrame frame,
        bool entity_present,
        const QuantizedFrameData* from,
        const QuantizedFrameData* to,
        SyncFrame from_frame,
        SyncFrame to_frame);
    bool apply_buffered_sample(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const EntityState::BufferedFrame& sample);
    bool apply_snap_sample(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const QuantizedFrameData& decoded,
        bool full);
    bool apply_latest_snap(ecs::Registry& registry, const SyncSettings& settings, EntityState& state);
    bool switch_entity_mode(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        ReplicationClientMode mode);
    bool has_buffered_entities() const noexcept;
    void blend_snap_errors(const SyncSettings& settings, float dt_seconds);
    bool write_display_samples(
        const ecs::Registry& registry,
        double target_frame,
        bool include_snap,
        bool include_empty_buffered,
        DisplaySampleBuffer& out) const;
    void update_display_target(double dt_seconds) noexcept;
    ComponentInterpolation interpolation_for(
        const SyncSettings& settings,
        SyncArchetypeId archetype,
        ecs::Entity component) const;
    void remember_baseline(EntityState& state);
    void queue_ack(std::uint32_t packet_id);
    void record_update_timing(SyncFrame server_frame, SyncFrame receive_frame, SyncFrame playback_frame) noexcept;
    void record_ping_sample(float latency_frames) noexcept;
    void advance_control_time_to(SyncFrame receive_frame) noexcept;
    bool receive_connect_response(ecs::Registry& registry, BitBuffer& packet);
    bool receive_pong(BitBuffer& packet, SyncFrame receive_frame);
    void drain_connect_packets(std::vector<BitBuffer>& packets);
    void drain_ping_packets(std::vector<BitBuffer>& packets);
    void drain_ack_packets_into(std::vector<BitBuffer>& packets);

    ReplicationClientOptions options_;
    ReplicationClientTimingStats timing_stats_;
    std::vector<EntityState> entities_;
    std::vector<std::uint32_t> active_entities_;
    std::vector<std::uint32_t> buffered_entities_;
    std::vector<std::uint32_t> snap_error_entities_;
    std::vector<std::uint32_t> network_entity_indices_;
    std::vector<std::uint32_t> pending_acks_;
    std::unordered_map<std::uint32_t, SyncFrame> pending_pings_;
    std::unordered_map<std::uint64_t, SyncFrame> destroy_tombstones_;
    DisplaySampleBuffer display_frame_;
    DisplaySampleBuffer display_scratch_;
    std::string connect_error_;
    ClientId client_id_ = invalid_client_id;
    ReplicationClientConnectionState connection_state_ = ReplicationClientConnectionState::Connecting;
    double connect_resend_accumulator_seconds_ = 0.0;
    double ping_accumulator_seconds_ = 0.0;
    std::uint32_t next_ping_sequence_ = 1;
    bool sent_initial_connect_request_ = false;
    bool sent_initial_ping_ = false;
    double receive_accumulator_seconds_ = 0.0;
    double playback_accumulator_seconds_ = 0.0;
    double display_accumulator_seconds_ = 0.0;
    double display_target_frame_ = 0.0;
    SyncFrame receive_frame_ = 0;
    SyncFrame playback_frame_ = 0;
    SyncFrame last_applied_buffered_frame_ = 0;
    bool has_applied_buffered_frame_ = false;
    bool has_display_target_frame_ = false;
};

}  // namespace kage::sync
