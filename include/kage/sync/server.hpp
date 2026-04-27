#pragma once

#include "kage/sync/component_traits.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
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

    void tick(ecs::Registry& registry);
    void tick(ecs::Registry& registry, const ReplicateFn& replicate);

private:
    struct ReplicatedSlot {
        ecs::Entity entity;
        SyncArchetypeId archetype;
        bool active = false;
    };

    struct QuantizedBaseline {
        ecs::Entity component;
        SyncComponentOps::QuantizedBytes bytes;
    };

    struct ClientState {
        ClientId id = invalid_client_id;
        std::uint64_t epoch = 0;
        std::vector<std::uint32_t> order;
        std::vector<std::uint64_t> reset_epochs;
        std::vector<std::vector<QuantizedBaseline>> baselines;
    };

    using EntityKey = std::uint64_t;

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
        const ClientState& client,
        std::uint32_t slot,
        BitBuffer& out,
        std::vector<QuantizedBaseline>& current) const;

    ReplicationServerOptions options_;
    std::vector<ReplicatedSlot> replicated_;
    std::vector<std::uint32_t> free_replicated_slots_;
    std::unordered_map<EntityKey, std::uint32_t> entity_to_slot_;
    std::unordered_map<std::uint32_t, std::uint32_t> entity_index_to_slot_;
    std::vector<ClientState> clients_;
    std::unordered_map<ClientId, std::size_t> client_to_index_;
    std::size_t active_replicated_count_ = 0;
    bool replicated_initialized_ = false;
};

}  // namespace kage::sync
