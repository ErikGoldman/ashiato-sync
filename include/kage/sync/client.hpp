#pragma once

#include "kage/sync/component_traits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kage::sync {

struct ReplicatedComponentUpdate {
    ecs::Entity component;
    SyncComponentOps::QuantizedBytes bytes;
};

struct ReplicatedEntityUpdateView {
    ClientEntityNetworkId client_entity_network_id = invalid_client_entity_network_id;
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
    ClientEntityNetworkId client_entity_network_id = invalid_client_entity_network_id;
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
    std::size_t network_entity_id_tier0_bits = protocol::default_network_entity_id_tier0_bits;
    ReplicationRollbackPolicy rollback_policy = ReplicationRollbackPolicy::All;
    std::size_t prediction_buffer_capacity_frames = 64;
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
    std::uint64_t server_update_packets_received = 0;
    std::uint64_t server_update_packets_missing = 0;
    std::uint64_t server_update_packets_reordered_or_duplicate = 0;
    float server_update_packet_loss = 0.0f;
};

#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
inline constexpr std::size_t interpolation_diagnostics_window_frames = 120;

struct ReplicationClientInterpolationDiagnostics {
    struct FrameBucket {
        std::uint64_t checks = 0;
        std::uint64_t starvations = 0;
    };

    std::uint64_t total_interpolated_entity_frame_checks = 0;
    std::uint64_t total_interpolated_entity_frame_starvations = 0;
    std::uint64_t window_interpolated_entity_frame_checks = 0;
    std::uint64_t window_interpolated_entity_frame_starvations = 0;
    std::uint64_t sampled_interpolation_frames = 0;

    float interpolated_entity_starvation_rate() const noexcept {
        return window_interpolated_entity_frame_checks == 0
            ? 0.0f
            : static_cast<float>(window_interpolated_entity_frame_starvations) /
                static_cast<float>(window_interpolated_entity_frame_checks);
    }

    float interpolated_entity_starvation_percent() const noexcept {
        return interpolated_entity_starvation_rate() * 100.0f;
    }

    float lifetime_interpolated_entity_starvation_rate() const noexcept {
        return total_interpolated_entity_frame_checks == 0
            ? 0.0f
            : static_cast<float>(total_interpolated_entity_frame_starvations) /
                static_cast<float>(total_interpolated_entity_frame_checks);
    }

    float lifetime_interpolated_entity_starvation_percent() const noexcept {
        return lifetime_interpolated_entity_starvation_rate() * 100.0f;
    }

    void record_frame(std::uint64_t checks, std::uint64_t starvations) noexcept {
        if (checks == 0) {
            return;
        }

        total_interpolated_entity_frame_checks += checks;
        total_interpolated_entity_frame_starvations += starvations;
        ++sampled_interpolation_frames;

        FrameBucket& bucket = window_[window_next_];
        window_interpolated_entity_frame_checks -= bucket.checks;
        window_interpolated_entity_frame_starvations -= bucket.starvations;
        bucket.checks = checks;
        bucket.starvations = starvations;
        window_interpolated_entity_frame_checks += checks;
        window_interpolated_entity_frame_starvations += starvations;
        window_next_ = (window_next_ + 1U) % window_.size();
    }

private:
    std::array<FrameBucket, interpolation_diagnostics_window_frames> window_{};
    std::size_t window_next_ = 0;
};
#endif

namespace detail {

template <typename JobBuilder, bool AddNoResimTag>
class ClientJobBuilder {
public:
    ClientJobBuilder(ecs::Registry& registry, JobBuilder builder, std::vector<ecs::Entity>& jobs)
        : registry_(&registry), builder_(std::move(builder)), jobs_(&jobs) {}

    ClientJobBuilder& max_threads(std::size_t count) {
        builder_.max_threads(count);
        return *this;
    }

    ClientJobBuilder& single_thread() {
        builder_.single_thread();
        return *this;
    }

    ClientJobBuilder& min_entities_per_thread(std::size_t count) {
        builder_.min_entities_per_thread(count);
        return *this;
    }

    template <typename Fn>
    ecs::Entity each(Fn&& fn) {
        const ecs::Entity job = builder_.each(std::forward<Fn>(fn));
        if constexpr (AddNoResimTag) {
            registry_->template add<NoResim>(job);
        }
        jobs_->push_back(job);
        return job;
    }

    template <typename... AccessComponents>
    auto access() const {
        return ClientJobBuilder<decltype(builder_.template access<AccessComponents...>()), AddNoResimTag>(
            *registry_,
            builder_.template access<AccessComponents...>(),
            *jobs_);
    }

