#include "client/store/cue_store.hpp"

#include "client/store/frame_ring_store.hpp"

#include <algorithm>
#include <limits>

namespace ashiato::sync::client_detail {

EntityPlayedCue* ClientCueStore::find_played(
    ashiato::Registry& registry,
    const SyncSettings& settings,
    std::uint32_t entity_index,
    const EntityCue& cue,
    EntityReferenceContext* references) {
    (void)registry;
    if (cue.type >= settings.cue_ops.size() || settings.cue_ops[cue.type].equals == nullptr) {
        return nullptr;
    }
    const auto found = std::find_if(
        played.begin(),
        played.end(),
        [&](const EntityPlayedCue& candidate) {
            (void)references;
            return candidate.entity_index == entity_index &&
                candidate.frame == cue.frame &&
                candidate.type == cue.type &&
                settings.cue_ops[cue.type].equals(
                    cue.type,
                    settings.cue_ops[cue.type].user_data,
                    candidate.value.data(),
                    cue.value.data());
        });
    return found == played.end() ? nullptr : &*found;
}

const EntityCue* ClientCueStore::find_authoritative(
    ashiato::Registry& registry,
    const SyncSettings& settings,
    const std::vector<EntityCue>& cues,
    const EntityPlayedCue& played_cue) const {
    (void)registry;
    if (played_cue.type >= settings.cue_ops.size() || settings.cue_ops[played_cue.type].equals == nullptr) {
        return nullptr;
    }
    const auto found = std::find_if(
        cues.begin(),
        cues.end(),
        [&](const EntityCue& cue) {
            return cue.frame == played_cue.frame &&
                cue.type == played_cue.type &&
                settings.cue_ops[cue.type].equals(
                    cue.type,
                    settings.cue_ops[cue.type].user_data,
                    played_cue.value.data(),
                    cue.value.data());
        });
    return found == cues.end() ? nullptr : &*found;
}

void ClientCueStore::mark_seen_in_resim(EntityPlayedCue& cue) const noexcept {
    cue.seen_resim_generation = resim_generation;
}

void ClientCueStore::store_buffered(
    const ClientFrameRingStore& buffered_frames,
    std::uint32_t entity_index,
    SyncFrame frame,
    const std::vector<EntityCue>& incoming_cues,
    bool has_applied_buffered_frame,
    SyncFrame last_applied_buffered_frame) {
    if (buffered_frames.empty(entity_index) || incoming_cues.empty() ||
        !buffered_frames.contains(entity_index, frame)) {
        return;
    }
    buffered.reserve(buffered.size() + incoming_cues.size());
    for (const EntityCue& cue : incoming_cues) {
        if (has_applied_buffered_frame && cue.frame <= last_applied_buffered_frame) {
            continue;
        }
        buffered.push_back(BufferedEntityCue{entity_index, cue});
    }
}

void ClientCueStore::begin_resimulation() noexcept {
    if (resim_generation == std::numeric_limits<std::uint32_t>::max()) {
        resim_generation = 1;
        for (EntityPlayedCue& cue : played) {
            cue.seen_resim_generation = 0;
        }
        return;
    }
    ++resim_generation;
}

void ClientCueStore::erase_buffered_for_entity(std::uint32_t entity_index) {
    buffered.erase(
        std::remove_if(
            buffered.begin(),
            buffered.end(),
            [entity_index](const BufferedEntityCue& cue) {
                return cue.entity_index == entity_index;
            }),
        buffered.end());
}

void ClientCueStore::erase_for_entity(std::uint32_t entity_index) {
    played.erase(
        std::remove_if(
            played.begin(),
            played.end(),
            [entity_index](const EntityPlayedCue& cue) {
                return cue.entity_index == entity_index;
            }),
        played.end());
    erase_buffered_for_entity(entity_index);
}

}  // namespace ashiato::sync::client_detail
