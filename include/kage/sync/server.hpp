#pragma once

#include "kage/sync/component_traits.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

namespace kage::sync {

class ReplicationServer {
public:
    using ReplicateFn = std::function<void(ClientId, ecs::Entity)>;

    explicit ReplicationServer(ReplicationServerOptions options = {});

    const ReplicationServerOptions& options() const noexcept {
        return options_;
    }

    void set_transport(TransportFn transport);

    bool add_client(ClientId client);
    bool remove_client(ClientId client);
    bool has_client(ClientId client) const;
    std::size_t client_count() const noexcept;

    void refresh_replicated(ecs::Registry& registry);
    bool is_replicated(ecs::Entity entity) const;
    std::size_t replicated_count() const noexcept;

    std::uint64_t priority(ClientId client, ecs::Entity entity) const;
    bool acknowledge_entity(ClientId client, ecs::Entity entity, SyncFrame frame);
    bool process_packet(ClientId client, BitBuffer packet);
    std::size_t retained_snapshot_count() const noexcept;
    std::size_t retained_snapshot_bytes() const noexcept;

    void tick(ecs::Registry& registry);
    void tick(ecs::Registry& registry, const ReplicateFn& replicate);

private:
    struct ReplicatedSlot {
        ecs::Entity entity;
        SyncArchetypeId archetype;
        std::vector<std::uint32_t> snapshots;
        bool active = false;
    };

    struct QuantizedBaseline {
        ecs::Entity component;
        SyncComponentOps::QuantizedBytes bytes;
    };

    static constexpr std::uint32_t invalid_snapshot_id = std::numeric_limits<std::uint32_t>::max();

    struct QuantizedSnapshot {
        std::uint32_t slot = 0;
        SyncFrame frame = 0;
        SyncArchetypeId archetype;
        std::uint32_t ref_count = 0;
        bool active = false;
        std::vector<QuantizedBaseline> baselines;
    };

    struct ClientEntityState {
        struct PendingSnapshot {
            std::uint32_t snapshot = invalid_snapshot_id;
            SyncFrame frame = 0;
        };

        std::uint32_t baseline = invalid_snapshot_id;
        std::vector<PendingSnapshot> pending;
    };

    struct ClientDestroyState {
        ecs::Entity entity;
        SyncFrame frame = 0;
    };

    struct ClientState {
        ClientId id = invalid_client_id;
        std::uint64_t epoch = 0;
        std::vector<std::uint32_t> order;
        std::vector<std::uint64_t> reset_epochs;
        std::vector<ClientEntityState> entity_states;
        std::vector<ClientDestroyState> pending_destroys;
    };

    using EntityKey = std::uint64_t;

    struct SerializedEntity {
        std::uint32_t snapshot = invalid_snapshot_id;
        BitBuffer payload;
    };

    bool valid_archetype(const ecs::Registry& registry, SyncArchetypeId archetype) const;
    bool upsert_replicated(ecs::Registry& registry, ecs::Entity entity, SyncArchetypeId archetype);
    std::uint32_t allocate_slot(ecs::Entity entity, SyncArchetypeId archetype);
    void deactivate_slot(std::uint32_t slot);
    void deactivate_entity_index(std::uint32_t entity_index);
    void remove_slot_from_client_orders(std::uint32_t slot);
    bool slot_is_replicable(const ecs::Registry& registry, std::uint32_t slot) const;
    void tick_serialized(ecs::Registry& registry);
    bool serialize_entity(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        ClientState& client,
        std::uint32_t slot,
        SyncFrame frame,
        std::vector<QuantizedBaseline>& scratch,
        SerializedEntity& out);
    std::uint32_t find_or_create_snapshot(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        ClientId client,
        std::uint32_t slot,
        SyncFrame frame,
        std::vector<QuantizedBaseline>& scratch);
    void retain_snapshot(std::uint32_t snapshot);
    void release_snapshot(std::uint32_t snapshot);
    void clear_client_entity_state(ClientEntityState& state);
    bool acknowledge_destroy(ClientState& client, ecs::Entity entity, SyncFrame frame);
    const SyncComponentOps::QuantizedBytes* find_baseline_component(
        std::uint32_t snapshot,
        ecs::Entity component) const;
    bool same_snapshot_components(
        const QuantizedSnapshot& snapshot,
        const std::vector<QuantizedBaseline>& baselines) const;
    void write_entity_record(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        const ClientState& client,
        std::uint32_t slot,
        const QuantizedSnapshot& snapshot,
        BitBuffer& out) const;
    void send_packet(ClientId client, SyncFrame frame, std::uint16_t entity_count, const BitBuffer& records) const;

    ReplicationServerOptions options_;
    std::vector<ReplicatedSlot> replicated_;
    std::vector<std::uint32_t> free_replicated_slots_;
    std::vector<QuantizedSnapshot> snapshots_;
    std::vector<std::uint32_t> free_snapshots_;
    std::unordered_map<EntityKey, std::uint32_t> entity_to_slot_;
    std::unordered_map<std::uint32_t, std::uint32_t> entity_index_to_slot_;
    std::vector<ClientState> clients_;
    std::unordered_map<ClientId, std::size_t> client_to_index_;
    std::size_t active_replicated_count_ = 0;
    SyncFrame frame_ = 0;
    bool replicated_initialized_ = false;
};

}  // namespace kage::sync
