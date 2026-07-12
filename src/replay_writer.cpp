#include "ashiato/sync/replay_writer.hpp"

#include "detail/frame_data.hpp"

#include "ashiato/sync/components.hpp"
#include "ashiato/sync/assert.hpp"
#include "ashiato/sync/protocol.hpp"
#include "ashiato/sync/server.hpp"
#include "ashiato/sync/tracing.hpp"

#include "server/dirty_slots.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ashiato::sync {
namespace {

constexpr std::uint32_t replay_id_for_slot(std::uint32_t slot) noexcept {
    return slot + 1U;
}

constexpr std::uint32_t invalid_replay_slot = std::numeric_limits<std::uint32_t>::max();

struct ReplayReferenceContextData {
    ReplicationServer* server = nullptr;
};

struct PendingReplayCueEntry {
    std::uint32_t owner_replay_id = 0;
    SyncFrame frame = 0;
    SyncCueTypeId type = 0;
    float relevance_seconds = 0.0f;
    bool only_replicate_to_owner = false;
    ashiato::BitBuffer payload;
};

std::uint32_t replay_network_id_for_entity(void* userContext, ashiato::Entity entity) {
    auto& data = *static_cast<ReplayReferenceContextData*>(userContext);
    if (data.server == nullptr) {
        return 0U;
    }
    const std::uint32_t slot = data.server->replicated_slot_for_entity(entity);
    if (slot == invalid_replay_slot || !data.server->replicated_slot_active(slot)) {
        return 0U;
    }
    return replay_id_for_slot(slot);
}

std::uint64_t replay_tag_mask(const ashiato::Registry& registry, const SyncArchetype& archetype, ashiato::Entity entity) {
    std::uint64_t mask = 0;
    for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
        if (registry.has(entity, archetype.tags[tag_index].tag)) {
            mask |= std::uint64_t{1} << tag_index;
        }
    }
    return mask;
}

bool quantize_replay_entity(
    const ashiato::Registry& registry,
    const SyncArchetype& archetype,
    ashiato::Entity entity,
    QuantizedFrameData& out) {
    if (!detail::init_frame_data(archetype, out)) {
        return false;
    }
    if (detail::has_tag_slot(archetype)) {
        out.tag_mask = replay_tag_mask(registry, archetype, entity);
    }
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (component_index >= archetype.component_ops.size()) {
            return false;
        }
        const SyncComponentOps& ops = archetype.component_ops[component_index];
        if (ops.serialization.quantize == nullptr) {
            return false;
        }
        const void* component = registry.get(entity, archetype.components[component_index].component);
        if (component == nullptr) {
            continue;
        }
        std::uint8_t* destination = detail::mutable_frame_component_data(archetype, out, component_index);
        if (destination == nullptr) {
            return false;
        }
        ops.serialization.quantize(component, destination);
    }
    return out.present_mask != 0U || detail::has_tag_slot(archetype);
}

