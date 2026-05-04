#pragma once

#include "kage/sync/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kage::sync {

class SyncTracer;

class ReplicationServer {
public:
    using ReplicateFn = std::function<void(ClientId, ecs::Entity)>;

    struct ClientInputStats {
        std::uint64_t input_frames_applied = 0;
        std::uint64_t input_starvation_frames = 0;
        std::uint64_t input_reused_frames = 0;
        SyncFrame latest_received_input_frame = 0;
        SyncFrame latest_applied_input_frame = 0;
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
    void set_packet_sender(TransportFn sender);
#ifdef KAGE_SYNC_ENABLE_TRACING
    void set_tracer(SyncTracer* tracer) noexcept;
#endif

    bool add_client(ClientId client);
    bool remove_client(ClientId client);
    bool has_client(ClientId client) const;
    std::size_t client_count() const noexcept;

    void refresh_replicated(ecs::Registry& registry);
    bool is_replicated(ecs::Entity entity) const;
    std::size_t replicated_count() const noexcept;

    std::uint64_t priority(ClientId client, ecs::Entity entity) const;
    bool acknowledge_entity(ClientId client, ecs::Entity entity, SyncFrame frame);
    void receive_packet(ClientId client, BitBuffer packet);
    bool process_packet(ClientId client, BitBuffer packet);
    bool process_packet(ecs::Registry& registry, ClientId client, BitBuffer packet);
    ClientInputStats input_stats(ClientId client) const noexcept;
    std::size_t retained_quantized_frame_count() const noexcept;
    std::size_t retained_quantized_frame_bytes() const noexcept;

    void begin_tick(ecs::Registry& registry);
    void end_tick(ecs::Registry& registry);
    void end_tick(ecs::Registry& registry, const ReplicateFn& replicate);
    bool tick(ecs::Registry& registry, double dt_seconds);
    void tick(ecs::Registry& registry);
    void tick(ecs::Registry& registry, const ReplicateFn& replicate);
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

private:
    static constexpr std::uint32_t invalid_quantized_frame_id = std::numeric_limits<std::uint32_t>::max();

    struct ReplicatedSlot;
    struct QuantizedFrame;
    struct ClientEntityState;
    struct ClientDestroyState;
    struct PacketAckRecord;
    struct PendingPacketAck;
    struct ClientState;
    struct PendingInboundPacket;

    using EntityKey = std::uint64_t;

    struct SerializedEntity;
    struct SerializedCandidate;

