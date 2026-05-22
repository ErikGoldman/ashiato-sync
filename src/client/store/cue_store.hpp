#pragma once

#include "client/state.hpp"

#include "ashiato/sync/components.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ashiato::sync::client_detail {

class ClientFrameRingStore;

struct BufferedEntityCue {
    std::uint32_t entity_index = std::numeric_limits<std::uint32_t>::max();
    EntityCue cue;
};

class ClientCueStore {
public:
    EntityPlayedCue* find_played(
        ashiato::Registry& registry,
        const SyncSettings& settings,
        std::uint32_t entity_index,
        const EntityCue& cue,
        EntityReferenceContext* references);
    const EntityCue* find_authoritative(
        ashiato::Registry& registry,
        const SyncSettings& settings,
        const std::vector<EntityCue>& cues,
        const EntityPlayedCue& played) const;
    void mark_seen_in_resim(EntityPlayedCue& cue) const noexcept;
    void prune_confirmed(SyncFrame server_frame);
    void store_buffered(
        const ClientFrameRingStore& buffered_frames,
        std::uint32_t entity_index,
        SyncFrame frame,
        const std::vector<EntityCue>& cues,
        bool has_applied_buffered_frame,
        SyncFrame last_applied_buffered_frame);
    void begin_resimulation() noexcept;
    void erase_buffered_for_entity(std::uint32_t entity_index);
    void erase_for_entity(std::uint32_t entity_index);

    std::vector<EntityCue> received_scratch;
    std::vector<EntityPlayedCue> played;
    std::vector<BufferedEntityCue> buffered;
    std::uint32_t resim_generation = 1;
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_PACKET_LOGS)
    std::vector<std::string> current_packet_cue_summaries;
#endif
};

}  // namespace ashiato::sync::client_detail
