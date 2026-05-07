#pragma once

#include "kage/sync/components.hpp"
#include "kage/sync/server_frame_consumer.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kage::sync {

class SyncTracer;
class KTraceDirectoryWriter;
class FractionalTickSampler;

namespace server_detail {
struct ClientDirtyQueue;
struct ClientEntityState;
struct PacketAckRecord;
struct ServerClientReplicator;
struct SerializedEntity;
}

class ReplicationServer {
public:
    struct ClientInputStats {
        std::uint64_t input_frames_applied = 0;
        std::uint64_t input_starvation_frames = 0;
        std::uint64_t input_reused_frames = 0;
        SyncFrame latest_received_input_frame = 0;
        SyncFrame latest_applied_input_frame = 0;
    };

    struct ClientBandwidthStats {
        bool dynamic = false;
        double target_bytes_per_second = 0.0;
        double available_bytes = 0.0;
        std::size_t in_flight_bytes = 0;
        std::uint64_t delivered_bytes_window = 0;
        float loss_rate = 0.0f;
    };

    struct ObservabilityStats {
        std::uint64_t client_packet_warnings = 0;
        std::uint64_t suppressed_client_packet_warnings = 0;
        std::uint64_t server_errors = 0;
        std::uint64_t client_connects_accepted = 0;
        std::uint64_t client_connects_rejected = 0;
        std::uint64_t clients_ready = 0;
        std::uint64_t clients_removed = 0;
        std::uint64_t clients_timed_out = 0;
    };

    explicit ReplicationServer(ReplicationServerOptions options = {});
    ~ReplicationServer();
    ReplicationServer(const ReplicationServer& other) = delete;
    ReplicationServer& operator=(const ReplicationServer& other) = delete;
    ReplicationServer(ReplicationServer&& other) noexcept;
    ReplicationServer& operator=(ReplicationServer&& other) noexcept;

    const ReplicationServerOptions& options() const noexcept {
        return options_;
    }

    void set_transport(TransportFn transport);
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

    bool add_client(ClientId client);
    ClientId add_local_client();
    ClientId local_client() const noexcept {
        return local_client_;
    }
    bool is_local_client(ClientId client) const noexcept;
    bool remove_client(ClientId client);
    bool has_client(ClientId client) const;
    std::size_t client_count() const noexcept;

    void rediscover_all_replicated_entities(ecs::Registry& registry);
    bool is_replicated(ecs::Entity entity) const;
    std::size_t replicated_count() const noexcept;

    bool acknowledge_entity(ClientId client, ecs::Entity entity, SyncFrame frame);
    void receive_packet(ClientId client, ecs::BitBuffer packet);
    bool process_packet(ClientId client, ecs::BitBuffer packet);
    bool process_packet(ecs::Registry& registry, ClientId client, ecs::BitBuffer packet);
    template <typename T>
    bool set_local_input(ecs::Registry& registry, const T& input) {
        register_components(registry);
        const ecs::Entity component = registry.template component<T>();
        const SyncSettings& settings = registry.template get<SyncSettings>();
        if (settings.input_component != component) {
            return false;
        }
        return set_local_input_bytes(registry, component, &input);
    }
    bool set_local_input_bytes(ecs::Registry& registry, ecs::Entity component, const void* input);
    ClientInputStats input_stats(ClientId client) const noexcept;
    ClientBandwidthStats bandwidth_stats(ClientId client) const noexcept;
    ObservabilityStats observability_stats() const noexcept {
        return observability_stats_;
    }
    std::size_t retained_quantized_frame_count() const noexcept;
    std::size_t retained_quantized_frame_bytes() const noexcept;
    ServerFrameConsumerSubscription subscribe_frame_consumer(ServerFrameConsumer& consumer);

    bool tick(ecs::Registry& registry, double dt_seconds);
    // Current server simulation frame; remains 0 until the first tick begins.
    SyncFrame frame() const noexcept {
        return frame_;
    }
    double accumulator_seconds() const noexcept {
        return tick_accumulator_seconds_;
    }
    double continuous_frame() const noexcept {
        return static_cast<double>(frame_) + tick_accumulator_seconds_ / options_.fixed_dt_seconds;
    }

