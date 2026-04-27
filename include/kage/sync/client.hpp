#pragma once

#include "kage/sync/component_traits.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace kage::sync {

struct ReplicationClientOptions {
    std::size_t mtu_bytes = 1200;
    ReplicationClientMode client_mode = ReplicationClientMode::Snap;
    SyncFrame interpolation_buffer_frames = 2;
    std::size_t interpolation_buffer_capacity_frames = 64;
};

class ReplicationClient {
public:
    explicit ReplicationClient(ReplicationClientOptions options = {});

    const ReplicationClientOptions& options() const noexcept {
        return options_;
    }

    void set_client_mode(ReplicationClientMode mode) noexcept;
    bool set_interpolation_buffer_frames(SyncFrame frames) noexcept;
    bool receive(ecs::Registry& registry, BitBuffer packet);
    bool apply_frame(ecs::Registry& registry, SyncFrame client_frame);
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
        ReplicationClientMode mode = ReplicationClientMode::Snap;
        SyncFrame frame = 0;
        std::vector<ComponentBaseline> baselines;
        std::vector<FrameBaseline> history;

        struct BufferedFrame {
            SyncFrame frame = 0;
            bool valid = false;
            bool entity_present = false;
            SyncArchetypeId archetype;
            std::vector<ComponentBaseline> baselines;
        };

        std::vector<BufferedFrame> buffered_frames;
        std::vector<ComponentBaseline> applied_baselines;
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
    bool apply_buffered_upsert(
        ecs::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        ecs::Entity server_entity,
        SyncArchetypeId archetype,
        std::vector<ComponentBaseline>& decoded);
    bool apply_buffered_destroy(ecs::Registry& registry, SyncFrame frame, ecs::Entity server_entity);
    bool validate_buffered_archetype(const SyncSettings& settings, SyncArchetypeId archetype) const;
    bool fill_buffered_frames(
        const SyncSettings& settings,
        EntityState& state,
        SyncFrame frame,
        bool entity_present,
        std::vector<ComponentBaseline>& decoded);
    bool write_buffered_frame(
        const SyncSettings& settings,
        EntityState& state,
        SyncFrame frame,
        bool entity_present,
        const std::vector<ComponentBaseline>* from,
        const std::vector<ComponentBaseline>* to,
        SyncFrame from_frame,
        SyncFrame to_frame);
    bool apply_buffered_sample(
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const EntityState::BufferedFrame& sample);
    ComponentInterpolation interpolation_for(
        const SyncSettings& settings,
        SyncArchetypeId archetype,
        ecs::Entity component) const;
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
