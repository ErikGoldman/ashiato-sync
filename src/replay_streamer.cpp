#include "kage/sync/replay_streamer.hpp"

#include "detail/frame_data.hpp"

#include "kage/sync/components.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace kage::sync {

struct ReplicationReplayStreamSessionAccess {
    static std::unordered_map<std::uint32_t, ecs::Entity>& entities(ReplicationReplayStreamSession& session) {
        return session.entities_;
    }

    static const std::unordered_map<std::uint32_t, ecs::Entity>& entities(const ReplicationReplayStreamSession& session) {
        return session.entities_;
    }

    static std::vector<ReplicationReplayFrame>& frames(ReplicationReplayStreamSession& session) {
        return session.frames_;
    }

    static const std::vector<ReplicationReplayFrame>& frames(const ReplicationReplayStreamSession& session) {
        return session.frames_;
    }

    static std::vector<std::uint8_t>& scratch(ReplicationReplayStreamSession& session) {
        return session.scratch_;
    }
};

namespace {

struct ReplayApplyReferenceContextData {
    ReplicationReplayStreamSession* session = nullptr;
};

ClientEntityNetworkId replay_network_id_for_wire(void*, std::uint32_t wire_network_id) {
    if (wire_network_id == 0U) {
        return invalid_client_entity_network_id;
    }
    return make_client_entity_network_id(0U, wire_network_id, 1U);
}

ecs::Entity replay_local_entity(void* userContext, ClientEntityNetworkId network_id) {
    auto& data = *static_cast<ReplayApplyReferenceContextData*>(userContext);
    if (data.session == nullptr) {
        return {};
    }
    const std::uint32_t replay_id = client_entity_network_id_wire_id(network_id);
    const auto& entities = ReplicationReplayStreamSessionAccess::entities(*data.session);
    const auto found = entities.find(replay_id);
    return found == entities.end() ? ecs::Entity{} : found->second;
}

EntityReferenceContext make_reference_context(ReplicationReplayStreamSession& session) {
    static thread_local ReplayApplyReferenceContextData data;
    data.session = &session;

    EntityReferenceContext context;
    context.userContext = &data;
    context.client_entity_network_id_for_wire = &replay_network_id_for_wire;
    context.client_local_entity = &replay_local_entity;
    return context;
}

ecs::Entity entity_for_replay_id(
    ReplicationReplayStreamSession& session,
    ecs::Registry& registry,
    std::uint32_t replay_id) {
    if (replay_id == 0U) {
        return {};
    }
    auto& entities = ReplicationReplayStreamSessionAccess::entities(session);
    const auto found = entities.find(replay_id);
    if (found != entities.end() && registry.alive(found->second)) {
        return found->second;
    }
    const ecs::Entity entity = registry.create();
    entities[replay_id] = entity;
    return entity;
}

void destroy_replay_entity(
    ReplicationReplayStreamSession& session,
    ecs::Registry& registry,
    std::uint32_t replay_id) {
    auto& entities = ReplicationReplayStreamSessionAccess::entities(session);
    const auto found = entities.find(replay_id);
    if (found == entities.end()) {
        return;
    }
    registry.destroy(found->second);
    entities.erase(found);
}

void apply_tags(
    ecs::Registry& registry,
    ecs::Entity entity,
    const SyncArchetype& archetype,
    std::uint64_t tag_mask) {
    for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
        const ecs::Entity tag = archetype.tags[tag_index].tag;
        if ((tag_mask & (std::uint64_t{1} << tag_index)) != 0U) {
            registry.add_tag(entity, tag);
        } else {
            registry.remove_tag(entity, tag);
        }
    }
}

