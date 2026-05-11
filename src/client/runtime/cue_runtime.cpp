#include "client/runtime/cue_runtime.hpp"

#include "client/runtime/buffered_runtime.hpp"
#include "client/store/cue_store.hpp"
#include "client/store/entity_store.hpp"
#include "client/store/frame_ring_store.hpp"
#include "client/runtime/prediction_runtime.hpp"

#include "ashiato/sync/client.hpp"
#include "ashiato/sync/tracing.hpp"

#include <algorithm>
#include <cmath>

namespace ashiato::sync::client_detail {

void ClientCueRuntime::erase_for_entity(std::uint32_t entity_index) {
    store_.erase_for_entity(entity_index);
}

void ClientCueRuntime::erase_buffered_for_entity(std::uint32_t entity_index) {
    store_.erase_buffered_for_entity(entity_index);
}

void ClientCueRuntime::prune_confirmed(SyncFrame server_frame) {
    store_.prune_confirmed(server_frame);
}

void ClientCueRuntime::clear_current_packet_cue_summaries() {
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_PACKET_LOGS)
    store_.current_packet_cue_summaries.clear();
#endif
}

#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_PACKET_LOGS)
const std::vector<std::string>& ClientCueRuntime::current_packet_cue_summaries() const noexcept {
    return store_.current_packet_cue_summaries;
}
#endif

bool ClientCueRuntime::play(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    std::uint32_t entity_index,
    EntityState& state,
    const EntityCue& cue,
    float late_seconds,
    bool confirmed) {
    if (cue.type >= settings.cue_ops.size() || settings.cue_ops[cue.type].play == nullptr) {
        return false;
    }
    EntityReferenceContext reference_context = make_reference_context(client);
    EntityReferenceContext* references = settings.cue_ops[cue.type].references_entities ? &reference_context : nullptr;
    EntityPlayedCue* existing = store_.find_played(settings, entity_index, cue, references);
    if (existing != nullptr) {
        if (confirmed) {
            confirm_played(client, settings, state, *existing, cue, "server");
        }
        store_.mark_seen_in_resim(*existing);
        return true;
    }
    if (!state.identity.local || !registry.alive(state.identity.local)) {
        return false;
    }
    if (!settings.cue_ops[cue.type].play(registry, state.identity.local, cue.payload, late_seconds, cue.frame, references)) {
        return false;
    }
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    client.trace_cue_event(
        SyncTraceEventType::CuePlayed,
        settings,
        state,
        cue,
        nullptr,
        confirmed ? "server" : "local_prediction");
#endif
    store_.played.push_back(EntityPlayedCue{
        entity_index,
        cue.frame,
        confirmed ? expire_frame(client, cue) : 0U,
        cue.type,
        cue.payload,
        confirmed,
        store_.resim_generation});
    return true;
}

EntityReferenceContext ClientCueRuntime::make_reference_context(ReplicationClient& client) const {
    EntityReferenceContext reference_context;
    reference_context.userContext = &client;
    reference_context.network_entity_id_tier0_bits = client.options_.network.protocol.network_entity_id_tier0_bits;
    reference_context.client_entity_network_id_for_wire = [](void* userContext, std::uint32_t wire_network_id) {
        return static_cast<ReplicationClient*>(userContext)->client_entity_network_id_for_wire(wire_network_id);
    };
    reference_context.client_local_entity = [](void* userContext, ClientEntityNetworkId client_entity_network_id) {
        return static_cast<ReplicationClient*>(userContext)->local_entity(client_entity_network_id);
    };
    return reference_context;
}

SyncFrame ClientCueRuntime::expire_frame(const ReplicationClient& client, const EntityCue& cue) const noexcept {
    const SyncFrame relevance_frames = static_cast<SyncFrame>(
        std::ceil(static_cast<double>(cue.relevance_seconds) / client.fixed_dt_seconds_));
    return cue.frame + relevance_frames;
}