    // Implementation-facing API for server-side frame consumers. These functions are public so
    // internal consumers do not depend on ReplicationServer's private storage layout.
    std::size_t replicated_slot_count() const noexcept;
    std::size_t active_replicated_slot_count() const noexcept;
    bool replicated_slot_active(std::uint32_t slot) const noexcept;
    ecs::Entity replicated_slot_entity(std::uint32_t slot) const noexcept;
    SyncArchetypeId replicated_slot_archetype(std::uint32_t slot) const noexcept;
    bool replicated_slot_is_replicable(const ecs::Registry& registry, std::uint32_t slot) const;
    void deactivate_replicated_slot(std::uint32_t slot);
    std::uint32_t replicated_slot_for_entity(ecs::Entity entity) const noexcept;
    std::uint32_t replicated_slot_for_entity_index(std::uint32_t entity_index) const noexcept;
    std::uint32_t quantized_frame_for_client(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        const server_detail::ServerClientReplicator& client,
        std::uint32_t slot,
        SyncFrame frame,
        QuantizedFrameData& scratch,
        std::vector<std::uint64_t>& scratch_dirty_generations);
    bool quantized_frame_active(std::uint32_t quantized_frame) const noexcept;
    SyncFrame quantized_frame_frame(std::uint32_t quantized_frame) const noexcept;
    SyncArchetypeId quantized_frame_archetype(std::uint32_t quantized_frame) const noexcept;
    const QuantizedFrameData* quantized_frame_data(std::uint32_t quantized_frame) const noexcept;
    void retain_server_quantized_frame(std::uint32_t quantized_frame);
    void release_server_quantized_frame(std::uint32_t quantized_frame);
    void clear_server_client_entity_state(server_detail::ClientEntityState& state);
    void acknowledge_server_cues(server_detail::ClientEntityState& state, SyncFrame frame);
    void send_server_update_packet(
        server_detail::ServerClientReplicator& client,
        SyncFrame frame,
        std::uint16_t entity_count,
        const ecs::BitBuffer& records,
        const std::vector<server_detail::PacketAckRecord>& ack_records);
    bool prepare_client_update_send(server_detail::ServerClientReplicator& client);
    std::size_t begin_client_bandwidth_tick(server_detail::ServerClientReplicator& client);
    std::size_t charged_packet_bytes(std::size_t payload_bytes) const noexcept;
#ifdef KAGE_SYNC_ENABLE_TRACING
    SyncTracer* server_tracer() const noexcept;
    void trace_entity_started_syncing(
        ClientId client,
        std::uint32_t slot,
        std::uint32_t network_id,
        std::uint32_t network_version);
    void trace_entity_destroyed(
        ClientId client,
        ecs::Entity entity,
        std::uint32_t network_id,
        std::uint32_t network_version);
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    void append_server_packet_ack_cues(
        const SyncSettings& settings,
        const server_detail::ClientEntityState& state,
        server_detail::PacketAckRecord& record) const;
#endif
#endif

private:
    friend class FractionalTickSampler;

    static constexpr std::uint32_t invalid_quantized_frame_id = std::numeric_limits<std::uint32_t>::max();

    struct ReplicatedSlot;
    struct QuantizedFrame;
    using ClientDirtyQueue = server_detail::ClientDirtyQueue;
    using ClientEntityState = server_detail::ClientEntityState;
    using PacketAckRecord = server_detail::PacketAckRecord;
    using ServerClientReplicator = server_detail::ServerClientReplicator;
    using SerializedEntity = server_detail::SerializedEntity;

    struct ClientState;
    struct PendingInboundPacket;

    using EntityKey = std::uint64_t;
    using ReplicatedSlotIndex = std::uint32_t;

