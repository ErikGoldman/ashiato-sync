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
    std::size_t retained_quantized_frame_count() const noexcept;
    std::size_t retained_quantized_frame_bytes() const noexcept;

    void tick(ecs::Registry& registry);
    void tick(ecs::Registry& registry, const ReplicateFn& replicate);

private:
    static constexpr std::uint32_t invalid_quantized_frame_id = std::numeric_limits<std::uint32_t>::max();

    struct ReplicatedSlot {
        ecs::Entity entity;
        SyncArchetypeId archetype;
        std::vector<std::uint32_t> quantized_frames;
        std::vector<std::uint64_t> component_dirty_generations;
        std::uint32_t same_frame_quantized_frame = invalid_quantized_frame_id;
        SyncFrame same_frame_quantized_frame_frame = 0;
        bool same_frame_cacheable = false;
        bool active = false;
    };

    struct QuantizedFrame {
        std::uint32_t slot = 0;
        SyncFrame frame = 0;
        SyncArchetypeId archetype;
        std::uint32_t ref_count = 0;
        bool active = false;
        QuantizedFrameData data;
        std::vector<std::uint64_t> dirty_generations;
    };

    struct ClientEntityState {
        struct PendingQuantizedFrame {
            std::uint32_t quantized_frame = invalid_quantized_frame_id;
            SyncFrame frame = 0;
        };

        std::uint32_t baseline = invalid_quantized_frame_id;
        std::vector<PendingQuantizedFrame> pending;
    };

    struct ClientDestroyState {
        ecs::Entity entity;
        SyncFrame frame = 0;
        std::uint64_t reset_epoch = 0;
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
        std::uint32_t quantized_frame = invalid_quantized_frame_id;
        BitBuffer payload;
    };

    struct SerializedCandidate {
        enum class Kind {
            Update,
            Destroy
        };

        Kind kind = Kind::Update;
        std::uint32_t slot = 0;
        std::size_t destroy_index = 0;
        std::uint64_t priority = 0;
    };

    bool valid_archetype(const ecs::Registry& registry, SyncArchetypeId archetype) const;
    bool upsert_replicated(ecs::Registry& registry, ecs::Entity entity, SyncArchetypeId archetype);
    std::uint32_t allocate_slot(ecs::Entity entity, SyncArchetypeId archetype);
    void deactivate_slot(std::uint32_t slot);
    void deactivate_entity_index(std::uint32_t entity_index);
    void remove_slot_from_client_orders(std::uint32_t slot);
    bool slot_is_replicable(const ecs::Registry& registry, std::uint32_t slot) const;
    void capture_dirty_components(const ecs::Registry& registry, const SyncSettings& settings);
    void mark_dirty_component(const SyncSettings& settings, std::uint32_t slot, ecs::Entity component);
    void mark_dirty_tag(const SyncSettings& settings, std::uint32_t slot, ecs::Entity tag);
    void mark_dirty_tags(const SyncSettings& settings, std::uint32_t slot);
    void mark_owner_visibility_dirty(const SyncSettings& settings, std::uint32_t slot);
    static bool archetype_is_same_frame_cacheable(const SyncArchetype& archetype);
    void tick_serialized(ecs::Registry& registry);
    void tick_serialized_parallel(ecs::Registry& registry);
    bool serialize_entity(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        ClientState& client,
        std::uint32_t slot,
        SyncFrame frame,
        QuantizedFrameData& scratch,
        std::vector<std::uint64_t>& scratch_dirty_generations,
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
    bool acknowledge_destroy(ClientState& client, ecs::Entity entity, SyncFrame frame);
    bool same_quantized_frame_components(
        const QuantizedFrame& quantized_frame,
        const QuantizedFrameData& data,
        const std::vector<std::uint64_t>& dirty_generations) const;
    void write_entity_record(
        const ecs::Registry& registry,
        const SyncSettings& settings,
        const ClientState& client,
        std::uint32_t slot,
        const QuantizedFrame& quantized_frame,
        BitBuffer& out) const;
    void send_packet(ClientId client, SyncFrame frame, std::uint16_t entity_count, const BitBuffer& records) const;

    ReplicationServerOptions options_;
    std::vector<ReplicatedSlot> replicated_;
    std::vector<std::uint32_t> free_replicated_slots_;
    std::vector<QuantizedFrame> quantized_frames_;
    std::vector<std::uint32_t> free_quantized_frames_;
    std::unordered_map<EntityKey, std::uint32_t> entity_to_slot_;
    std::unordered_map<std::uint32_t, std::uint32_t> entity_index_to_slot_;
    std::vector<ClientState> clients_;
    std::unordered_map<ClientId, std::size_t> client_to_index_;
    std::size_t active_replicated_count_ = 0;
    SyncFrame frame_ = 0;
    bool replicated_initialized_ = false;
};

}  // namespace kage::sync
