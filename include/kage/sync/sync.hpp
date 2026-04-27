#pragma once

#include "ecs/ecs.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace kage::sync {

using ClientId = std::uint64_t;

inline constexpr ClientId invalid_client_id = std::numeric_limits<ClientId>::max();

enum class SyncRole {
    Server,
    Client
};

struct SyncArchetypeId {
    std::uint32_t value = std::numeric_limits<std::uint32_t>::max();
};

inline constexpr SyncArchetypeId invalid_sync_archetype_id{};

constexpr bool operator==(SyncArchetypeId lhs, SyncArchetypeId rhs) noexcept {
    return lhs.value == rhs.value;
}

constexpr bool operator!=(SyncArchetypeId lhs, SyncArchetypeId rhs) noexcept {
    return !(lhs == rhs);
}

enum class ReplicationAudience {
    Owner,
    All
};

struct ComponentReplication {
    ecs::Entity component;
    ReplicationAudience audience = ReplicationAudience::All;
};

struct SyncArchetype {
    std::string name;
    std::vector<ComponentReplication> components;
};

struct SyncSettings {
    SyncRole role = SyncRole::Server;
    ClientId local_client = invalid_client_id;
    std::vector<SyncArchetype> archetypes;
};

struct Replicated {
    SyncArchetypeId archetype;
};

struct NetworkOwner {
    ClientId client = invalid_client_id;
};

struct ReplicationServerOptions {
    std::size_t bandwidth_limit_bytes_per_tick = 1024;
    std::size_t fixed_entity_replication_cost_bytes = 128;
};

class ReplicationServer {
public:
    using ReplicateFn = std::function<void(ClientId, ecs::Entity)>;

    explicit ReplicationServer(ReplicationServerOptions options = {});

    const ReplicationServerOptions& options() const noexcept {
        return options_;
    }

    bool add_client(ClientId client);
    bool remove_client(ClientId client);
    bool has_client(ClientId client) const;
    std::size_t client_count() const noexcept;

    bool add_replicated(ecs::Registry& registry, ecs::Entity entity, SyncArchetypeId archetype);
    bool remove_replicated(ecs::Registry& registry, ecs::Entity entity);
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

    struct ClientState {
        ClientId id = invalid_client_id;
        std::uint64_t epoch = 0;
        std::vector<std::uint32_t> order;
        std::vector<std::uint64_t> reset_epochs;
    };

    using EntityKey = std::uint64_t;

    std::uint32_t allocate_slot(ecs::Entity entity, SyncArchetypeId archetype);
    void deactivate_slot(std::uint32_t slot);
    void remove_slot_from_client_orders(std::uint32_t slot);
    bool slot_is_replicable(const ecs::Registry& registry, std::uint32_t slot) const;

    ReplicationServerOptions options_;
    std::vector<ReplicatedSlot> replicated_;
    std::vector<std::uint32_t> free_replicated_slots_;
    std::unordered_map<EntityKey, std::uint32_t> entity_to_slot_;
    std::vector<ClientState> clients_;
    std::unordered_map<ClientId, std::size_t> client_to_index_;
    std::size_t active_replicated_count_ = 0;
};

void register_components(ecs::Registry& registry);

void configure_server(ecs::Registry& registry);
void configure_client(ecs::Registry& registry, ClientId local_client);

SyncArchetypeId define_archetype(
    ecs::Registry& registry,
    std::string name,
    std::vector<ComponentReplication> components);

const SyncArchetype* find_archetype(const ecs::Registry& registry, SyncArchetypeId id);

bool mark_replicated(ecs::Registry& registry, ecs::Entity entity, SyncArchetypeId archetype);
bool unmark_replicated(ecs::Registry& registry, ecs::Entity entity);
bool set_owner(ecs::Registry& registry, ecs::Entity entity, ClientId client);

}  // namespace kage::sync

namespace ecs {

template <>
struct is_singleton_component<kage::sync::SyncSettings> : std::true_type {};

}  // namespace ecs