bool apply_components(
    ecs::Registry& registry,
    ecs::Entity entity,
    const SyncArchetype& archetype,
    std::uint64_t present_mask,
    ecs::BitBuffer& payload,
    EntityReferenceContext& references,
    ReplicationReplayStreamSession& session) {
    const auto component_count = static_cast<std::uint16_t>(payload.read_bits(16U));
    for (std::uint16_t component = 0; component < component_count; ++component) {
        const auto component_index = static_cast<std::uint16_t>(payload.read_bits(16U));
        const auto component_bits = static_cast<std::uint16_t>(payload.read_bits(16U));
        ecs::BitBuffer component_payload;
        payload.read_buffer_bits(component_payload, component_bits);
        if (component_index >= archetype.components.size() ||
            component_index >= archetype.component_ops.size()) {
            continue;
        }
        const SyncComponentOps& ops = archetype.component_ops[component_index];
        if (ops.serialization.deserialize == nullptr ||
            ops.serialization.push_to_registry == nullptr ||
            ops.serialization.quantized_size == 0U) {
            continue;
        }
        std::vector<std::uint8_t>& scratch = ReplicationReplayStreamSessionAccess::scratch(session);
        scratch.assign(ops.serialization.quantized_size, 0U);
        ecs::ComponentSerializationContext serialization_context{ops.references_entities ? &references : nullptr};
        if (!ops.serialization.deserialize(
                component_payload,
                nullptr,
                scratch.data(),
                ops.references_entities ? &serialization_context : nullptr)) {
            return false;
        }
        if (!ops.serialization.push_to_registry(registry, entity, scratch.data())) {
            return false;
        }
    }

    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if ((present_mask & (std::uint64_t{1} << component_index)) == 0U) {
            registry.remove(entity, archetype.components[component_index].component);
        }
    }
    return true;
}

}  // namespace

ReplicationReplayStreamer::ReplicationReplayStreamer(ReplicationReplayStreamerOptions options)
    : options_(options) {
    if (options_.max_frames != 0U) {
        frames_.resize(options_.max_frames);
    }
}

void ReplicationReplayStreamer::clear() {
    if (options_.max_frames == 0U) {
        frames_.clear();
    }
    head_ = 0;
    count_ = 0;
}

void ReplicationReplayStreamer::push_frame(const ReplicationReplayFrame& frame) {
    if (options_.max_frames == 0U) {
        frames_.push_back(frame);
        count_ = frames_.size();
        return;
    }
    const std::size_t index = (head_ + count_) % frames_.size();
    frames_[index] = frame;
    if (count_ < frames_.size()) {
        ++count_;
    } else {
        head_ = (head_ + 1U) % frames_.size();
    }
}

void ReplicationReplayStreamer::push_frame(ReplicationReplayFrame&& frame) {
    if (options_.max_frames == 0U) {
        frames_.push_back(std::move(frame));
        count_ = frames_.size();
        return;
    }
    const std::size_t index = (head_ + count_) % frames_.size();
    frames_[index] = std::move(frame);
    if (count_ < frames_.size()) {
        ++count_;
    } else {
        head_ = (head_ + 1U) % frames_.size();
    }
}

bool ReplicationReplayStreamer::begin_session(
    SyncFrame focus_frame,
    ecs::Registry& registry,
    ReplicationServer& server,
    ReplicationReplayStreamSession& session) const {
    if (count_ == 0U) {
        return false;
    }
    const std::size_t start = find_start_frame(focus_frame);
    if (start >= count_) {
        return false;
    }

    session = {};
    session.end_frame = focus_frame + options_.tail_frames;
    session.active = true;
    auto& session_frames = ReplicationReplayStreamSessionAccess::frames(session);
    for (std::size_t index = start; index < count_; ++index) {
        const ReplicationReplayFrame& frame = frame_at(index);
        if (frame.frame > session.end_frame) {
            break;
        }
        session_frames.push_back(frame);
    }
    if (session_frames.empty() || !apply_frame(session_frames.front(), registry, session, false)) {
        session = {};
        return false;
    }
    server.rediscover_all_replicated_entities(registry);
    session.next_frame_index = 0;
    session.playback_frame = session_frames.front().frame;
    return true;
}