void ClientCueRuntime::confirm_played(
    ReplicationClient& client,
    const SyncSettings& settings,
    EntityState& state,
    EntityPlayedCue& played,
    const EntityCue& cue,
    const char* cue_source) {
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    const bool newly_confirmed = !played.confirmed;
#else
    (void)client;
    (void)settings;
    (void)state;
    (void)cue_source;
#endif
    played.confirmed = true;
    played.expire_frame = expire_frame(client, cue);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    if (newly_confirmed) {
        client.trace_cue_event(SyncTraceEventType::CueConfirmed, settings, state, played, nullptr, cue_source);
    }
#endif
}

bool ClientCueRuntime::rollback_played(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const EntityPlayedCue& cue,
    const char* rollback_reason) {
#ifndef ASHIATO_SYNC_ENABLE_TRACING
    (void)rollback_reason;
#endif
    if (cue.type >= settings.cue_ops.size() || settings.cue_ops[cue.type].rollback == nullptr) {
        return false;
    }
    if (!state.identity.local || !registry.alive(state.identity.local)) {
        return true;
    }
    EntityReferenceContext reference_context = make_reference_context(client);
    EntityReferenceContext* references = settings.cue_ops[cue.type].references_entities ? &reference_context : nullptr;
    const bool rolled_back = settings.cue_ops[cue.type].rollback(registry, state.identity.local, cue.payload, references);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    if (rolled_back) {
        client.trace_cue_event(SyncTraceEventType::CueRolledBack, settings, state, cue, rollback_reason, "local_prediction");
    }
#endif
    return rolled_back;
}

void ClientCueRuntime::play_snap(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const std::vector<EntityCue>& cues) {
    const std::uint32_t entity_index = client.entity_store_->index_of(state);
    for (const EntityCue& cue : cues) {
        const double server_frame = client.clock_.estimated_server_frame();
        const double late_frames = std::max(0.0, server_frame - static_cast<double>(cue.frame));
        (void)play(
            client,
            registry,
            settings,
            entity_index,
            state,
            cue,
            static_cast<float>(late_frames * client.fixed_dt_seconds_),
            true);
    }
}

void ClientCueRuntime::reconcile_authoritative_predicted(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    std::uint32_t entity_index,
    EntityState& state,
    const std::vector<EntityCue>& cues,
    SyncFrame frame) {
    for (const EntityCue& cue : cues) {
        EntityPlayedCue* found = store_.find_played(settings, entity_index, cue, nullptr);
        if (found != nullptr) {
            confirm_played(client, settings, state, *found, cue, "server");
            continue;
        }
        const double predicted_frame = client.clock_.continuous_predicted_frame();
        const double late_frames = std::max(0.0, predicted_frame - static_cast<double>(cue.frame));
        (void)play(
            client,
            registry,
            settings,
            entity_index,
            state,
            cue,
            static_cast<float>(late_frames * client.fixed_dt_seconds_),
            true);
    }

    for (auto played = store_.played.begin(); played != store_.played.end();) {
        if (played->entity_index != entity_index || played->confirmed || played->frame != frame) {
            ++played;
            continue;
        }
        const EntityCue* authoritative_match = store_.find_authoritative(settings, cues, *played);
        if (authoritative_match != nullptr) {
            confirm_played(client, settings, state, *played, *authoritative_match, "server");
            ++played;
            continue;
        }
        (void)rollback_played(client, registry, settings, state, *played, "server_mismatch");
        played = store_.played.erase(played);
    }
}