    template <typename... StructuralComponents>
    auto structural() const {
        return ClientJobBuilder<decltype(builder_.template structural<StructuralComponents...>()), AddNoResimTag>(
            *registry_,
            builder_.template structural<StructuralComponents...>(),
            *jobs_);
    }

    template <typename... Tags>
    auto with_tags() const {
        return ClientJobBuilder<decltype(builder_.template with_tags<Tags...>()), AddNoResimTag>(
            *registry_,
            builder_.template with_tags<Tags...>(),
            *jobs_);
    }

    template <typename... Tags>
    auto without_tags() const {
        return ClientJobBuilder<decltype(builder_.template without_tags<Tags...>()), AddNoResimTag>(
            *registry_,
            builder_.template without_tags<Tags...>(),
            *jobs_);
    }

    template <typename T>
    decltype(auto) get(ecs::Entity entity) const {
        return builder_.template get<T>(entity);
    }

    template <typename T>
    decltype(auto) get() const {
        return builder_.template get<T>();
    }

    template <typename T>
    decltype(auto) write(ecs::Entity entity) {
        return builder_.template write<T>(entity);
    }

    template <typename T>
    decltype(auto) write() {
        return builder_.template write<T>();
    }

private:
    ecs::Registry* registry_;
    JobBuilder builder_;
    std::vector<ecs::Entity>* jobs_;
};

}  // namespace detail

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
    bool tick(ecs::Registry& registry, double dt_seconds, ecs::RunJobsOptions prediction_options);
    bool predict_tick(ecs::Registry& registry, SyncFrame frame, ecs::RunJobsOptions options = {});
    template <typename... Components>
    auto cosmetic_job(ecs::Registry& registry, int order) {
        register_components(registry);
        auto builder = registry.template job<Components...>(order);
        return detail::ClientJobBuilder<decltype(builder), true>(registry, std::move(builder), cosmetic_jobs_);
    }
    template <typename... Components>
    auto simulation_job(ecs::Registry& registry, int order) {
        register_components(registry);
        auto builder = registry.template job<Components...>(order).template without_tags<const NoSimulate>();
        return detail::ClientJobBuilder<decltype(builder), false>(registry, std::move(builder), simulation_jobs_);
    }
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
    bool is_alive_network_id(ClientEntityNetworkId network_id) const noexcept;
    ecs::Entity local_entity(ClientEntityNetworkId network_id) const;
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
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    const ReplicationClientInterpolationDiagnostics& interpolation_diagnostics() const noexcept {
        return interpolation_diagnostics_;
    }
    void reset_interpolation_diagnostics() noexcept {
        interpolation_diagnostics_ = {};
    }
#endif