    bool valid_archetype(const ecs::Registry& registry, SyncArchetypeId archetype) const;
    bool upsert_replicated(ecs::Registry& registry, ecs::Entity entity, SyncArchetypeId archetype);
    ReplicatedSlotIndex allocate_replicated_slot(ecs::Entity entity, SyncArchetypeId archetype);
    void deactivate_replicated(ReplicatedSlotIndex replicated_index);
    void deactivate_entity_index(std::uint32_t entity_index);
    void remove_replicated_from_client_replicators(ReplicatedSlotIndex replicated_index);
    bool replicated_is_replicable(const ecs::Registry& registry, ReplicatedSlotIndex replicated_index) const;
    void push_dirty_info_to_listeners(ecs::Registry& registry);
    void push_frame_to_listeners(ecs::Registry& registry, double dt_seconds, std::uint32_t completed_frames);
    void remove_unsubscribed_frame_consumers();
    void rediscover_replicated_entities(ecs::Registry& registry, ecs::Registry::DirtyView dirty);
    void capture_dirty_generations(ecs::Registry::DirtyView dirty, const SyncSettings& settings);
    void capture_queued_cues(ecs::Registry& registry, const SyncSettings& settings);
    bool play_local_cue(ecs::Registry& registry, const SyncSettings& settings, const QueuedSyncCue& cue);
    void attach_cue_to_clients(const ecs::Registry& registry, const SyncSettings& settings, std::uint32_t slot, const QueuedSyncCue& cue);
    void mark_dirty_component(const SyncSettings& settings, std::uint32_t slot, ecs::Entity component);
    void mark_dirty_tag(const SyncSettings& settings, std::uint32_t slot, ecs::Entity tag);
    void mark_dirty_tags(const SyncSettings& settings, std::uint32_t slot);
    void mark_owner_visibility_dirty(const SyncSettings& settings, std::uint32_t slot);
    static bool archetype_is_same_frame_cacheable(const SyncArchetype& archetype);
    void advance_client_idle_timers(double dt_seconds);
    void resend_pending_connect_responses(double dt_seconds);
    void disconnect_timed_out_clients();
    std::uint32_t find_or_create_quantized_frame(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        const ServerClientReplicator& client,
        std::uint32_t slot,
        SyncFrame frame,
        QuantizedFrameData& scratch,
        std::vector<std::uint64_t>& scratch_dirty_generations);
    void retain_quantized_frame(std::uint32_t quantized_frame);
    void release_quantized_frame(std::uint32_t quantized_frame);
    void clear_client_entity_state(ClientEntityState& state);
    struct ClientUpdateAckResult {
        bool packet_valid = false;
        bool all_acknowledged = false;
    };
    bool process_connect_request_packet(ClientId peer, ecs::BitBuffer& packet);
    bool process_message_from_connected_client(
        ecs::Registry& registry,
        ClientState& client,
        std::uint8_t message,
        ecs::BitBuffer& packet);
    bool process_connection_request_ack_packet(ClientState& client, ecs::BitBuffer& packet);
    bool process_ping_packet(ClientState& client, ecs::BitBuffer& packet);
    bool process_client_ack_packet(ServerClientReplicator& replication, ecs::BitBuffer& packet);
    ClientUpdateAckResult process_client_acks_from_packet(
        ServerClientReplicator& replication,
        ecs::BitBuffer& packet
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
        ,
        std::vector<std::uint32_t>& trace_acks
#endif
    );
    bool process_input_with_acks_packet(ecs::Registry& registry, ClientState& client, ecs::BitBuffer& packet);
    void log_info(const char* event, const std::string& fields) const;
    void log_client_packet_warning(ClientId peer, std::uint8_t message, const char* reason_code, const char* reason_detail);
    void log_server_error(ClientId peer, const char* event, const char* reason);
    void push_client_inputs_to_ecs(ecs::Registry& registry);
    void acknowledge_cues(ClientEntityState& state, SyncFrame frame);
    bool same_quantized_frame_components(
        const QuantizedFrame& quantized_frame,
        const QuantizedFrameData& data,
        const std::vector<std::uint64_t>& dirty_generations) const;
    void send_packet(
        ServerClientReplicator& client,
        SyncFrame frame,
        std::uint16_t entity_count,
        const ecs::BitBuffer& records,
        const std::vector<PacketAckRecord>& ack_records);
    bool add_client_for_peer(ClientId peer, ClientId client, bool ready_for_updates);
    bool add_client_state(ClientState state);
    void create_client_replicator(ClientState& client);
    void send_connect_response(ClientState& client);
    void send_pong(
        ClientId peer,
        std::uint32_t sequence,
        SyncFrame send_frame,
        std::uint16_t send_subframe);
#ifdef KAGE_SYNC_ENABLE_TRACING
    void trace_frame_components(const ecs::Registry& registry, const SyncSettings& settings);
    void trace_input_component(
        ecs::Entity entity,
        ClientState& client,
        SyncFrame frame,
        ecs::Entity component,
        const SyncComponentOps& ops,
        const std::uint8_t* quantized);
    void trace_input_starved(
        ecs::Entity entity,
        ClientState& client,
        SyncFrame due_frame,
        SyncFrame input_frame,
        ecs::Entity component,
        const SyncComponentOps& ops);
#ifdef KAGE_SYNC_TRACE_PACKET_LOGS
    void trace_incoming_ack_packet(ServerClientReplicator& client, const std::vector<std::uint32_t>& acks) const;
    void trace_incoming_input_packet(
        ClientState& client,
        const std::vector<std::uint32_t>& acks,
        SyncFrame baseline_frame,
        SyncFrame first_input_frame,
        SyncFrame last_input_frame) const;
    void trace_outgoing_update_packet(
        ServerClientReplicator& client,
        SyncFrame frame,
        std::uint32_t packet_id,
        SyncFrame input_ack_frame,
        const std::vector<PacketAckRecord>& records) const;
    void append_packet_ack_cues(
        const SyncSettings& settings,
        const ClientEntityState& state,
        PacketAckRecord& record) const;
#endif
#endif

