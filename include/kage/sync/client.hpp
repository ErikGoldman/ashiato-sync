#pragma once

#include "kage/sync/client_clock.hpp"
#include "kage/sync/components.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kage::sync {

class SyncTracer;
enum class SyncTraceEventType : std::uint8_t;

namespace client_detail {
class ClientAckQueue;
struct EntityState;
struct EntityCue;
struct EntityPlayedCue;
struct EntityBufferedFrame;
struct OriginalPredictionCapture;
struct WireNetworkIdState;
struct InputFrame;
struct PendingPing;
class ClientInputBuffer;
}  // namespace client_detail

struct ReplicatedComponentUpdate {
    ecs::Entity component;
    SyncComponentOps::QuantizedBytes bytes;
};

struct ReplicatedEntityUpdateView {
    ClientEntityNetworkId client_entity_network_id = invalid_client_entity_network_id;
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

struct DisplayInterpolationSample {
    ClientEntityNetworkId client_entity_network_id = invalid_client_entity_network_id;
    ecs::Entity local_entity;
    SyncArchetypeId archetype;
    SyncFrame frame = 0;
    float alpha = 0.0f;
    std::uint64_t tag_mask = 0;
    std::vector<ReplicatedComponentUpdate> components;

    template <typename T>
    bool try_get_display_value(const ecs::Registry& registry, T& out) const {
        const ecs::Entity component = registry.component<T>();
        return try_get_display_value(registry, component, &out);
    }

    bool try_get_display_value(const ecs::Registry& registry, ecs::Entity component, void* out) const;
    bool has_tag(const ecs::Registry& registry, ecs::Entity tag) const;
};

struct DisplayInterpolationSampleBuffer {
    std::vector<DisplayInterpolationSample> entities;

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

enum class EntityReferenceStatus {
    Invalid,
    Pending,
    Alive,
    Destroyed
};

enum class ClientStatus {
    Ok,
    EntityNotFound,
    EntityUnavailable,
    ModeSwitchFailed,
    MissingPredictionRollbackTrait
};

class ClientError : public std::runtime_error {
public:
    ClientError(ClientStatus status, const char* message)
        : std::runtime_error(message), status_(status) {}

    ClientStatus status() const noexcept {
        return status_;
    }

private:
    ClientStatus status_;
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
    protocol::Descriptor protocol = protocol::default_descriptor;
    std::string connect_token;
    double connect_resend_interval_seconds = 0.25;
    double ping_interval_seconds = 3.0;
    ReplicationRollbackPolicy rollback_policy = ReplicationRollbackPolicy::All;
    std::size_t prediction_buffer_capacity_frames = 64;
    std::size_t input_buffer_capacity_frames = 64;
    bool auto_prediction_lead_frames = true;
    SyncFrame prediction_lead_frames = 2;
    SyncFrame auto_prediction_min_frames = 1;
    SyncFrame auto_prediction_safety_frames = 1;
    float auto_prediction_jitter_multiplier = 2.0f;
    float auto_prediction_time_dilation_min = 0.95f;
    float auto_prediction_time_dilation_max = 1.10f;
    float auto_prediction_time_dilation_gain = 0.05f;
    std::uint32_t auto_timing_warmup_samples = 3;
    bool auto_timing_fast_recovery = true;
    SyncFrame auto_timing_fast_recovery_min_frame_gap = 4;
    bool adaptive_ping_interval = true;
    double adaptive_ping_interval_seconds = 0.25;
    std::uint32_t adaptive_ping_stable_samples = 4;
    float adaptive_ping_stable_threshold_frames = 0.5f;
    float adaptive_ping_jump_threshold_frames = 3.0f;
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
    auto access_other_entities() const {
        return ClientJobBuilder<decltype(builder_.template access_other_entities<AccessComponents...>()), AddNoResimTag>(
            *registry_,
            builder_.template access_other_entities<AccessComponents...>(),
            *jobs_);
    }