void append_cue_entries(
    const QueuedSyncCueView& cues,
    const SyncSettings& settings,
    ReplicationServer& server,
    EntityReferenceContext& reference_context,
    std::vector<PendingReplayCueEntry>& out,
    SyncFrame sync_frame
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    ,
    SyncTracer* serialization_tracer
#endif
) {
#ifndef ASHIATO_SYNC_ENABLE_TRACING
    (void)sync_frame;
#endif
    const std::size_t count = std::min<std::size_t>(cues.size, std::numeric_limits<std::uint16_t>::max());
    for (std::size_t index = 0; index < count; ++index) {
        const QueuedSyncCue& cue = cues.data[index];
        if (cue.type >= settings.cue_ops.size() || settings.cue_ops[cue.type].serialize == nullptr) {
            continue;
        }
        if (!detail::cue_relevance_fits_frame_range(
                cue.frame,
                cue.relevance_seconds,
                server.options().fixed_dt_seconds)) {
            ASHIATO_SYNC_ASSERT_FAIL("cue relevance must be finite, non-negative, and fit in the frame range");
            continue;
        }
        ashiato::BitBuffer payload = cue.payload;
        if (cue.value.has_value()) {
            payload.clear();
            EntityReferenceContext* references = settings.cue_ops[cue.type].references_entities ? &reference_context : nullptr;
#ifdef ASHIATO_SYNC_ENABLE_TRACING
            ScopedSerializationTraceCapture cue_serialization_capture(
                serialization_tracer,
                SyncTracePayloadSource::Replay,
                SyncTraceRole::Server,
                invalid_client_id,
                sync_frame,
                "replay_cue_payload",
                false);
            cue_serialization_capture.set_target(&payload);
            if (cue_serialization_capture.active()) {
                SyncTraceEvent& event = cue_serialization_capture.event();
                event.server_entity = cue.entity;
                event.cue_type = cue.type;
                event.component_name = settings.cue_ops[cue.type].name;
                event.data = "payload_kind=cue";
            }
            ashiato::ComponentSerializationContext serialization_context{
                references,
                cue_serialization_capture.payload_capture()};
            serialization_context.currentFrame = cue.frame;
            serialization_context.previousFrame = 0U;
            {
                ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(cue_serialization_capture, settings.cue_ops[cue.type].name.c_str());
                settings.cue_ops[cue.type].serialize(cue.value.data(), payload, serialization_context);
            }
            if (cue_serialization_capture.active()) {
                SyncTraceEvent& event = cue_serialization_capture.event();
                event.payload_bits = payload.bit_size();
                event.wire_bits = payload.bit_size();
                event.data += ",payload_bits=" + std::to_string(payload.bit_size());
                event.data += ",payload_bytes=" + std::to_string(payload.byte_size());
                cue_serialization_capture.flush();
            }
#else
            ashiato::ComponentSerializationContext serialization_context{references};
            serialization_context.currentFrame = cue.frame;
            serialization_context.previousFrame = 0U;
            settings.cue_ops[cue.type].serialize(cue.value.data(), payload, serialization_context);
#endif
        }
        if (payload.bit_size() > protocol::max_cue_payload_bits) {
            ASHIATO_SYNC_ASSERT_FAIL("serialized cue payload exceeds the protocol limit");
            continue;
        }
        const std::uint32_t slot = server.replicated_slot_for_entity(cue.entity);
        out.push_back(PendingReplayCueEntry{
            slot == invalid_replay_slot ? 0 : replay_id_for_slot(slot),
            cue.frame,
            cue.type,
            cue.relevance_seconds,
            cue.only_replicate_to_owner,
            std::move(payload)});
    }
}

void write_cue_entries(
    const std::vector<PendingReplayCueEntry>& cues,
    SyncFrame sync_frame,
    ashiato::BitBuffer& out
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    ,
    ScopedSerializationTraceCapture* serialization_capture
#endif
) {
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    ScopedSerializationTraceScope cue_entries_scope(serialization_capture, "cue_entries");
#endif
    for (const PendingReplayCueEntry& cue : cues) {
        if (cue.payload.bit_size() > protocol::max_cue_payload_bits) {
            ASHIATO_SYNC_ASSERT_FAIL("replay cue payload exceeds the protocol limit");
            continue;
        }
        ASHIATO_SERIALIZE_BOOL_TRACE_WITH_CAPTURE(serialization_capture, out, true, "has_cue");
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        ScopedSerializationTraceScope cue_entry_scope(serialization_capture, "cue_entry");
#endif
        ashiato::BitBuffer entry;
        entry.write_bits(cue.owner_replay_id, 32U);
        protocol::write_cue_frame(entry, sync_frame, cue.frame);
        entry.write_bits(cue.type, 16U);
        entry.write_float32_le(cue.relevance_seconds);
        entry.write_bool(cue.only_replicate_to_owner);
        entry.write_bits(static_cast<std::uint16_t>(cue.payload.bit_size()), 16U);
        entry.write_buffer_bits(cue.payload);
        ASHIATO_SERIALIZE_BUFFER_TRACE_WITH_CAPTURE(serialization_capture, out, entry, "payload");
    }
    ASHIATO_SERIALIZE_BOOL_TRACE_WITH_CAPTURE(serialization_capture, out, false, "has_cue");
}

}  // namespace

