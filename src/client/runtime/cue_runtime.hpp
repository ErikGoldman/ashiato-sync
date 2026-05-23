#pragma once

#include "client/state.hpp"
#include "client/store/cue_store.hpp"

#include "ashiato/ashiato.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ashiato::sync {

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
    void clear_current_packet_cue_summaries();
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_PACKET_LOGS)
    const std::vector<std::string>& current_packet_cue_summaries() const noexcept;
#endif
    bool play(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        std::uint32_t entity_index,
        EntityState& state,
        const EntityCue& cue,
        float late_seconds,
        bool confirmed);
    bool rollback_played(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const EntityPlayedCue& cue,
        const char* rollback_reason);
    void play_snap(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        EntityState& state,
        const std::vector<EntityCue>& cues);
    void reconcile_authoritative_predicted(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        std::uint32_t entity_index,
        EntityState& state,
        const std::vector<EntityCue>& cues,
        SyncFrame frame);
    void drain_emitted_prediction(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        SyncFrame frame,
        bool play);
    bool finish_resimulation(ReplicationClient& client, ashiato::Registry& registry, const SyncSettings& settings);
    void play_buffered_for_frame(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        std::uint32_t entity_index,
        EntityState& state,
        SyncFrame buffered_frame);
    void discard_applied_buffered(ReplicationClient& client, SyncFrame buffered_frame);
    void store_authoritative_buffered(
        ReplicationClient& client,
        ashiato::Registry& registry,
        const SyncSettings& settings,
        std::uint32_t entity_index,
        EntityState& state,
        SyncFrame frame,
        const std::vector<EntityCue>& cues);

private:
    EntityReferenceContext make_reference_context(ReplicationClient& client) const;
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
}  // namespace ashiato::sync