    ReplicationServerOptions options_;
    std::vector<ReplicatedSlot> replicated_;
    std::vector<std::uint32_t> free_replicated_indices_;
    std::vector<QuantizedFrame> quantized_frames_;
    std::vector<std::uint32_t> free_quantized_frames_;
    std::unordered_map<EntityKey, std::uint32_t> entity_to_replicated_index_;
    std::unordered_map<std::uint32_t, std::uint32_t> entity_index_to_replicated_index_;
    std::vector<ClientState> clients_;
    std::unordered_map<ClientId, std::size_t> client_to_index_;
    std::unordered_map<ClientId, std::size_t> peer_to_index_;
    std::shared_ptr<ServerFrameConsumerSubscription::State> frame_consumers_;
    std::vector<PendingInboundPacket> inbound_packets_;
    std::vector<QueuedSyncCue> post_tick_cues_;
    std::shared_ptr<spdlog::logger> logger_;
    ObservabilityStats observability_stats_;
    std::unordered_map<ClientId, std::uint32_t> warning_logs_by_peer_;
    mutable bool processing_client_packet_ = false;
    mutable bool server_error_logged_ = false;
    std::size_t active_replicated_count_ = 0;
    ClientId next_connect_client_id_ = 1;
    ClientId local_client_ = invalid_client_id;
    SyncFrame frame_ = 0;
    double tick_accumulator_seconds_ = 0.0;
    bool replicated_initialized_ = false;
#ifdef KAGE_SYNC_ENABLE_TRACING
    SyncTracer* tracer_ = nullptr;
    std::unique_ptr<KTraceDirectoryWriter> trace_writer_;
#endif
};

template <std::size_t NetworkEntityIdTier0Bits = protocol::default_network_entity_id_tier0_bits>
class ReplicationServerT : public ReplicationServer {
    static_assert(
        protocol::valid_network_entity_id_tier0_bits(NetworkEntityIdTier0Bits),
        "NetworkEntityIdTier0Bits must be in [1, 22]");

public:
    static constexpr std::size_t network_entity_id_tier0_bits = NetworkEntityIdTier0Bits;

    explicit ReplicationServerT(ReplicationServerOptions options = {})
        : ReplicationServer(configure(std::move(options))) {}

private:
    static ReplicationServerOptions configure(ReplicationServerOptions options) {
        options.protocol.network_entity_id_tier0_bits = NetworkEntityIdTier0Bits;
        return options;
    }
};

}  // namespace kage::sync
