#pragma once

#include "kage/sync/client_clock.hpp"
#include "kage/sync/components.hpp"
#include "kage/sync/logging.hpp"

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
class KTraceDirectoryWriter;
class FractionalTickSampler;
enum class SyncTraceEventType : std::uint8_t;

namespace detail {
class BitReader;
struct FrameDataView;
}  // namespace detail

namespace client_detail {
class ClientAckQueue;
class ClientBufferedRuntime;
class ClientCueRuntime;
class ClientEntityStore;
class ClientPredictionRuntime;
class ClientSessionTransport;
class ClientTimingStatsCalculator;
class ClientDisplaySampler;
class ClientUpdateRuntime;
struct EntityState;
struct EntityCue;
struct EntityPlayedCue;
struct EntityBufferedFrame;
struct EntityFrameView;
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
    friend class client_detail::ClientUpdateRuntime;

    const std::vector<ReplicatedComponentUpdate>* components = nullptr;
};

struct FractionalTickSample {
    ClientEntityNetworkId client_entity_network_id = invalid_client_entity_network_id;
    ecs::Entity local_entity;
    SyncFrame frame = 0;
    float alpha = 0.0f;

    template <typename T>
    bool try_get_sampled_value(const ecs::Registry& registry, T& out) const {
        const ecs::Entity component = registry.component<T>();
        return try_get_sampled_value(registry, component, &out);
    }

    bool try_get_sampled_value(const ecs::Registry& registry, ecs::Entity component, void* out) const;

private:
    friend class ReplicationClient;
    friend class FractionalTickSampler;
    friend class client_detail::ClientDisplaySampler;

    std::vector<ReplicatedComponentUpdate> components_;
};

struct FractionalTickSampleBuffer {
    std::vector<FractionalTickSample> entities;

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

struct ReplicationClientNetworkOptions {
    std::size_t mtu_bytes = 1200;
    protocol::Descriptor protocol = protocol::default_descriptor;
};

struct ReplicationClientEntityOptions {
    ReplicationClientMode default_mode = ReplicationClientMode::Snap;
    EntityModeSelector mode_selector;
};

struct ReplicationClientBufferedOptions {
    SyncFrame buffered_frame_lag = 2;
    bool auto_buffered_frame_lag = true;
    SyncFrame auto_buffered_frame_lag_min = 2;
    float auto_buffered_frame_lag_jitter_multiplier = 2.0f;
    float auto_buffered_frame_lag_smoothing = 0.1f;
    float auto_buffered_time_dilation_min = 0.95f;
    float auto_buffered_time_dilation_max = 1.05f;
    float auto_buffered_time_dilation_gain = 0.05f;
};

struct ReplicationClientPredictionOptions {
    ReplicationRollbackPolicy rollback_policy = ReplicationRollbackPolicy::All;
    std::size_t input_buffer_capacity_frames = 64;
    bool auto_lead_frames = true;
    SyncFrame lead_frames = 2;
    SyncFrame auto_min_frames = 1;
    SyncFrame auto_safety_frames = 1;
    float auto_jitter_multiplier = 2.0f;
    float auto_predicted_time_dilation_min = 0.95f;
    float auto_predicted_time_dilation_max = 1.10f;
    float auto_predicted_time_dilation_gain = 0.05f;
};

struct ReplicationClientSessionOptions {
    ClientId local_client = invalid_client_id;
    std::string connect_token;
    double connect_resend_interval_seconds = 0.25;
    double ping_interval_seconds = 3.0;
    double adaptive_ping_interval_seconds = 0.25;
    std::uint32_t adaptive_ping_stable_samples = 4;
    float adaptive_ping_stable_threshold_frames = 0.5f;
    float adaptive_ping_jump_threshold_frames = 3.0f;
};

struct ReplicationClientClockOptions {
    double fixed_dt_seconds = 1.0 / 60.0;
    std::uint32_t auto_timing_warmup_samples = 3;
    bool auto_timing_fast_recovery = true;
    SyncFrame auto_timing_fast_recovery_min_frame_gap = 4;
    std::uint32_t max_fixed_steps_per_tick = 0;
};

struct ReplicationClientOptions {
    ReplicationClientNetworkOptions network;
    ReplicationClientEntityOptions entities;
    ReplicationClientBufferedOptions buffered;
    ReplicationClientPredictionOptions prediction;
    ReplicationClientSessionOptions session;
    ReplicationClientClockOptions clock;
    LoggingOptions logging;
#ifdef KAGE_SYNC_ENABLE_TRACING
    TraceOptions trace;
#endif
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

template <typename JobBuilder, bool IsCosmeticJob>
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
        if constexpr (IsCosmeticJob) {
            registry_->template add<NoResim>(job);
        }
        jobs_->push_back(job);
        return job;
    }