struct ReplicationReplayWriter::State {
    std::vector<std::uint8_t> pending_dirty_slots;
    std::vector<std::uint32_t> slots;
    std::vector<std::uint32_t> pending_slots;
    std::vector<ServerDestroyedReplicatedSlot> pending_destroyed_slots;
    std::vector<PendingReplayCueEntry> pending_cues;
    QuantizedFrameData scratch;
    bool has_written = false;
    SyncFrame last_full_frame = 0;
};

ReplicationReplayWriter::ReplicationReplayWriter(ReplicationReplayWriterOptions options)
    : options_(std::move(options)),
      state_(std::make_unique<State>()) {}

ReplicationReplayWriter::~ReplicationReplayWriter() = default;

ReplicationReplayWriter::ReplicationReplayWriter(ReplicationReplayWriter&& other) noexcept
    : options_(std::move(other.options_)),
      state_(std::move(other.state_)),
      server_(other.server_),
      subscription_(std::move(other.subscription_)) {
    other.server_ = nullptr;
    other.detach();
}

ReplicationReplayWriter& ReplicationReplayWriter::operator=(ReplicationReplayWriter&& other) noexcept {
    if (this != &other) {
        detach();
        options_ = std::move(other.options_);
        state_ = std::move(other.state_);
        server_ = other.server_;
        subscription_ = std::move(other.subscription_);
        other.server_ = nullptr;
        other.detach();
    }
    return *this;
}

void ReplicationReplayWriter::attach(ReplicationServer& server) {
    detach();
    server_ = &server;
    subscription_ = server.subscribe_registry_dirty_frame_listener(*this);
}

void ReplicationReplayWriter::detach() {
    subscription_.reset();
    server_ = nullptr;
}

bool ReplicationReplayWriter::attached() const noexcept {
    return subscription_.active();
}