    template <typename... OptionalComponents>
    auto optional() const {
        return ClientJobBuilder<decltype(builder_.template optional<OptionalComponents...>()), AddNoResimTag>(
            *registry_,
            builder_.template optional<OptionalComponents...>(),
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
    ~ReplicationClient();
    ReplicationClient(const ReplicationClient& other) = delete;
    ReplicationClient& operator=(const ReplicationClient& other) = delete;
    ReplicationClient(ReplicationClient&& other) noexcept;
    ReplicationClient& operator=(ReplicationClient&& other) noexcept;

    const ReplicationClientOptions& options() const noexcept {
        return options_;
    }
#ifdef KAGE_SYNC_ENABLE_TRACING
    void set_tracer(SyncTracer* tracer) noexcept;
#endif

    bool set_default_entity_mode(ReplicationClientMode mode) noexcept;
    void set_entity_mode(ecs::Registry& registry, ClientEntityNetworkId network_id, ReplicationClientMode mode);
    ReplicationClientMode entity_mode(ClientEntityNetworkId network_id) const noexcept;
    bool set_interpolation_buffer_frames(SyncFrame frames) noexcept;
    SyncFrame current_interpolation_buffer_frames() const noexcept;
    template <typename T>
    bool set_input(ecs::Registry& registry, const T& input) {
        register_components(registry);
        const ecs::Entity component = registry.template component<T>();
        const SyncSettings& settings = registry.template get<SyncSettings>();
        if (settings.input_component != component) {
            return false;
        }
        return set_input_bytes(registry, component, &input);
    }
    bool tick(ecs::Registry& registry, double dt_seconds);
    void set_packet_sender(std::function<void(const BitBuffer&)> sender);
    void receive_packet(BitBuffer packet);
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
    bool sample_display_interpolation_target_frame(
        const ecs::Registry& registry,
        double target_frame,
        DisplayInterpolationSampleBuffer& out) const;
    bool sample_display_interpolation_frame(
        const ecs::Registry& registry,
        double client_frame,
        DisplayInterpolationSampleBuffer& out) const;
    const DisplayInterpolationSampleBuffer& display_interpolation_frame(const ecs::Registry& registry);
    std::vector<BitBuffer> drain_packets();
    std::vector<BitBuffer> drain_ack_packets();
    std::size_t pending_ack_count() const noexcept;
    bool is_alive_network_id(ClientEntityNetworkId network_id) const noexcept;
    ecs::Entity local_entity(ClientEntityNetworkId network_id) const;
    EntityReferenceStatus resolve_entity_reference(EntityReference& reference) const noexcept;
    ClientId client_id() const noexcept {
        return client_id_;
    }
    ReplicationClientConnectionState connection_state() const noexcept {
        return connection_state_;
    }
    const std::string& connect_error() const noexcept {
        return connect_error_;
    }
    SyncFrame receive_frame() const noexcept {
        return clock_.receive_frame();
    }
    double continuous_receive_frame() const noexcept {
        return clock_.continuous_receive_frame();
    }
    SyncFrame playback_frame() const noexcept {
        return clock_.playback_frame();
    }
    double continuous_playback_frame() const noexcept {
        return clock_.continuous_playback_frame();
    }
    SyncFrame input_frame() const noexcept {
        return clock_.input_frame();
    }
    double continuous_input_frame() const noexcept {
        return clock_.continuous_input_frame();
    }
    double display_target_frame() const noexcept {
        return clock_.display_target_frame();
    }
    double estimated_server_frame() const noexcept;
    double continuous_prediction_frames_ahead() const noexcept;
    double continuous_interpolation_frames_behind() const noexcept;
    const ReplicationClientTimingStats& timing_stats() const noexcept {
        return clock_.stats();
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

    using EntityState = client_detail::EntityState;
    using EntityCue = client_detail::EntityCue;
    using EntityPlayedCue = client_detail::EntityPlayedCue;
    using EntityBufferedFrame = client_detail::EntityBufferedFrame;
    using OriginalPredictionCapture = client_detail::OriginalPredictionCapture;

    EntityState* find_entity_state(ClientEntityNetworkId network_id) noexcept;
    const EntityState* find_entity_state(ClientEntityNetworkId network_id) const noexcept;
    EntityState* find_entity_state(std::uint32_t network_id) noexcept;
    const EntityState* find_entity_state(std::uint32_t network_id) const noexcept;
    EntityState* find_entity_state_for_local(ecs::Entity local) noexcept;
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
    void compact_destroy_tombstone_ages();
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
    bool read_cues(BitBuffer& packet, std::vector<EntityCue>& out);
    bool play_cue(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const EntityCue& cue,
        float late_seconds,
        bool confirmed);
    bool rollback_played_cue(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const EntityPlayedCue& cue,
        const char* rollback_reason);
    void play_snap_cues(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const std::vector<EntityCue>& cues);
    void store_buffered_cues(EntityState& state, SyncFrame frame, const std::vector<EntityCue>& cues);
    void reconcile_authoritative_predicted_cues(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const std::vector<EntityCue>& cues,
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
        const EntityBufferedFrame& sample);
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
        DisplayInterpolationSampleBuffer& out) const;
    ComponentInterpolation interpolation_for(
        const SyncSettings& settings,
        SyncArchetypeId archetype,
        ecs::Entity component) const;
    void remember_baseline(EntityState& state);
    void queue_ack(std::uint32_t packet_id);
    void record_server_packet_sequence(std::uint32_t packet_id) noexcept;
    bool receive_connect_response(ecs::Registry& registry, BitBuffer& packet);
    bool receive_pong(ecs::Registry& registry, BitBuffer& packet, SyncFrame receive_frame);
    void drain_connect_packets(std::vector<BitBuffer>& packets);
    void drain_ping_packets(std::vector<BitBuffer>& packets);
    void drain_ack_packets_into(std::vector<BitBuffer>& packets);
    void drain_input_packets_into(std::vector<BitBuffer>& packets);
    bool process_inbound_packets(ecs::Registry& registry);
    void send_pending_packets();
    ClientEntityNetworkId client_entity_network_id_for_wire(std::uint32_t wire_network_id);
    void advance_wire_network_id_version(std::uint32_t wire_network_id);
#ifdef KAGE_SYNC_ENABLE_TRACING
    enum class TraceFrameComponentScope {
        All,
        NonPredicted,
        Predicted
    };

    void trace_frame_components(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        bool resimulated = false,
        bool only_pending_rollback = false,
        TraceFrameComponentScope scope = TraceFrameComponentScope::All);
    void trace_input_components(
        ecs::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        ecs::Entity component,
        const std::uint8_t* quantized);
    void trace_clock_skew(
        const char* stage,
        SyncFrame local_frame,
        SyncFrame server_frame,
        SyncFrame observed_receive_frame,
        SyncFrame observed_playback_frame,
        SyncFrame last_recorded_input_frame,
        SyncFrame prefill_input_frame) const;
    void trace_cue_event(
        SyncTraceEventType type,
        const SyncSettings& settings,
        const EntityState& state,
        const EntityCue& cue,
        const char* rollback_reason = nullptr,
        const char* cue_source = nullptr) const;
    void trace_cue_event(
        SyncTraceEventType type,
        const SyncSettings& settings,
        const EntityState& state,
        const EntityPlayedCue& cue,
        const char* rollback_reason = nullptr,
        const char* cue_source = nullptr) const;
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    void trace_outgoing_ack_packet(const std::vector<std::uint32_t>& acks) const;
    void trace_outgoing_input_packet(
        const std::vector<std::uint32_t>& acks,
        SyncFrame baseline_frame,
        SyncFrame first_input_frame,
        SyncFrame last_input_frame) const;
    void trace_incoming_update_packet(
        SyncFrame local_frame,
        SyncFrame server_frame,
        std::uint32_t packet_id,
        SyncFrame input_ack_frame,
        std::uint16_t record_count) const;
#endif
#endif
    bool set_input_bytes(ecs::Registry& registry, ecs::Entity component, const void* input);
    bool record_input_frame(ecs::Registry& registry, const SyncSettings& settings, SyncFrame frame);
    bool fill_input_frames_through(ecs::Registry& registry, const SyncSettings& settings, SyncFrame frame);
    bool apply_input_frame(ecs::Registry& registry, const SyncSettings& settings, SyncFrame frame);
    void acknowledge_input_frame(SyncFrame frame);

    using WireNetworkIdState = client_detail::WireNetworkIdState;

    ReplicationClientOptions options_;
    ReplicationClientClock clock_;
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    ReplicationClientInterpolationDiagnostics interpolation_diagnostics_;
#endif
    std::vector<ecs::Entity> simulation_jobs_;
    std::vector<ecs::Entity> cosmetic_jobs_;
    ecs::JobGraph resim_job_graph_;
    bool resim_job_graph_valid_ = false;
    std::vector<EntityState> entities_;
    std::vector<std::uint32_t> free_entity_indices_;
    std::vector<std::uint32_t> active_entities_;
    std::vector<std::uint32_t> buffered_entities_;
    std::vector<std::uint32_t> snap_error_entities_;
    std::vector<std::uint32_t> prediction_rollback_entities_;
    std::vector<std::uint32_t> rollback_entity_indices_scratch_;
    std::vector<ecs::Entity> rollback_affected_entities_scratch_;
    std::vector<OriginalPredictionCapture> rollback_original_current_scratch_;
    std::vector<WireNetworkIdState> wire_network_ids_;
    std::unordered_map<ClientEntityNetworkId, std::uint32_t> network_entity_indices_;
    std::unique_ptr<client_detail::ClientAckQueue> ack_queue_;
    std::unique_ptr<client_detail::ClientInputBuffer> input_;
    SyncFrame last_server_update_frame_ = 0;
    double last_server_update_receive_frame_ = 0.0;
    bool has_received_server_update_ = false;
    using PendingPing = client_detail::PendingPing;

    struct DestroyTombstone {
        SyncFrame frame = 0;
        std::uint32_t generation = 0;
        std::uint64_t age_order = 0;
    };

    struct DestroyTombstoneAgeEntry {
        std::uint32_t wire_network_id = 0;
        std::uint32_t generation = 0;
        std::uint64_t age_order = 0;
    };

    std::unordered_map<std::uint32_t, PendingPing> pending_pings_;
    std::unordered_map<std::uint32_t, DestroyTombstone> destroy_tombstones_;
    std::vector<DestroyTombstoneAgeEntry> destroy_tombstone_ages_;
    std::size_t destroy_tombstone_age_begin_ = 0;
    std::uint64_t destroy_tombstone_next_age_order_ = 0;
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    std::vector<std::string> current_packet_cue_summaries_;
#endif
    std::vector<BitBuffer> inbound_packets_;
    std::function<void(const BitBuffer&)> packet_sender_;
    DisplayInterpolationSampleBuffer display_frame_;
    DisplayInterpolationSampleBuffer display_scratch_;
    std::string connect_error_;
    ClientId client_id_ = invalid_client_id;
    ReplicationClientConnectionState connection_state_ = ReplicationClientConnectionState::Connecting;
    double connect_resend_accumulator_seconds_ = 0.0;
    double ping_accumulator_seconds_ = 0.0;
    std::uint32_t next_ping_sequence_ = 1;
    std::uint32_t stable_ping_samples_ = 0;
    bool sent_initial_connect_request_ = false;
    bool sent_initial_ping_ = false;
    bool adaptive_ping_active_ = true;
    SyncFrame last_applied_buffered_frame_ = 0;
    bool has_applied_buffered_frame_ = false;
    SyncFrame last_predicted_frame_ = 0;
    bool has_predicted_frame_ = false;
    SyncFrame active_prediction_snap_lead_frames_ = 0;
    SyncFrame pending_prediction_catchup_frame_ = 0;
    SyncFrame pending_prediction_catchup_server_frame_ = 0;
    SyncFrame pending_prediction_rollback_frame_ = 0;
    bool has_pending_prediction_rollback_ = false;
    std::uint32_t highest_server_update_packet_id_ = 0;
    std::uint64_t server_update_packet_window_mask_ = 0;
    std::uint32_t server_update_packet_window_span_ = 0;
    bool has_server_update_packet_window_ = false;
#ifdef KAGE_SYNC_ENABLE_TRACING
    SyncTracer* tracer_ = nullptr;
#endif
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
        options.protocol.network_entity_id_tier0_bits = NetworkEntityIdTier0Bits;
        return options;
    }
};

}  // namespace kage::sync