    bool valid_archetype(const ecs::Registry& registry, SyncArchetypeId archetype) const;
    bool upsert_replicated(ecs::Registry& registry, ecs::Entity entity, SyncArchetypeId archetype);
    std::uint32_t allocate_slot(ecs::Entity entity, SyncArchetypeId archetype);
    void deactivate_slot(std::uint32_t slot);
    void deactivate_entity_index(std::uint32_t entity_index);
    void hide_slot_for_client(ClientState& client, std::uint32_t slot);
    void remove_slot_from_client_orders(std::uint32_t slot);
    std::uint32_t allocate_network_id(ClientState& client, std::uint32_t slot);
    void free_network_id(ClientState& client, std::uint32_t network_id);
    std::uint32_t network_id_for_slot(ClientState& client, std::uint32_t slot);
    bool slot_is_replicable(const ecs::Registry& registry, std::uint32_t slot) const;
    void capture_dirty_components(const ecs::Registry& registry, const SyncSettings& settings);
    void capture_queued_cues(const ecs::Registry& registry, const SyncSettings& settings);
    void attach_cue_to_clients(const ecs::Registry& registry, const SyncSettings& settings, std::uint32_t slot, const QueuedSyncCue& cue);
    void expire_pending_cues(ClientState& client, SyncFrame frame);
    void mark_dirty_component(const SyncSettings& settings, std::uint32_t slot, ecs::Entity component);
    void mark_dirty_tag(const SyncSettings& settings, std::uint32_t slot, ecs::Entity tag);
    void mark_dirty_tags(const SyncSettings& settings, std::uint32_t slot);
    void mark_owner_visibility_dirty(const SyncSettings& settings, std::uint32_t slot);
    static bool archetype_is_same_frame_cacheable(const SyncArchetype& archetype);
    void refresh_client_priorities(const ecs::Registry& registry, ClientState& client);
    void tick_serialized(ecs::Registry& registry);
    void tick_serialized_parallel(ecs::Registry& registry);
    void emit_post_tick(const ecs::Registry& registry);
    void disconnect_idle_clients();
    bool serialize_entity(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        ClientState& client,
        std::uint32_t slot,
        SyncFrame frame,
        QuantizedFrameData& scratch,
        std::vector<std::uint64_t>& scratch_dirty_generations,
        std::uint64_t component_mask,
        SerializedEntity& out);
    std::uint32_t find_or_create_quantized_frame(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        const ClientState& client,
        std::uint32_t slot,
        SyncFrame frame,
        QuantizedFrameData& scratch,
        std::vector<std::uint64_t>& scratch_dirty_generations);
    void retain_quantized_frame(std::uint32_t quantized_frame);
    void release_quantized_frame(std::uint32_t quantized_frame);
    void clear_client_entity_state(ClientEntityState& state);
    bool acknowledge_packet(ClientState& client, std::uint32_t packet_id);
    bool process_packet_impl(ecs::Registry* registry, ClientId client, BitBuffer packet);
    bool process_input_packet(ecs::Registry& registry, ClientState& client, BitBuffer& packet);
    void apply_client_inputs(ecs::Registry& registry);
    bool packet_ack_record_pending(const ClientState& client, const PacketAckRecord& record) const;
    void cleanup_packet_acks(ClientState& client);
    std::uint32_t allocate_packet_id(ClientState& client);
    void enforce_pending_packet_ack_limit(ClientState& client);
    bool acknowledge_destroy(ClientState& client, ecs::Entity entity, SyncFrame frame);
    void acknowledge_cues(ClientEntityState& state, SyncFrame frame);
    bool same_quantized_frame_components(
        const QuantizedFrame& quantized_frame,
        const QuantizedFrameData& data,
        const std::vector<std::uint64_t>& dirty_generations) const;
    void write_entity_record(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        ClientState& client,
        std::uint32_t slot,
        const QuantizedFrame& quantized_frame,
        std::uint64_t component_mask,
        BitBuffer& out);
    void send_packet(
        ClientState& client,
        SyncFrame frame,
        std::uint16_t entity_count,
        const BitBuffer& records,
        const std::vector<PacketAckRecord>& ack_records);
    bool add_client_for_peer(ClientId peer, ClientId client, bool ready_for_updates);
    void send_connect_response(ClientState& client);
    void send_pong(
        ClientId peer,
        std::uint32_t sequence,
        SyncFrame send_frame,
        std::uint16_t send_subframe);
    static void track_packet_ack(
        ClientState& client,
        std::uint32_t packet_id,
        const std::vector<PacketAckRecord>& records);
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
    void trace_incoming_ack_packet(ClientState& client, const std::vector<std::uint32_t>& acks) const;
    void trace_incoming_input_packet(
        ClientState& client,
        const std::vector<std::uint32_t>& acks,
        SyncFrame baseline_frame,
        SyncFrame first_input_frame,
        SyncFrame last_input_frame) const;
    void trace_outgoing_update_packet(
        ClientState& client,
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
    std::vector<std::uint32_t> free_replicated_slots_;
    std::vector<QuantizedFrame> quantized_frames_;
    std::vector<std::uint32_t> free_quantized_frames_;
    std::unordered_map<EntityKey, std::uint32_t> entity_to_slot_;
    std::unordered_map<std::uint32_t, std::uint32_t> entity_index_to_slot_;
    std::vector<ClientState> clients_;
    std::unordered_map<ClientId, std::size_t> client_to_index_;
    std::unordered_map<ClientId, std::size_t> peer_to_index_;
    std::vector<PendingInboundPacket> inbound_packets_;
    std::vector<QueuedSyncCue> post_tick_cues_;
    std::size_t active_replicated_count_ = 0;
    ClientId next_connect_client_id_ = 1;
    SyncFrame frame_ = 0;
    double tick_accumulator_seconds_ = 0.0;
    bool replicated_initialized_ = false;
#ifdef KAGE_SYNC_ENABLE_TRACING
    SyncTracer* tracer_ = nullptr;
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