void ReplicationReplayWriter::on_server_registry_dirty_frame(const ServerRegistryDirtyFrame& frame) {
    if (!options_.write || state_ == nullptr || server_ == nullptr) {
        return;
    }
    if (&frame.server != server_) {
        return;
    }
    ReplicationServer& server = frame.server;
    const SyncSettings& settings = frame.registry.get<SyncSettings>();
    const SyncFrame sync_frame = frame.frame;
    const ServerDestroyedReplicatedSlotView destroyed_slots = frame.destroyed_slots;
    const QueuedSyncCueView cues = frame.cues;

    ReplayReferenceContextData reference_data{&server};
    EntityReferenceContext reference_context;
    reference_context.userContext = &reference_data;
    reference_context.network_entity_id_tier0_bits = server.options().protocol.network_entity_id_tier0_bits;
    reference_context.server_network_id_for_entity = &replay_network_id_for_entity;

    append_cue_entries(
        cues,
        settings,
        server,
        reference_context,
        state_->pending_cues,
        sync_frame
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        ,
        options_.serialization_tracer
#endif
    );

    for (const ServerDestroyedReplicatedSlot& destroyed : destroyed_slots) {
        state_->pending_destroyed_slots.push_back(destroyed);
    }

    if (state_->pending_dirty_slots.size() != server.replicated_slot_count()) {
        state_->pending_dirty_slots.resize(server.replicated_slot_count(), std::uint8_t{0});
    }
    auto mark_pending_slot = [&](std::uint32_t slot) {
        if (slot >= state_->pending_dirty_slots.size() || state_->pending_dirty_slots[slot] != 0U) {
            return;
        }
        state_->pending_dirty_slots[slot] = std::uint8_t{1};
        state_->pending_slots.push_back(slot);
    };
    server_detail::each_dirty_replicated_slot(frame, settings, mark_pending_slot);

    const SyncFrame write_interval = options_.write_interval_frames == 0U ? 1U : options_.write_interval_frames;
    const bool write_due = !state_->has_written || write_interval <= 1U || (sync_frame % write_interval) == 0U;
    if (!write_due) {
        return;
    }

    const bool full = !state_->has_written ||
        (options_.full_frame_interval_frames != 0U &&
            sync_frame - state_->last_full_frame >= options_.full_frame_interval_frames);

    state_->slots.clear();
    if (full) {
        state_->slots.reserve(server.replicated_slot_count());
        for (std::uint32_t slot = 0; slot < server.replicated_slot_count(); ++slot) {
            if (server.replicated_slot_is_replicable(frame.registry, slot)) {
                state_->slots.push_back(slot);
            }
        }
    } else {
        state_->slots = state_->pending_slots;
    }

    ashiato::BitBuffer payload;
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    ScopedSerializationTraceCapture serialization_capture(
        options_.serialization_tracer,
        SyncTracePayloadSource::Replay,
        SyncTraceRole::Server,
        invalid_client_id,
        sync_frame,
        full ? "replay_full_frame" : "replay_delta_frame");
    serialization_capture.set_target(&payload);
    if (serialization_capture.active()) {
        serialization_capture.event().data = full ? "source=replay,kind=full" : "source=replay,kind=delta";
    }
#endif
    const std::size_t record_count_offset = payload.bit_size();
    ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(serialization_capture, payload, 0, 16U, "record_count");
    std::uint16_t record_count = 0;
    std::vector<std::uint8_t> emitted_destroyed_slots;

    if (!full) {
        emitted_destroyed_slots.assign(server.replicated_slot_count(), std::uint8_t{0});
        for (const ServerDestroyedReplicatedSlot& destroyed : state_->pending_destroyed_slots) {
            if (record_count == std::numeric_limits<std::uint16_t>::max()) {
                break;
            }
            if (destroyed.slot < emitted_destroyed_slots.size() && emitted_destroyed_slots[destroyed.slot] != 0U) {
                continue;
            }
            {
                ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture, "destroy_record");
                ASHIATO_SERIALIZE_BOOL_TRACE_WITH_CONTEXT(serialization_capture, payload, true, "destroy");
                ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(serialization_capture, payload, replay_id_for_slot(destroyed.slot), 32U, "entity");
            }
            if (destroyed.slot < emitted_destroyed_slots.size()) {
                emitted_destroyed_slots[destroyed.slot] = std::uint8_t{1};
            }
            ++record_count;
        }
    }

    for (const std::uint32_t slot : state_->slots) {
        if (slot == invalid_replay_slot || slot >= server.replicated_slot_count()) {
            continue;
        }
        const bool active = server.replicated_slot_is_replicable(frame.registry, slot);
        if (!active && full) {
            continue;
        }
        if (!active && slot < emitted_destroyed_slots.size() && emitted_destroyed_slots[slot] != 0U) {
            continue;
        }
        if (record_count == std::numeric_limits<std::uint16_t>::max()) {
            break;
        }
        {
            ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(
                serialization_capture,
                active ? "entity_record_header" : "destroy_record");
            ASHIATO_SERIALIZE_BOOL_TRACE_WITH_CONTEXT(serialization_capture, payload, !active, "destroy");
            ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(serialization_capture, payload, replay_id_for_slot(slot), 32U, "entity");
        }
        if (!active) {
            ++record_count;
            continue;
        }

        const ashiato::Entity entity = server.replicated_slot_entity(slot);
        const SyncArchetypeId archetype_id = server.replicated_slot_archetype(slot);
        if (archetype_id.value >= settings.archetypes.size()) {
            continue;
        }
        const SyncArchetype& archetype = settings.archetypes[archetype_id.value];
        if (!quantize_replay_entity(frame.registry, archetype, entity, state_->scratch)) {
            continue;
        }
        {
            ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture, "entity_metadata");
            ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(serialization_capture, payload, archetype_id.value, 32U, "archetype");
            ASHIATO_SERIALIZE_UNSIGNED_TRACE_WITH_CONTEXT(serialization_capture, payload, state_->scratch.tag_mask, 64U, "tag_mask");
            ASHIATO_SERIALIZE_UNSIGNED_TRACE_WITH_CONTEXT(
                serialization_capture,
                payload,
                state_->scratch.present_mask,
                64U,
                "present_mask");
        }

        const std::size_t component_count_offset = payload.bit_size();
        ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(serialization_capture, payload, 0, 16U, "component_count");
        std::uint16_t component_count = 0;
        for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
            if (!detail::frame_has_component(state_->scratch, component_index) ||
                component_index >= archetype.component_ops.size()) {
                continue;
            }
            const SyncComponentOps& ops = archetype.component_ops[component_index];
            if (ops.serialization.serialize == nullptr) {
                continue;
            }
            const std::uint8_t* current =
                detail::frame_component_data(archetype, state_->scratch, component_index);
            if (current == nullptr) {
                continue;
            }
            {
                ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture, "component_record");
                ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(
                    serialization_capture,
                    payload,
                    static_cast<std::int64_t>(component_index),
                    16U,
                    "component_index");
                const std::size_t component_bits_offset = payload.bit_size();
                ASHIATO_SERIALIZE_TRACE_WITH_CONTEXT(serialization_capture, payload, 0, 16U, "payload_bits");
                const std::size_t component_payload_begin_bits = payload.bit_size();