bool ReplicationReplayStreamer::begin_network_session(
    SyncFrame focus_frame,
    ecs::Registry& registry,
    ReplicationServer& server,
    ReplicationReplayStreamSession& session,
    ReplicationReplayNetworkSessionOptions options) const {
    if (!begin_session(focus_frame, registry, server, session)) {
        return false;
    }
    if (!attach_network_session_bandwidth(server, options)) {
        session = {};
        return false;
    }
    return true;
}

bool ReplicationReplayStreamer::attach_network_session_bandwidth(
    ReplicationServer& replay_server,
    ReplicationReplayNetworkSessionOptions options) const {
    if (options.source_server == nullptr || options.client == invalid_client_id) {
        return false;
    }
    std::shared_ptr<ReplicationBandwidthBudget> budget =
        options.source_server->client_bandwidth_budget(options.client);
    if (budget == nullptr) {
        return false;
    }
    return replay_server.set_client_bandwidth_budget(
        options.client,
        std::move(budget),
        options.bandwidth_share);
}

bool ReplicationReplayStreamer::tick_session(
    ReplicationReplayStreamSession& session,
    ecs::Registry& registry,
    ReplicationServer& server) const {
    if (!session.active) {
        session.active = false;
        return false;
    }
    append_available_frames(session);
    auto& session_frames = ReplicationReplayStreamSessionAccess::frames(session);
    if (session.playback_frame == 0U || session.playback_frame > session.end_frame) {
        session.active = false;
        return false;
    }

    while (session.next_frame_index < session_frames.size() &&
           session_frames[session.next_frame_index].frame <= session.playback_frame) {
        if (!apply_frame(session_frames[session.next_frame_index], registry, session)) {
            session.active = false;
            return false;
        }
        ++session.next_frame_index;
    }

    if (!server.advance_frame_without_simulating(registry, session.playback_frame)) {
        session.active = false;
        return false;
    }
    ++session.playback_frame;
    return true;
}

bool ReplicationReplayStreamer::apply_frame(
    const ReplicationReplayFrame& frame,
    ecs::Registry& registry,
    ReplicationReplayStreamSession& session) const {
    return apply_frame(frame, registry, session, true);
}

bool ReplicationReplayStreamer::apply_frame(
    const ReplicationReplayFrame& frame,
    ecs::Registry& registry,
    ReplicationReplayStreamSession& session,
    bool include_cues) const {
    try {
        const SyncSettings& settings = registry.get<SyncSettings>();
        ecs::BitBuffer payload = frame.payload;
        EntityReferenceContext references = make_reference_context(session);
        std::unordered_set<std::uint32_t> full_replay_ids;

        const auto record_count = static_cast<std::uint16_t>(payload.read_bits(16U));
        for (std::uint16_t record = 0; record < record_count; ++record) {
            const bool destroy = payload.read_bool();
            const auto replay_id = static_cast<std::uint32_t>(payload.read_bits(32U));
            if (destroy) {
                destroy_replay_entity(session, registry, replay_id);
                continue;
            }
            if (frame.kind == ReplicationReplayFrameKind::Full) {
                full_replay_ids.insert(replay_id);
            }

            const auto archetype_value = static_cast<std::uint32_t>(payload.read_bits(32U));
            const auto tag_mask = payload.read_unsigned_bits(64U);
            const auto present_mask = payload.read_unsigned_bits(64U);
            if (archetype_value >= settings.archetypes.size()) {
                return false;
            }
            const SyncArchetypeId archetype_id{archetype_value};
            const SyncArchetype& archetype = settings.archetypes[archetype_id.value];
            const ecs::Entity entity = entity_for_replay_id(session, registry, replay_id);
            if (!entity) {
                return false;
            }
            if (registry.contains<Replicated>(entity)) {
                registry.write<Replicated>(entity).archetype = archetype_id;
            } else {
                registry.add<Replicated>(entity, Replicated{archetype_id});
            }
            apply_tags(registry, entity, archetype, tag_mask);
            if (!apply_components(registry, entity, archetype, present_mask, payload, references, session)) {
                return false;
            }
        }

        if (frame.kind == ReplicationReplayFrameKind::Full) {
            std::vector<std::uint32_t> stale;
            for (const auto& mapped : ReplicationReplayStreamSessionAccess::entities(session)) {
                if (full_replay_ids.find(mapped.first) == full_replay_ids.end()) {
                    stale.push_back(mapped.first);
                }
            }
            for (const std::uint32_t replay_id : stale) {
                destroy_replay_entity(session, registry, replay_id);
            }
        }

        return !include_cues || apply_cues(registry, settings, payload, references, session);
    } catch (const std::exception&) {
        return false;
    }
}

