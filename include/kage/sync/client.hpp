#pragma once

#include "kage/sync/component_traits.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace kage::sync {

struct ReplicationClientOptions {
    std::size_t mtu_bytes = 1200;
};

class ReplicationClient {
public:
    explicit ReplicationClient(ReplicationClientOptions options = {});

    const ReplicationClientOptions& options() const noexcept {
        return options_;
    }

    bool receive(ecs::Registry& registry, BitBuffer packet);
    std::vector<BitBuffer> drain_ack_packets();
    std::size_t pending_ack_count() const noexcept;
    ecs::Entity local_entity(ecs::Entity server_entity) const;

private:
    struct ComponentBaseline {
        ecs::Entity component;
        SyncComponentOps::QuantizedBytes bytes;
    };

    struct EntityState {
        struct FrameBaseline {
            SyncFrame frame = 0;
            std::vector<ComponentBaseline> baselines;
        };

        ecs::Entity local;
        SyncArchetypeId archetype;
        SyncFrame frame = 0;
        std::vector<ComponentBaseline> baselines;
        std::vector<FrameBaseline> history;
    };

    struct AckRecord {
        ecs::Entity entity;
        SyncFrame frame = 0;
        bool destroy = false;
    };

    bool apply_update(ecs::Registry& registry, BitBuffer& packet);
    bool apply_upsert(
        ecs::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        ecs::Entity server_entity,
        BitBuffer& packet);
    bool apply_destroy(ecs::Registry& registry, SyncFrame frame, ecs::Entity server_entity);
    const SyncComponentOps::QuantizedBytes* baseline_for(
        const std::vector<ComponentBaseline>& baselines,
        ecs::Entity component) const;
    void remember_baseline(EntityState& state);
    void queue_ack(ecs::Entity entity, SyncFrame frame, bool destroy);

    ReplicationClientOptions options_;
    std::unordered_map<std::uint64_t, EntityState> entities_;
    std::vector<AckRecord> pending_acks_;
};

}  // namespace kage::sync