    template <typename... AccessComponents>
    auto access_other_entities() const {
        return ClientJobBuilder<decltype(builder_.template access_other_entities<AccessComponents...>()), IsCosmeticJob>(
            *registry_,
            builder_.template access_other_entities<AccessComponents...>(),
            *jobs_);
    }

    template <typename... OptionalComponents>
    auto optional() const {
        return ClientJobBuilder<decltype(builder_.template optional<OptionalComponents...>()), IsCosmeticJob>(
            *registry_,
            builder_.template optional<OptionalComponents...>(),
            *jobs_);
    }

    template <typename... StructuralComponents>
    auto structural() const {
        return ClientJobBuilder<decltype(builder_.template structural<StructuralComponents...>()), IsCosmeticJob>(
            *registry_,
            builder_.template structural<StructuralComponents...>(),
            *jobs_);
    }

    template <typename... Tags>
    auto with_tags() const {
        return ClientJobBuilder<decltype(builder_.template with_tags<Tags...>()), IsCosmeticJob>(
            *registry_,
            builder_.template with_tags<Tags...>(),
            *jobs_);
    }

    template <typename... Tags>
    auto without_tags() const {
        return ClientJobBuilder<decltype(builder_.template without_tags<Tags...>()), IsCosmeticJob>(
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
    struct ObservabilityStats {
        std::uint64_t server_packet_warnings = 0;
        std::uint64_t suppressed_server_packet_warnings = 0;
        std::uint64_t client_errors = 0;
        std::uint64_t client_connects_accepted = 0;
        std::uint64_t client_connects_rejected = 0;
    };

    static constexpr std::size_t buffered_frame_capacity = 64;
    static constexpr std::size_t prediction_frame_capacity = 64;

    explicit ReplicationClient(ecs::Registry& registry, ReplicationClientOptions options = {});
    ~ReplicationClient();
    ReplicationClient(const ReplicationClient& other) = delete;
    ReplicationClient& operator=(const ReplicationClient& other) = delete;
    ReplicationClient(ReplicationClient&& other) noexcept;
    ReplicationClient& operator=(ReplicationClient&& other) noexcept;

    const ReplicationClientOptions& options() const noexcept {
        return options_;
    }
    double fixed_dt_seconds() const noexcept {
        return fixed_dt_seconds_;
    }
    void set_logger(std::shared_ptr<spdlog::logger> logger);
    void set_log_level(LogLevel level);
    LogLevel log_level() const noexcept {
        return options_.logging.level;
    }
#ifdef KAGE_SYNC_ENABLE_TRACING
    void set_tracer(SyncTracer* tracer) noexcept;
    void set_trace_options(TraceOptions options);
    void flush_trace();
    void close_trace();
#endif

    bool set_default_entity_mode(ReplicationClientMode mode) noexcept;
    void set_entity_mode(
        ecs::Registry& registry,
        ClientEntityNetworkId client_entity_network_id,
        ReplicationClientMode mode);
    bool has_entity(ClientEntityNetworkId client_entity_network_id) const noexcept;
    ReplicationClientMode entity_mode(ClientEntityNetworkId client_entity_network_id) const noexcept;
    bool set_buffered_frame_lag(SyncFrame frames) noexcept;
    SyncFrame current_buffered_frame_lag() const noexcept;
    template <typename T>
    bool set_input(ecs::Registry& registry, const T& input) {
        const ecs::Entity component = registry.template component<T>();
        const SyncSettings& settings = registry.template get<SyncSettings>();
        if (settings.input_component != component) {
            return false;
        }
        return set_input_bytes(registry, component, &input);
    }
    bool tick(ecs::Registry& registry, double dt_seconds, ecs::RunJobsOptions prediction_options = {});
    void set_packet_sender(std::function<void(const ecs::BitBuffer&)> sender);
    void receive_packet(ecs::BitBuffer packet);
    bool receive(ecs::Registry& registry, ecs::BitBuffer packet);
    template <typename... Components>
    auto cosmetic_job(ecs::Registry& registry, int order) {
        auto builder = registry.template job<Components...>(order);
        return detail::ClientJobBuilder<decltype(builder), true>(registry, std::move(builder), cosmetic_jobs_);
    }
    template <typename... Components>
    auto simulation_job(ecs::Registry& registry, int order) {
        auto builder = registry.template job<Components...>(order).template without_tags<const NoSimulate>();
        return detail::ClientJobBuilder<decltype(builder), false>(registry, std::move(builder), simulation_jobs_);
    }
    bool apply_frame(ecs::Registry& registry, SyncFrame buffered_frame);
    bool sample_fractional_tick_frame(
        const ecs::Registry& registry,
        double target_frame,
        FractionalTickSampleBuffer& out) const;
    const FractionalTickSampleBuffer& fractional_tick_frame(const ecs::Registry& registry);
    std::vector<ecs::BitBuffer> drain_packets();
    std::vector<ecs::BitBuffer> drain_ack_packets();
    std::size_t pending_ack_count() const noexcept;
    bool is_alive_client_entity_network_id(ClientEntityNetworkId client_entity_network_id) const noexcept;
    ecs::Entity local_entity(ClientEntityNetworkId client_entity_network_id) const;
    EntityReferenceStatus resolve_entity_reference(EntityReference& reference) const noexcept;
    ClientId client_id() const noexcept {
        return client_id_;
    }
    ReplicationClientConnectionState connection_state() const noexcept;
    const std::string& connect_error() const noexcept;
    double local_time_seconds() const noexcept {
        return clock_.local_time_seconds();
    }
    SyncFrame buffered_frame() const noexcept {
        return clock_.buffered_frame();
    }
    double continuous_buffered_frame() const noexcept {
        return clock_.continuous_buffered_frame();
    }
    SyncFrame predicted_frame() const noexcept {
        return clock_.predicted_frame();
    }
    double continuous_predicted_frame() const noexcept {
        return clock_.continuous_predicted_frame();
    }
    double fractional_tick_target_frame() const noexcept {
        return clock_.display_target_frame();
    }
    bool has_applied_buffered_frame() const noexcept;
    SyncFrame last_applied_buffered_frame() const noexcept;
    double estimated_server_frame() const noexcept;
    double continuous_prediction_frames_ahead() const noexcept;
    double continuous_buffered_frames_behind() const noexcept;
    const ReplicationClientTimingStats& timing_stats() const noexcept {
        return clock_.stats();
    }
    ObservabilityStats observability_stats() const noexcept {
        return observability_stats_;
    }
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    const ReplicationClientInterpolationDiagnostics& interpolation_diagnostics() const noexcept {
        return interpolation_diagnostics_;
    }
    void reset_interpolation_diagnostics() noexcept {
        interpolation_diagnostics_ = {};
    }
#endif

protected:
    struct FrameHistoryCapacities {
        std::size_t buffered = buffered_frame_capacity;
        std::size_t predicted = prediction_frame_capacity;
    };

    ReplicationClient(ecs::Registry& registry, ReplicationClientOptions options, FrameHistoryCapacities capacities);

private:
    friend class client_detail::ClientBufferedRuntime;
    friend class client_detail::ClientCueRuntime;
    friend class client_detail::ClientPredictionRuntime;
    friend class client_detail::ClientUpdateRuntime;

    static constexpr std::size_t invalid_ack_index = std::numeric_limits<std::size_t>::max();
    static constexpr std::uint32_t invalid_entity_index = std::numeric_limits<std::uint32_t>::max();

    using EntityState = client_detail::EntityState;
    using EntityCue = client_detail::EntityCue;
    using EntityPlayedCue = client_detail::EntityPlayedCue;
    using EntityBufferedFrame = client_detail::EntityBufferedFrame;
    using OriginalPredictionCapture = client_detail::OriginalPredictionCapture;
    struct RegistryFrameApplyInfo;
    struct ReceiveContext {
        double local_time_seconds = 0.0;
        double estimated_server_time_seconds = 0.0;
        double estimated_server_frame = 0.0;
        SyncFrame buffered_frame = 0;
        double continuous_buffered_frame = 0.0;
        double continuous_predicted_frame = 0.0;
    };

    EntityState* find_entity_state(ClientEntityNetworkId client_entity_network_id) noexcept;
    const EntityState* find_entity_state(ClientEntityNetworkId client_entity_network_id) const noexcept;
    EntityState* find_entity_state_by_wire_id(std::uint32_t wire_network_id) noexcept;
    const EntityState* find_entity_state_by_wire_id(std::uint32_t wire_network_id) const noexcept;
    EntityState* find_entity_state_for_local(ecs::Entity local) noexcept;
#ifdef KAGE_SYNC_ENABLE_TRACING
    void register_local_entity_index(const EntityState& state);
    void unregister_local_entity_index(const EntityState& state);
#endif
    EntityState* ensure_entity_state(
        ecs::Registry& registry,
        ClientEntityNetworkId client_entity_network_id,
        std::uint32_t wire_network_id);
    void erase_entity_state(ecs::Registry& registry, std::uint32_t entity_index, bool destroy_local);
    void sync_entity_memberships(EntityState& state);
    bool destroy_tombstone_blocks(std::uint32_t wire_network_id, SyncFrame frame) const;
    void record_destroy_tombstone(std::uint32_t wire_network_id, SyncFrame frame);
    const QuantizedFrameData* find_baseline(const EntityState& state, SyncFrame frame) const noexcept;
    bool validate_buffered_archetype(const SyncSettings& settings, SyncArchetypeId archetype) const;
    bool ensure_local_entity(ecs::Registry& registry, EntityState& state);
    std::uint64_t registry_tag_mask(
        const ecs::Registry& registry,
        ecs::Entity entity,
        const SyncArchetype& archetype) const;
    void trace_applied_tag_delta(
        const SyncArchetype& archetype,
        const EntityState& state,
        SyncFrame frame,
        std::uint64_t previous_tag_mask,
        std::uint64_t next_tag_mask);
    bool apply_registry_tags(
        ecs::Registry& registry,
        const SyncArchetype& archetype,
        EntityState& state,
        SyncFrame frame,
        std::uint64_t tag_mask,
        bool verify_tag_apply);
    bool remove_missing_registry_components(
        ecs::Registry& registry,
        const SyncArchetype& archetype,
        const EntityState& state,
        SyncFrame frame,
        std::uint64_t previous_present_mask,
        std::uint64_t next_present_mask);
    bool apply_registry_frame(
        ecs::Registry& registry,
        const SyncArchetype& archetype,
        EntityState& state,
        const RegistryFrameApplyInfo& input);
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
        const client_detail::EntityFrameView& sample);
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
    bool quantize_predicted_entity(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        SyncFrame frame);
    void apply_buffered_frames_to_ecs(
        ecs::Registry& registry,
        const ReplicationClientClock::FrameRange& frames);
    bool run_predicted_frames(
        ecs::Registry& registry,
        const ReplicationClientClock::FrameRange& frames,
        ecs::RunJobsOptions options);
#ifdef KAGE_SYNC_ENABLE_TRACING
    void trace_local_time_frames(ecs::Registry& registry, const ReplicationClientClock::FrameRange& frames);
#endif
    bool compare_predicted_frame(
        const SyncSettings& settings,
        EntityState& state,
        SyncFrame frame,
        const QuantizedFrameData& authoritative) const;
    const ecs::JobGraph& resim_job_graph(ecs::Registry& registry);
    bool apply_latest_snap(ecs::Registry& registry, const SyncSettings& settings, EntityState& state);
    bool switch_entity_mode(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        ReplicationClientMode mode);
    enum class BaselineUpdate {
        KeepExisting,
        ReplaceAndApplyMask
    };
    void mark_mode_auto_selected(EntityState& state, ReplicationClientMode mode) noexcept;
    void mark_mode_user_selected(EntityState& state, ReplicationClientMode mode) noexcept;
    void reset_absent_entity_state(EntityState& state, SyncArchetypeId archetype) noexcept;
    void record_authoritative_present(
        EntityState& state,
        SyncFrame frame,
        SyncArchetypeId archetype,
        QuantizedFrameData baseline,
        BaselineUpdate baseline_update);
    void record_authoritative_absent(EntityState& state, SyncFrame frame);
    bool transition_to_snap(ecs::Registry& registry, const SyncSettings& settings, EntityState& state);
    bool transition_to_buffered(const SyncSettings& settings, EntityState& state);
    bool transition_to_predict(ecs::Registry& registry, const SyncSettings& settings, EntityState& state);
    bool has_buffered_entities() const noexcept;
    bool has_predicted_entities() const noexcept;
    void blend_snap_errors(const SyncSettings& settings, float dt_seconds);
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    void record_interpolation_frame(std::uint64_t checks, std::uint64_t starvations) noexcept;
#endif
    void remember_baseline(EntityState& state);
    void queue_ack(std::uint32_t packet_id);
    bool receive_connect_response(ecs::Registry& registry, ecs::BitBuffer& packet);
    bool receive_pong(ecs::Registry& registry, ecs::BitBuffer& packet, const ReceiveContext& context);
    bool receive_entity_update(ecs::Registry& registry, ecs::BitBuffer& packet, const ReceiveContext& context);
    bool update_prediction_input_prefill_from_entity_update(
        ecs::Registry& registry,
        SyncFrame server_frame,
        const ReceiveContext& context);
    bool apply_prediction_input_prefill(
        ecs::Registry& registry,
        SyncFrame server_frame,
        SyncFrame prefill_input_frame,
        SyncFrame prediction_snap_lead_frames);
    void drain_connect_packets(std::vector<ecs::BitBuffer>& packets);
    void drain_ping_packets(std::vector<ecs::BitBuffer>& packets);
    void drain_ack_packets_into(std::vector<ecs::BitBuffer>& packets);
    void drain_input_packets_into(std::vector<ecs::BitBuffer>& packets);
    void process_inbound_packets(ecs::Registry& registry);
    void send_pending_packets();
    void log_info(const char* event, const std::string& fields) const;
    void log_server_packet_warning(std::uint8_t message, const char* reason_code, const char* reason_detail);
    void log_client_error(std::uint8_t message, const char* event, const char* reason);
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
        double observed_server_frame,
        SyncFrame observed_buffered_frame,
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
    void set_client_id(ecs::Registry& registry, ClientId client) noexcept;
    bool record_input_frame(ecs::Registry& registry, const SyncSettings& settings, SyncFrame frame);
    bool fill_input_frames_through(ecs::Registry& registry, const SyncSettings& settings, SyncFrame frame);
    bool apply_input_frame(ecs::Registry& registry, const SyncSettings& settings, SyncFrame frame);

    ReplicationClientOptions options_;
    double fixed_dt_seconds_ = 1.0 / 60.0;
    ReplicationClientClock clock_;
#ifdef KAGE_SYNC_ENABLE_INTERPOLATION_DIAGNOSTICS
    ReplicationClientInterpolationDiagnostics interpolation_diagnostics_;
#endif
    std::vector<ecs::Entity> simulation_jobs_;
    std::vector<ecs::Entity> cosmetic_jobs_;
    ecs::JobGraph resim_job_graph_;
    bool resim_job_graph_valid_ = false;
    std::unique_ptr<client_detail::ClientEntityStore> entity_store_;
    std::unique_ptr<client_detail::ClientPredictionRuntime> prediction_;
    std::unique_ptr<client_detail::ClientSessionTransport> session_transport_;
    std::unique_ptr<client_detail::ClientCueRuntime> cue_runtime_;
    std::unique_ptr<client_detail::ClientBufferedRuntime> buffered_runtime_;
    std::unique_ptr<client_detail::ClientAckQueue> ack_queue_;
    std::unique_ptr<client_detail::ClientTimingStatsCalculator> timing_stats_;
    std::unique_ptr<client_detail::ClientUpdateRuntime> update_runtime_;
    std::unique_ptr<client_detail::ClientInputBuffer> input_;
    std::shared_ptr<spdlog::logger> logger_;
    ObservabilityStats observability_stats_;
    std::unordered_map<std::uint8_t, std::uint32_t> warning_logs_by_message_;
    SyncFrame last_server_update_frame_ = 0;
    bool has_received_server_update_ = false;
    FractionalTickSampleBuffer fractional_tick_frame_;
    FractionalTickSampleBuffer fractional_tick_scratch_;
    ClientId client_id_ = invalid_client_id;
#ifdef KAGE_SYNC_ENABLE_TRACING
    SyncTracer* tracer_ = nullptr;
    std::unique_ptr<KTraceDirectoryWriter> trace_writer_;
#endif
};

namespace detail {

constexpr bool client_frame_capacity_power_of_two(std::size_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
}

}  // namespace detail

template <
    std::size_t NetworkEntityIdTier0Bits = protocol::default_network_entity_id_tier0_bits,
    std::size_t BufferedFrameCapacity = ReplicationClient::buffered_frame_capacity,
    std::size_t PredictionFrameCapacity = ReplicationClient::prediction_frame_capacity>
class ReplicationClientT : public ReplicationClient {
    static_assert(
        protocol::valid_network_entity_id_tier0_bits(NetworkEntityIdTier0Bits),
        "NetworkEntityIdTier0Bits must be in [1, 22]");
    static_assert(
        detail::client_frame_capacity_power_of_two(BufferedFrameCapacity),
        "BufferedFrameCapacity must be a nonzero power of two");
    static_assert(
        detail::client_frame_capacity_power_of_two(PredictionFrameCapacity),
        "PredictionFrameCapacity must be a nonzero power of two");

public:
    static constexpr std::size_t network_entity_id_tier0_bits = NetworkEntityIdTier0Bits;
    static constexpr std::size_t buffered_frame_capacity = BufferedFrameCapacity;
    static constexpr std::size_t prediction_frame_capacity = PredictionFrameCapacity;

    explicit ReplicationClientT(ecs::Registry& registry, ReplicationClientOptions options = {})
        : ReplicationClient(
              registry,
              configure(std::move(options)),
              FrameHistoryCapacities{BufferedFrameCapacity, PredictionFrameCapacity}) {}

private:
    static ReplicationClientOptions configure(ReplicationClientOptions options) {
        options.network.protocol.network_entity_id_tier0_bits = NetworkEntityIdTier0Bits;
        return options;
    }
};

}  // namespace kage::sync