void ClientCueRuntime::drain_emitted_prediction(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    bool play_cues) {
    (void)frame;
    std::vector<QueuedSyncCue> cues = registry.write<CueDispatcher>().drain();

    for (const QueuedSyncCue& emitted : cues) {
        EntityState* state = client.find_entity_state_for_local(emitted.entity);
        if (state == nullptr || state->mode.current != ReplicationClientMode::Predict) {
            continue;
        }
        const std::uint32_t entity_index = client.entity_store_->index_of(*state);
        EntityCue cue;
        cue.frame = emitted.frame;
        cue.type = emitted.type;
        cue.relevance_seconds = emitted.relevance_seconds;
        cue.payload = emitted.payload;
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        client.trace_cue_event(SyncTraceEventType::CueEmitted, settings, *state, cue, nullptr, "local_prediction");
#endif

        EntityPlayedCue* found = store_.find_played(settings, entity_index, cue, nullptr);
        if (found != nullptr) {
            store_.mark_seen_in_resim(*found);
            continue;
        }
        if (!play_cues) {
            continue;
        }
        const SyncFrame late_frames = client.prediction_->late_frames_since(cue.frame);
        (void)play(
            client,
            registry,
            settings,
            entity_index,
            *state,
            cue,
            static_cast<float>(static_cast<double>(late_frames) * client.fixed_dt_seconds_),
            false);
    }
}

bool ClientCueRuntime::finish_resimulation(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings) {
    bool all_valid = true;
    for (auto cue = store_.played.begin(); cue != store_.played.end();) {
        if (cue->confirmed || cue->seen_resim_generation == store_.resim_generation) {
            ++cue;
            continue;
        }
        if (cue->entity_index >= client.entity_store_->entity_count()) {
            cue = store_.played.erase(cue);
            continue;
        }
        EntityState& state = client.entity_store_->state_unchecked(cue->entity_index);
        all_valid = rollback_played(client, registry, settings, state, *cue, "resim_not_replayed") && all_valid;
        cue = store_.played.erase(cue);
    }
    return all_valid;
}

void ClientCueRuntime::play_buffered_for_frame(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    std::uint32_t entity_index,
    EntityState& state,
    SyncFrame buffered_frame) {
    for (auto cue_record = store_.buffered.begin(); cue_record != store_.buffered.end();) {
        if (cue_record->entity_index != entity_index || cue_record->cue.frame != buffered_frame) {
            ++cue_record;
            continue;
        }
        const EntityCue& cue = cue_record->cue;
        const SyncFrame late_frames = buffered_frame > cue.frame ? buffered_frame - cue.frame : 0U;
        (void)play(
            client,
            registry,
            settings,
            entity_index,
            state,
            cue,
            static_cast<float>(static_cast<double>(late_frames) * client.fixed_dt_seconds_),
            true);
        cue_record = store_.buffered.erase(cue_record);
    }
}

void ClientCueRuntime::discard_applied_buffered(ReplicationClient& client, SyncFrame buffered_frame) {
    (void)client;
    store_.buffered.erase(
        std::remove_if(
            store_.buffered.begin(),
            store_.buffered.end(),
            [buffered_frame](const BufferedEntityCue& cue) {
                return cue.cue.frame <= buffered_frame;
            }),
        store_.buffered.end());
}

void ClientCueRuntime::store_authoritative_buffered(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    std::uint32_t entity_index,
    EntityState& state,
    SyncFrame frame,
    const std::vector<EntityCue>& cues) {
    store_.store_buffered(
        client.buffered_runtime_->frames(),
        entity_index,
        frame,
        cues,
        client.buffered_runtime_->has_applied_frame(),
        client.buffered_runtime_->last_applied_frame());
    if (!client.buffered_runtime_->has_applied_frame()) {
        return;
    }
    for (const EntityCue& cue : cues) {
        if (cue.frame > client.buffered_runtime_->last_applied_frame()) {
            continue;
        }
        const SyncFrame late_frames = client.buffered_runtime_->last_applied_frame() - cue.frame;
        (void)play(
            client,
            registry,
            settings,
            entity_index,
            state,
            cue,
            static_cast<float>(static_cast<double>(late_frames) * client.fixed_dt_seconds_),
            true);
    }
}

}  // namespace ashiato::sync::client_detail