private:
    using ComponentBaseline = ReplicatedComponentUpdate;
    static constexpr std::size_t invalid_ack_index = std::numeric_limits<std::size_t>::max();
    static constexpr std::uint32_t invalid_entity_index = std::numeric_limits<std::uint32_t>::max();

    struct EntityState {
        struct Cue {
            SyncFrame frame = 0;
            SyncCueTypeId type = 0;
            float relevance_seconds = 0.0f;
            BitBuffer payload;
        };

        struct PlayedCue {
            SyncFrame frame = 0;
            SyncCueTypeId type = 0;
            BitBuffer payload;
            bool confirmed = false;
            bool seen_in_resim = false;
        };

        struct FrameBaseline {
            SyncFrame frame = 0;
            bool valid = false;
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
            std::vector<Cue> cues;
        };

        std::vector<BufferedFrame> buffered_frames;
        std::vector<BufferedFrame> predicted_frames;
        std::vector<PlayedCue> played_cues;
        std::vector<Cue> received_cues;
        std::vector<Cue> pending_predicted_cues;
        std::uint64_t applied_present_mask = 0;
        std::vector<ComponentError> snap_errors;
        ClientEntityNetworkId client_entity_network_id = invalid_client_entity_network_id;
        std::uint32_t wire_network_id = 0;
        std::size_t active_index = invalid_ack_index;
        std::size_t buffered_index = invalid_ack_index;
        std::size_t snap_error_index = invalid_ack_index;
        std::size_t prediction_rollback_index = invalid_ack_index;
        bool prediction_rollback_pending = false;
        SyncFrame prediction_rollback_frame = 0;
    };

    struct OriginalPredictionCapture {
        std::uint32_t entity_index = invalid_entity_index;
        QuantizedFrameData baseline;
    };

    EntityState* find_entity_state(ecs::Entity server_entity) noexcept;
    const EntityState* find_entity_state(ecs::Entity server_entity) const noexcept;
    EntityState* find_entity_state(ClientEntityNetworkId network_id) noexcept;
    const EntityState* find_entity_state(ClientEntityNetworkId network_id) const noexcept;
    EntityState* find_entity_state(std::uint32_t network_id) noexcept;
    const EntityState* find_entity_state(std::uint32_t network_id) const noexcept;
    EntityState* ensure_entity_state(
        ecs::Registry& registry,
        ClientEntityNetworkId network_id,
        std::uint32_t wire_network_id);
    void erase_entity_state(ecs::Registry& registry, std::uint32_t entity_index, bool destroy_local);
    void set_buffered_membership(std::uint32_t entity_index, bool active);
    void set_snap_error_membership(std::uint32_t entity_index, bool active);
    void set_prediction_rollback_membership(std::uint32_t entity_index, bool active);
    void sync_entity_memberships(EntityState& state);
    bool destroy_tombstone_blocks(std::uint32_t wire_network_id, SyncFrame frame) const;
    void record_destroy_tombstone(std::uint32_t wire_network_id, SyncFrame frame);
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
    bool read_cues(BitBuffer& packet, std::vector<EntityState::Cue>& out);
    bool play_cue(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const EntityState::Cue& cue,
        float late_seconds,
        bool confirmed);
    bool rollback_played_cue(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const EntityState::PlayedCue& cue);
    void play_snap_cues(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const std::vector<EntityState::Cue>& cues);
    void store_buffered_cues(EntityState& state, SyncFrame frame, const std::vector<EntityState::Cue>& cues);
    void reconcile_authoritative_predicted_cues(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const std::vector<EntityState::Cue>& cues,
        SyncFrame frame);
    void drain_emitted_prediction_cues(
        ecs::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        bool play);
    void begin_cue_resimulation();
    bool finish_cue_resimulation(ecs::Registry& registry, const SyncSettings& settings);
    bool apply_destroy(ecs::Registry& registry, SyncFrame frame, std::uint32_t network_id);
    bool apply_buffered_upsert(
        ecs::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        ClientEntityNetworkId network_id,
        SyncArchetypeId archetype,
        QuantizedFrameData& decoded);
    bool apply_buffered_destroy(ecs::Registry& registry, SyncFrame frame, ClientEntityNetworkId network_id);
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
    bool apply_frame_data(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        SyncFrame frame,
        bool entity_present,
        const QuantizedFrameData& baseline);
    bool apply_snap_sample(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const QuantizedFrameData& decoded,
        bool full);
    bool validate_predicted_archetype(const SyncSettings& settings, SyncArchetypeId archetype) const;
    bool apply_predicted_upsert(
        ecs::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        ClientEntityNetworkId network_id,
        SyncArchetypeId archetype,
        QuantizedFrameData& authoritative,
        bool full);
    bool apply_predicted_destroy(ecs::Registry& registry, SyncFrame frame, ClientEntityNetworkId network_id);
    bool quantize_predicted_entity(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        SyncFrame frame);
    bool run_prediction_frame(ecs::Registry& registry, SyncFrame frame, ecs::RunJobsOptions options);
    bool compare_predicted_frame(
        const SyncSettings& settings,
        EntityState& state,
        SyncFrame frame,
        const QuantizedFrameData& authoritative) const;
    void queue_prediction_rollback(EntityState& state, SyncFrame frame);
    bool apply_pending_prediction_rollback(ecs::Registry& registry, ecs::RunJobsOptions options);
    bool resimulate_all_predicted(ecs::Registry& registry, SyncFrame begin_frame, SyncFrame current_frame, ecs::RunJobsOptions options);
    bool resimulate_affected_predicted(ecs::Registry& registry, SyncFrame begin_frame, SyncFrame current_frame, ecs::RunJobsOptions options);
    const ecs::JobGraph& resim_job_graph(ecs::Registry& registry);
    void collect_resimulated_prediction_entities(std::vector<std::uint32_t>& out) const;
    void capture_original_current_predictions(
        SyncFrame current_frame,
        const std::vector<std::uint32_t>& entity_indices,
        std::vector<OriginalPredictionCapture>& out) const;
    bool blend_resim_errors(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        SyncFrame current_frame,
        const std::vector<OriginalPredictionCapture>& original);
    bool apply_latest_snap(ecs::Registry& registry, const SyncSettings& settings, EntityState& state);
    bool switch_entity_mode(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        ReplicationClientMode mode);
    bool has_buffered_entities() const noexcept;
    bool has_predicted_entities() const noexcept;
    void blend_snap_errors(const SyncSettings& settings, float dt_seconds);
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    void record_interpolation_frame(std::uint64_t checks, std::uint64_t starvations) noexcept;
#endif
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
    void record_server_packet_sequence(std::uint32_t packet_id) noexcept;
    void record_update_timing(SyncFrame server_frame, SyncFrame receive_frame, SyncFrame playback_frame) noexcept;
    void record_ping_sample(float latency_frames) noexcept;
    void advance_control_time_to(SyncFrame receive_frame) noexcept;
    bool receive_connect_response(ecs::Registry& registry, BitBuffer& packet);
    bool receive_pong(BitBuffer& packet, SyncFrame receive_frame);
    void drain_connect_packets(std::vector<BitBuffer>& packets);
    void drain_ping_packets(std::vector<BitBuffer>& packets);
    void drain_ack_packets_into(std::vector<BitBuffer>& packets);
    ClientEntityNetworkId client_entity_network_id_for_wire(std::uint32_t wire_network_id);
    void advance_wire_network_id_version(std::uint32_t wire_network_id);

    struct WireNetworkIdState {
        std::uint32_t version = 0;
        std::uint32_t entity_index = invalid_entity_index;
        bool alive = false;
    };

    ReplicationClientOptions options_;
    ReplicationClientTimingStats timing_stats_;
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    ReplicationClientInterpolationDiagnostics interpolation_diagnostics_;
#endif
    std::vector<ecs::Entity> simulation_jobs_;
    std::vector<ecs::Entity> cosmetic_jobs_;
    ecs::JobGraph resim_job_graph_;
    bool resim_job_graph_valid_ = false;
    std::vector<EntityState> entities_;
    std::vector<std::uint32_t> active_entities_;
    std::vector<std::uint32_t> buffered_entities_;
    std::vector<std::uint32_t> snap_error_entities_;
    std::vector<std::uint32_t> prediction_rollback_entities_;
    std::vector<std::uint32_t> rollback_entity_indices_scratch_;
    std::vector<ecs::Entity> rollback_affected_entities_scratch_;
    std::vector<OriginalPredictionCapture> rollback_original_current_scratch_;
    std::vector<WireNetworkIdState> wire_network_ids_;
    std::unordered_map<ClientEntityNetworkId, std::uint32_t> network_entity_indices_;
    std::vector<std::uint32_t> pending_acks_;
    std::unordered_map<std::uint32_t, SyncFrame> pending_pings_;
    std::unordered_map<std::uint32_t, SyncFrame> destroy_tombstones_;
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
    double prediction_accumulator_seconds_ = 0.0;
    double display_accumulator_seconds_ = 0.0;
    double display_target_frame_ = 0.0;
    SyncFrame receive_frame_ = 0;
    SyncFrame playback_frame_ = 0;
    SyncFrame last_applied_buffered_frame_ = 0;
    bool has_applied_buffered_frame_ = false;
    SyncFrame last_predicted_frame_ = 0;
    bool has_predicted_frame_ = false;
    SyncFrame pending_prediction_rollback_frame_ = 0;
    bool has_pending_prediction_rollback_ = false;
    bool has_display_target_frame_ = false;
    std::uint32_t highest_server_update_packet_id_ = 0;
    std::uint64_t server_update_packet_window_mask_ = 0;
    std::uint32_t server_update_packet_window_span_ = 0;
    bool has_server_update_packet_window_ = false;
};

template <std::size_t NetworkEntityIdTier0Bits = protocol::default_network_entity_id_tier0_bits>
class ReplicationClientT : public ReplicationClient {
    static_assert(
        protocol::valid_network_entity_id_tier0_bits(NetworkEntityIdTier0Bits),
        "NetworkEntityIdTier0Bits must be in [1, 22]");

public:
    static constexpr std::size_t network_entity_id_tier0_bits = NetworkEntityIdTier0Bits;

    explicit ReplicationClientT(ReplicationClientOptions options = {})
        : ReplicationClient(configure(std::move(options))) {}

private:
    static ReplicationClientOptions configure(ReplicationClientOptions options) {
        options.network_entity_id_tier0_bits = NetworkEntityIdTier0Bits;
        return options;
    }
};

}  // namespace kage::sync