#ifdef ASHIATO_SYNC_ENABLE_TRACING
                ashiato::ComponentSerializationContext serialization_context{
                    ops.references_entities ? &reference_context : nullptr,
                    serialization_capture.payload_capture()};
                serialization_context.currentFrame = sync_frame;
                serialization_context.previousFrame = 0U;
                ASHIATO_SYNC_TRACE_SCOPE_WITH_CONTEXT(serialization_capture, ops.serialization.name.c_str());
#else
                ashiato::ComponentSerializationContext serialization_context{
                    ops.references_entities ? &reference_context : nullptr};
                serialization_context.currentFrame = sync_frame;
                serialization_context.previousFrame = 0U;
#endif
                ops.serialization.serialize(nullptr, current, payload, serialization_context);
                const std::size_t component_payload_bits = payload.bit_size() - component_payload_begin_bits;
                payload.overwrite_unsigned_bits(
                    component_bits_offset,
                    static_cast<std::uint64_t>(std::min<std::size_t>(
                        component_payload_bits,
                        std::numeric_limits<std::uint16_t>::max())),
                    16U);
            }
            ++component_count;
        }
        payload.overwrite_unsigned_bits(component_count_offset, component_count, 16U);
        ++record_count;
    }

    payload.overwrite_unsigned_bits(record_count_offset, record_count, 16U);
    write_cue_entries(
        state_->pending_cues,
        sync_frame,
        payload
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        ,
        &serialization_capture
#endif
    );

#ifdef ASHIATO_SYNC_ENABLE_TRACING
    serialization_capture.flush();
#endif
    options_.write(ReplicationReplayFrame{
        sync_frame,
        full ? ReplicationReplayFrameKind::Full : ReplicationReplayFrameKind::Delta,
        std::move(payload)});

    std::fill(state_->pending_dirty_slots.begin(), state_->pending_dirty_slots.end(), std::uint8_t{0});
    state_->pending_slots.clear();
    state_->pending_destroyed_slots.clear();
    state_->pending_cues.clear();
    state_->has_written = true;
    if (full) {
        state_->last_full_frame = sync_frame;
    }
}

}  // namespace ashiato::sync