bool ReplicationReplayStreamer::apply_cues(
    ecs::Registry& registry,
    const SyncSettings& settings,
    ecs::BitBuffer& payload,
    EntityReferenceContext& references,
    ReplicationReplayStreamSession& session) const {
    if (!payload.read_bool()) {
        return true;
    }
    const auto cue_count = static_cast<std::uint16_t>(payload.read_bits(16U));
    CueDispatcher& dispatcher = registry.write<CueDispatcher>();
    for (std::uint16_t cue_index = 0; cue_index < cue_count; ++cue_index) {
        const auto owner_replay_id = static_cast<std::uint32_t>(payload.read_bits(32U));
        const auto frame = static_cast<SyncFrame>(payload.read_bits(32U));
        const auto type = static_cast<SyncCueTypeId>(payload.read_bits(16U));
        float relevance_seconds = 0.0f;
        payload.read_bytes(reinterpret_cast<char*>(&relevance_seconds), sizeof(relevance_seconds));
        const bool only_replicate_to_owner = payload.read_bool();
        const auto cue_bits = static_cast<std::uint16_t>(payload.read_bits(16U));
        ecs::BitBuffer cue_payload;
        payload.read_buffer_bits(cue_payload, cue_bits);

        const auto& entities = ReplicationReplayStreamSessionAccess::entities(session);
        const auto found_owner = entities.find(owner_replay_id);
        if (found_owner == entities.end() || type >= settings.cue_ops.size()) {
            continue;
        }
        const SyncCueOps& ops = settings.cue_ops[type];
        QueuedSyncCue cue;
        cue.entity = found_owner->second;
        cue.frame = frame;
        cue.type = type;
        cue.relevance_seconds = relevance_seconds;
        cue.only_replicate_to_owner = only_replicate_to_owner;
        if (ops.references_entities) {
            if (ops.deserialize_value == nullptr) {
                continue;
            }
            cue.value = ops.deserialize_value(cue_payload, &references);
            if (!cue.value) {
                continue;
            }
        } else {
            cue.payload = std::move(cue_payload);
        }
        (void)dispatcher.enqueue(std::move(cue));
    }
    return true;
}

const ReplicationReplayFrame& ReplicationReplayStreamer::frame_at(std::size_t index) const {
    return frames_[(head_ + index) % frames_.size()];
}

void ReplicationReplayStreamer::append_available_frames(ReplicationReplayStreamSession& session) const {
    auto& session_frames = ReplicationReplayStreamSessionAccess::frames(session);
    if (session_frames.empty()) {
        return;
    }

    SyncFrame last_frame = session_frames.back().frame;
    for (std::size_t index = 0; index < count_; ++index) {
        const ReplicationReplayFrame& frame = frame_at(index);
        if (frame.frame <= last_frame || frame.frame > session.end_frame) {
            continue;
        }
        session_frames.push_back(frame);
        last_frame = frame.frame;
    }
}

std::size_t ReplicationReplayStreamer::find_start_frame(SyncFrame focus_frame) const {
    const SyncFrame target = focus_frame > options_.preroll_frames
        ? focus_frame - options_.preroll_frames
        : 1U;
    std::size_t start = count_;
    for (std::size_t index = 0; index < count_; ++index) {
        const ReplicationReplayFrame& frame = frame_at(index);
        if (frame.kind == ReplicationReplayFrameKind::Full &&
            frame.frame <= target) {
            start = index;
        }
    }
    return start;
}

}  // namespace kage::sync
