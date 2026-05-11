#pragma once

#include "client/state.hpp"
#include "client/store/cue_store.hpp"

#include "ecs/ecs.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace kage::sync {

class ReplicationClient;

namespace client_detail {

class ClientCueRuntime {
public:
    ClientCueStore& store() noexcept {
        return store_;
    }

    const ClientCueStore& store() const noexcept {
        return store_;
    }

    void erase_for_entity(std::uint32_t entity_index);
    void erase_buffered_for_entity(std::uint32_t entity_index);
    void prune_confirmed(SyncFrame server_frame);
    void clear_current_packet_cue_summaries();
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_PACKET_LOGS)
    const std::vector<std::string>& current_packet_cue_summaries() const noexcept;
#endif
    bool play(
        ReplicationClient& client,
        ecs::Registry& registry,
        const SyncSettings& settings,
        std::uint32_t entity_index,
        EntityState& state,
        const EntityCue& cue,
        float late_seconds,
        bool confirmed);
    bool rollback_played(
        ReplicationClient& client,
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const EntityPlayedCue& cue,
        const char* rollback_reason);
    void play_snap(
        ReplicationClient& client,
        ecs::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const std::vector<EntityCue>& cues);
    void reconcile_authoritative_predicted(
        ReplicationClient& client,
        ecs::Registry& registry,
        const SyncSettings& settings,
        std::uint32_t entity_index,
        EntityState& state,
        const std::vector<EntityCue>& cues,
        SyncFrame frame);
    void drain_emitted_prediction(
        ReplicationClient& client,
        ecs::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        bool play);
    bool finish_resimulation(ReplicationClient& client, ecs::Registry& registry, const SyncSettings& settings);
    void play_buffered_for_frame(
        ReplicationClient& client,
        ecs::Registry& registry,
        const SyncSettings& settings,
        std::uint32_t entity_index,
        EntityState& state,
        SyncFrame buffered_frame);
    void discard_applied_buffered(ReplicationClient& client, SyncFrame buffered_frame);
    void store_authoritative_buffered(
        ReplicationClient& client,
        ecs::Registry& registry,
        const SyncSettings& settings,
        std::uint32_t entity_index,
        EntityState& state,
        SyncFrame frame,
        const std::vector<EntityCue>& cues);

private:
    EntityReferenceContext make_reference_context(ReplicationClient& client) const;
    SyncFrame expire_frame(const ReplicationClient& client, const EntityCue& cue) const noexcept;
    void confirm_played(
        ReplicationClient& client,
        const SyncSettings& settings,
        EntityState& state,
        EntityPlayedCue& played,
        const EntityCue& cue,
        const char* cue_source);

    ClientCueStore store_;
};

}  // namespace client_detail
}  // namespace kage::sync
