#include "client/runtime/update_runtime.hpp"

#include "client/runtime/buffered_runtime.hpp"
#include "client/runtime/cue_runtime.hpp"
#include "client/store/entity_store.hpp"
#include "client/frame_data.hpp"
#include "client/store/frame_ring_store.hpp"
#include "client/runtime/prediction_runtime.hpp"
#include "client/tracing.hpp"

#include "ashiato/sync/client.hpp"
#include "ashiato/sync/detail/bit_reader.hpp"
#include "ashiato/sync/protocol.hpp"
#include "ashiato/sync/tracing.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <utility>

namespace ashiato::sync::client_detail {
namespace {

using client_detail::frame_component_data;
using client_detail::frame_has_component;
using client_detail::has_tag_slot;
using client_detail::init_frame_data;
using client_detail::mutable_frame_component_data;
using client_detail::remove_archetype_tags;
using client_detail::sync_slot_bit;
using client_detail::sync_slot_count;

}  // namespace

bool ClientUpdateRuntime::apply_update(
    ReplicationClient& client,
    ashiato::Registry& registry,
    detail::BitReader& packet,
    std::uint32_t packet_id,
    SyncFrame frame,
    std::uint16_t record_count) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    bool applied_any = false;

    for (std::uint16_t record = 0; record < record_count; ++record) {
        bool destroy = false;
        std::uint32_t wire_network_id = 0;
        if (!packet.read_bits(1U, destroy) ||
            !protocol::read_network_entity_id(
                packet,
                wire_network_id,
                client.options_.network.protocol.network_entity_id_tier0_bits)) {
            return false;
        }
        if (wire_network_id == 0U) {
            return false;
        }

        const bool applied = destroy
            ? apply_destroy(client, registry, frame, wire_network_id)
            : apply_upsert(client, registry, settings, frame, wire_network_id, packet);
        if (!applied) {
            return false;
        }
        applied_any = true;
    }

    if (applied_any) {
        client.queue_ack(packet_id);
    }
    return applied_any;
}

void ClientUpdateRuntime::reset_previously_absent_entity(
    ReplicationClient& client,
    EntityState& state,
    SyncArchetypeId archetype) {
    const std::uint32_t entity_index = client.entity_store_->index_of(state);
    client.cue_runtime_->erase_for_entity(entity_index);
    client.reset_absent_entity_state(state, archetype);
}

ReplicationClientMode ClientUpdateRuntime::select_entity_mode(
    ReplicationClient& client,
    const ReplicatedEntityUpdateView& update,
    const SyncSettings& settings,
    bool mode_needs_selection) const {
    ReplicationClientMode selected_mode = client.options_.entities.default_mode;
    if (!mode_needs_selection) {
        return selected_mode;
    }
    if (client.options_.entities.mode_selector) {
        selected_mode = client.options_.entities.mode_selector(update);
    }
    if (selected_mode == ReplicationClientMode::Predict) {
        (void)client.validate_predicted_archetype(settings, update.archetype);
    }
    return selected_mode;
}

bool ClientUpdateRuntime::apply_upsert(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    std::uint32_t wire_network_id,
    detail::BitReader& packet) {
    UpsertMetadata metadata;
    if (!read_upsert_metadata(client, settings, frame, wire_network_id, packet, metadata)) {
        return false;
    }

    AuthoritativeUpsertRecord record;
    if (!decode_upsert_record(client, settings, metadata, packet, record)) {
        return false;
    }

    std::vector<EntityCue>& received_cues = received_cues_scratch_;
    if (!read_cues(packet, received_cues)) {
        return false;
    }
    record.received_cues = &received_cues;

    build_upsert_mode_selector_updates(client, settings, metadata, record, selector_updates_scratch_);
    EntityState* state =
        ensure_upsert_entity(client, registry, settings, metadata, record, selector_updates_scratch_);
    if (state == nullptr) {
        return false;
    }
    trace_cues_received(client, settings, metadata, *record.received_cues);
    trace_received_upsert_record(client, settings, metadata, record, *state);
    if (!apply_upsert_record(client, registry, settings, *state, metadata, record)) {
        return false;
    }

    finish_upsert(client, metadata);
    return true;
}

bool ClientUpdateRuntime::read_upsert_metadata(
    ReplicationClient& client,
    const SyncSettings& settings,
    SyncFrame frame,
    std::uint32_t wire_network_id,
    detail::BitReader& packet,
    UpsertMetadata& metadata) {
    metadata = {};
    metadata.frame = frame;
    metadata.wire_network_id = wire_network_id;
    if (!packet.read_bits(1U, metadata.is_full_upsert)) {
        return false;
    }
    metadata.client_entity_network_id = client.client_entity_network_id_for_wire(wire_network_id);
    if (metadata.client_entity_network_id == invalid_client_entity_network_id) {
        return false;
    }

    if (metadata.is_full_upsert) {
        if (client.destroy_tombstone_blocks(wire_network_id, frame)) {
            return false;
        }
        metadata.found_state = client.find_entity_state(metadata.client_entity_network_id);
    } else {
        metadata.found_state = client.find_entity_state_by_wire_id(wire_network_id);
    }
    metadata.previous_absent = metadata.found_state != nullptr &&
        !metadata.found_state->replication.entity_present &&
        !metadata.found_state->identity.local;

    if (metadata.is_full_upsert) {
        std::uint32_t archetype_value = 0;
        if (!packet.read_bits(32U, archetype_value)) {
            return false;
        }
        metadata.archetype = SyncArchetypeId{archetype_value};
        if (metadata.archetype.value >= settings.archetypes.size()) {
            return false;
        }
        if (metadata.found_state != nullptr &&
            !metadata.previous_absent &&
            metadata.found_state->replication.entity_present &&
            metadata.found_state->identity.archetype != metadata.archetype) {
            return false;
        }
        if (metadata.found_state != nullptr &&
            !metadata.previous_absent &&
            frame <= metadata.found_state->replication.frame) {
            return false;
        }
        if (metadata.previous_absent) {
            reset_previously_absent_entity(client, *metadata.found_state, metadata.archetype);
        }
        return true;
    }

    if (metadata.found_state == nullptr || metadata.previous_absent) {
        return false;
    }
    metadata.archetype = metadata.found_state->identity.archetype;
    if (metadata.archetype.value >= settings.archetypes.size()) {
        return false;
    }
    if (frame <= metadata.found_state->replication.frame) {
        return false;
    }
    if (!protocol::read_baseline_frame(packet, frame, metadata.baseline_frame)) {
        return false;
    }
    metadata.previous_state = metadata.found_state;
    metadata.previous_baseline = client.find_baseline(*metadata.previous_state, metadata.baseline_frame);
    return metadata.previous_baseline != nullptr;
}

bool ClientUpdateRuntime::decode_upsert_record(
    ReplicationClient& client,
    const SyncSettings& settings,
    const UpsertMetadata& metadata,
    detail::BitReader& packet,
    AuthoritativeUpsertRecord& record) {
    record = {};
    const SyncArchetype& definition = settings.archetypes[metadata.archetype.value];

    if (!init_frame_data(definition, record.authoritative)) {
        return false;
    }
    return metadata.is_full_upsert
        ? decode_full_upsert_record_payload(client, settings, metadata, packet, record)
        : decode_delta_upsert_record_payload(client, settings, metadata, packet, record);
}

bool ClientUpdateRuntime::decode_full_upsert_record_payload(
    ReplicationClient& client,
    const SyncSettings& settings,
    const UpsertMetadata& metadata,
    detail::BitReader& packet,
    AuthoritativeUpsertRecord& record) {
    const SyncArchetype& definition = settings.archetypes[metadata.archetype.value];
    const std::size_t sync_slot_bits = protocol::bits_for_range(sync_slot_count(definition));

    bool uses_presence_mask = false;
    if (!packet.read_bits(1U, uses_presence_mask)) {
        return false;
    }
    std::uint64_t presence_mask = 0;
    std::uint16_t component_count = 0;
    if (uses_presence_mask) {
        if (!packet.read_bits(sync_slot_count(definition), presence_mask)) {
            return false;
        }
        for (std::size_t sync_slot = 0; sync_slot < sync_slot_count(definition); ++sync_slot) {
            if ((presence_mask & sync_slot_bit(sync_slot)) != 0U) {
                ++component_count;
            }
        }
    } else if (!packet.read_bits(16U, component_count)) {
        return false;
    }

    EntityReferenceContext reference_context;
    bool reference_context_initialized = false;
    auto references_for_component = [&]() -> EntityReferenceContext* {
        if (!reference_context_initialized) {
            reference_context = make_upsert_reference_context(client);
            reference_context_initialized = true;
        }
        return &reference_context;
    };

    auto read_slot = [&](std::uint16_t sync_slot) {
        if (sync_slot >= sync_slot_count(definition)) {
            return false;
        }
        if (sync_slot == 0U) {
            if (!has_tag_slot(definition)) {
                return false;
            }
            if (!packet.read_bits(definition.tags.size(), record.authoritative.tag_mask)) {
                return false;
            }
            record.changed_sync_slots |= sync_slot_bit(sync_slot);
            return true;
        }

        const std::size_t component_index = static_cast<std::size_t>(sync_slot - 1U);
        if (component_index >= definition.component_ops.size()) {
            return false;
        }
        const SyncComponentOps& ops = definition.component_ops[component_index];
        if (ops.serialization.deserialize == nullptr || ops.serialization.push_to_registry == nullptr) {
            return false;
        }

        std::uint8_t* received_bytes = mutable_frame_component_data(definition, record.authoritative, component_index);
        if (received_bytes == nullptr) {
            return false;
        }
        EntityReferenceContext* references = ops.references_entities ? references_for_component() : nullptr;
        ashiato::ComponentSerializationContext serialization_context{references};
        if (!ops.serialization.deserialize(
                packet.raw(),
                nullptr,
                received_bytes,
                references != nullptr ? &serialization_context : nullptr)) {
            return false;
        }
        record.changed_sync_slots |= sync_slot_bit(sync_slot);
        return true;
    };

    if (uses_presence_mask) {
        for (std::size_t sync_slot = 0; sync_slot < sync_slot_count(definition); ++sync_slot) {
            if ((presence_mask & sync_slot_bit(sync_slot)) == 0U) {
                continue;
            }
            if (!read_slot(static_cast<std::uint16_t>(sync_slot))) {
                return false;
            }
        }
    } else {
        for (std::uint16_t component = 0; component < component_count; ++component) {
            std::uint16_t sync_slot = 0;
            if (!packet.read_bits(sync_slot_bits, sync_slot) || !read_slot(sync_slot)) {
                return false;
            }
        }
    }
    return true;
}

bool ClientUpdateRuntime::decode_delta_upsert_record_payload(
    ReplicationClient& client,
    const SyncSettings& settings,
    const UpsertMetadata& metadata,
    detail::BitReader& packet,
    AuthoritativeUpsertRecord& record) {
    if (metadata.previous_baseline == nullptr) {
        return false;
    }
    const SyncArchetype& definition = settings.archetypes[metadata.archetype.value];
    std::uint64_t changed_mask = 0;
    if (!packet.read_bits(sync_slot_count(definition), changed_mask)) {
        return false;
    }
    record.changed_sync_slots = changed_mask;
    record.authoritative = *metadata.previous_baseline;

    if ((changed_mask & sync_slot_bit(0)) != 0U) {
        if (!has_tag_slot(definition)) {
            return false;
        }
        if (!packet.read_bits(definition.tags.size(), record.authoritative.tag_mask)) {
            return false;
        }
    }

    EntityReferenceContext reference_context;
    bool reference_context_initialized = false;
    auto references_for_component = [&]() -> EntityReferenceContext* {
        if (!reference_context_initialized) {
            reference_context = make_upsert_reference_context(client);
            reference_context_initialized = true;
        }
        return &reference_context;
    };

    for (std::size_t component_index = 0; component_index < definition.components.size(); ++component_index) {
        if ((changed_mask & sync_slot_bit(component_index + 1U)) == 0U) {
            continue;
        }
        if (component_index >= definition.component_ops.size()) {
            return false;
        }
        const SyncComponentOps& ops = definition.component_ops[component_index];
        if (ops.serialization.deserialize == nullptr || ops.serialization.push_to_registry == nullptr) {
            return false;
        }

        const std::uint8_t* previous_bytes =
            frame_component_data(definition, *metadata.previous_baseline, component_index);
        std::uint8_t* merged_bytes = mutable_frame_component_data(definition, record.authoritative, component_index);
        if (previous_bytes == nullptr || merged_bytes == nullptr) {
            return false;
        }
        EntityReferenceContext* references = ops.references_entities ? references_for_component() : nullptr;
        ashiato::ComponentSerializationContext serialization_context{references};
        if (!ops.serialization.deserialize(
                packet.raw(),
                previous_bytes,
                merged_bytes,
                references != nullptr ? &serialization_context : nullptr)) {
            return false;
        }
    }
    return true;
}

EntityReferenceContext ClientUpdateRuntime::make_upsert_reference_context(ReplicationClient& client) {
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

bool ClientUpdateRuntime::read_cues(detail::BitReader& packet, std::vector<EntityCue>& out) {
    out.clear();
    bool has_cues = false;
    if (!packet.read_bits(1U, has_cues)) {
        return false;
    }
    if (!has_cues) {
        return true;
    }
    std::uint16_t cue_count = 0;
    if (!packet.read_bits(16U, cue_count)) {
        return false;
    }
    out.reserve(cue_count);
    for (std::uint16_t index = 0; index < cue_count; ++index) {
		EntityCue cue;
		std::uint16_t payload_bits = 0;
		if (!packet.read_bits(32U, cue.frame) ||
			!packet.read_bits(16U, cue.type) ||
			!packet.read_bits(16U, payload_bits) ||
			!packet.read_buffer_bits(cue.payload, payload_bits)) {
			return false;
        }
        out.push_back(std::move(cue));
    }
    return true;
}

void ClientUpdateRuntime::trace_cues_received(
    ReplicationClient& client,
    const SyncSettings& settings,
    const UpsertMetadata& metadata,
    const std::vector<EntityCue>& out) {
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_PACKET_LOGS)
    if (!out.empty()) {
        std::vector<std::string>& cue_summaries = client.cue_runtime_->store().current_packet_cue_summaries;
        cue_summaries.reserve(cue_summaries.size() + out.size());
        for (const EntityCue& cue : out) {
            std::ostringstream summary;
            summary << "{entity=" << metadata.client_entity_network_id
                << ",frame=" << cue.frame
                << ",type=" << cue.type;
#ifdef ASHIATO_SYNC_TRACE_COMPONENT_DATA
            if (client.tracer_ != nullptr && client.tracer_->frame_data_enabled() &&
                cue.type < settings.cue_ops.size() && settings.cue_ops[cue.type].trace != nullptr) {
                SyncTraceStringBuilder builder;
                if (settings.cue_ops[cue.type].trace(
                        cue.type,
                        settings.cue_ops[cue.type].user_data,
                        cue.payload,
                        builder) &&
                    !builder.value.empty()) {
                    summary << ",data=" << builder.value;
                }
            }
#endif
            summary << "}";
            cue_summaries.push_back(summary.str());
        }
    }
#endif
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    if (client.tracer_ != nullptr && client.tracer_->enabled()) {
        for (const EntityCue& cue : out) {
            SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::CueReceived, client.client_id_, cue.frame);
            event.server_entity = ashiato::Entity{metadata.client_entity_network_id};
            event.client_network_id = metadata.client_entity_network_id;
            event.wire_network_id = metadata.wire_network_id;
            event.network_version = client_entity_network_id_version(metadata.client_entity_network_id);
            event.archetype = metadata.archetype;
            event.cue_type = cue.type;
            append_trace_cue_name(settings, cue.type, event);
            append_trace_cue_data(client.tracer_, settings, cue.type, cue.payload, event);
            append_trace_data_field(event, "source", "server");
            client.tracer_->trace(event);
        }
    }
#else
    (void)client;
    (void)settings;
    (void)metadata;
    (void)out;
#endif
}

EntityState* ClientUpdateRuntime::ensure_upsert_entity(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    const UpsertMetadata& metadata,
    const AuthoritativeUpsertRecord& record,
    std::vector<ComponentBaseline>& selector_updates) {
    const bool mode_needs_selection = metadata.found_state == nullptr ||
        metadata.previous_absent ||
        !metadata.found_state->mode.selected;
    ReplicatedEntityUpdateView mode_update;
    mode_update.client_entity_network_id = metadata.client_entity_network_id;
    mode_update.local_entity = metadata.found_state != nullptr ? metadata.found_state->identity.local : ashiato::Entity{};
    mode_update.archetype = metadata.archetype;
    mode_update.frame = metadata.frame;
    mode_update.tag_mask = record.authoritative.tag_mask;
    mode_update.components = &selector_updates;
    const ReplicationClientMode selected_mode = select_entity_mode(client, mode_update, settings, mode_needs_selection);

    EntityState* ensured_state = client.ensure_entity_state(registry, metadata.client_entity_network_id, metadata.wire_network_id);
    if (ensured_state == nullptr) {
        return nullptr;
    }
    EntityState& state = *ensured_state;
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    if (client.tracer_ != nullptr && client.tracer_->enabled() &&
        metadata.is_full_upsert && (metadata.found_state == nullptr || metadata.previous_absent)) {
        SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::EntityReceived, client.client_id_, metadata.frame);
        event.server_entity = ashiato::Entity{metadata.client_entity_network_id};
        event.local_entity = state.identity.local;
        event.client_network_id = metadata.client_entity_network_id;
        event.wire_network_id = metadata.wire_network_id;
        event.network_version = client_entity_network_id_version(metadata.client_entity_network_id);
        event.archetype = metadata.archetype;
        client.tracer_->trace(event);
    }
#endif
    if (mode_needs_selection) {
#ifdef ASHIATO_SYNC_ENABLE_TRACING
        if (client.tracer_ != nullptr && client.tracer_->enabled() && state.mode.current != selected_mode) {
            SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::ModeChanged, client.client_id_, metadata.frame);
            event.server_entity = ashiato::Entity{metadata.client_entity_network_id};
            event.local_entity = state.identity.local;
            event.client_network_id = metadata.client_entity_network_id;
            event.wire_network_id = metadata.wire_network_id;
            event.network_version = client_entity_network_id_version(metadata.client_entity_network_id);
            event.archetype = metadata.archetype;
            event.previous_mode = state.mode.current;
            event.mode = selected_mode;
            client.tracer_->trace(event);
        }
#endif
        client.mark_mode_auto_selected(state, selected_mode);
    }

    return &state;
}

void ClientUpdateRuntime::build_upsert_mode_selector_updates(
    ReplicationClient& client,
    const SyncSettings& settings,
    const UpsertMetadata& metadata,
    const AuthoritativeUpsertRecord& record,
    std::vector<ComponentBaseline>& out) const {
    out.clear();
    const bool mode_needs_selection = metadata.found_state == nullptr ||
        metadata.previous_absent ||
        !metadata.found_state->mode.selected;
    if (!client.options_.entities.mode_selector || !mode_needs_selection) {
        return;
    }

    const SyncArchetype& definition = settings.archetypes[metadata.archetype.value];
    out.reserve(definition.components.size());
    for (std::size_t component_index = 0; component_index < definition.components.size(); ++component_index) {
        if ((record.changed_sync_slots & sync_slot_bit(component_index + 1U)) == 0U ||
            component_index >= definition.component_ops.size()) {
            continue;
        }
        const SyncComponentOps& ops = definition.component_ops[component_index];
        const std::uint8_t* bytes = frame_component_data(definition, record.authoritative, component_index);
        if (bytes == nullptr) {
            continue;
        }
        ComponentBaseline baseline;
        baseline.component = definition.components[component_index].component;
        baseline.bytes.assign(bytes, ops.serialization.quantized_size);
        out.push_back(std::move(baseline));
    }
}

void ClientUpdateRuntime::trace_received_upsert_record(
    ReplicationClient& client,
    const SyncSettings& settings,
    const UpsertMetadata& metadata,
    const AuthoritativeUpsertRecord& record,
    const EntityState& state) {
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    if (client.tracer_ == nullptr || !client.tracer_->enabled()) {
        return;
    }

    const SyncArchetype& definition = settings.archetypes[metadata.archetype.value];
    if (has_tag_slot(definition)) {
        const bool tags_changed = metadata.is_full_upsert || metadata.previous_baseline == nullptr ||
            metadata.previous_baseline->tag_mask != record.authoritative.tag_mask;
        if (tags_changed) {
            for (std::size_t tag_index = 0; tag_index < definition.tags.size(); ++tag_index) {
                SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::TagReceived, client.client_id_, metadata.frame);
                event.server_entity = ashiato::Entity{metadata.client_entity_network_id};
                event.local_entity = state.identity.local;
                event.client_network_id = metadata.client_entity_network_id;
                event.wire_network_id = metadata.wire_network_id;
                event.network_version = client_entity_network_id_version(metadata.client_entity_network_id);
                event.archetype = metadata.archetype;
                event.mode = state.mode.current;
                event.tag = definition.tags[tag_index].tag;
                event.remove = (record.authoritative.tag_mask & (std::uint64_t{1} << tag_index)) == 0U;
                client.tracer_->trace(event);
            }
        }
    }

    for (std::size_t component_index = 0; component_index < definition.components.size(); ++component_index) {
        if ((record.changed_sync_slots & sync_slot_bit(component_index + 1U)) == 0U) {
            continue;
        }
        const std::uint8_t* bytes = frame_component_data(definition, record.authoritative, component_index);
        if (bytes == nullptr) {
            continue;
        }
        SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::ComponentReceived, client.client_id_, metadata.frame);
        event.server_entity = ashiato::Entity{metadata.client_entity_network_id};
        event.local_entity = state.identity.local;
        event.client_network_id = metadata.client_entity_network_id;
        event.wire_network_id = metadata.wire_network_id;
        event.network_version = client_entity_network_id_version(metadata.client_entity_network_id);
        event.archetype = metadata.archetype;
        event.mode = state.mode.current;
        event.component = definition.components[component_index].component;
        append_trace_component_data(client.tracer_, definition, component_index, bytes, event);
        client.tracer_->trace(event);
    }
#else
    (void)client;
    (void)settings;
    (void)metadata;
    (void)record;
    (void)state;
#endif
}

bool ClientUpdateRuntime::apply_upsert_record(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const UpsertMetadata& metadata,
    AuthoritativeUpsertRecord& record) {
    if (record.received_cues == nullptr) {
        return false;
    }
    QuantizedFrameData decoded_delta;
    QuantizedFrameData* decoded = &record.authoritative;
    if (!metadata.is_full_upsert && state.mode.current == ReplicationClientMode::Snap) {
        const SyncArchetype& definition = settings.archetypes[metadata.archetype.value];
        if (!init_frame_data(definition, decoded_delta)) {
            return false;
        }
        decoded_delta.tag_mask = record.authoritative.tag_mask;
        for (std::size_t component_index = 0; component_index < definition.components.size(); ++component_index) {
            if ((record.changed_sync_slots & sync_slot_bit(component_index + 1U)) == 0U ||
                component_index >= definition.component_ops.size()) {
                continue;
            }
            const SyncComponentOps& ops = definition.component_ops[component_index];
            const std::uint8_t* authoritative_bytes =
                frame_component_data(definition, record.authoritative, component_index);
            std::uint8_t* decoded_bytes = mutable_frame_component_data(definition, decoded_delta, component_index);
            if (authoritative_bytes == nullptr || decoded_bytes == nullptr) {
                return false;
            }
            std::memcpy(decoded_bytes, authoritative_bytes, ops.serialization.quantized_size);
        }
        decoded = &decoded_delta;
    }
    UpsertModeApplyContext mode_context{
        client.entity_store_->index_of(state),
        metadata.frame,
        metadata.client_entity_network_id,
        metadata.archetype,
        record.authoritative,
        *decoded,
        metadata.is_full_upsert,
        *record.received_cues};
    return apply_upsert_for_mode(client, registry, settings, state, mode_context);
}

void ClientUpdateRuntime::finish_upsert(ReplicationClient& client, const UpsertMetadata& metadata) {
    if (metadata.is_full_upsert) {
        client.entity_store_->erase_destroy_tombstone(metadata.wire_network_id);
    }
}

bool ClientUpdateRuntime::apply_destroy(
    ReplicationClient& client,
    ashiato::Registry& registry,
    SyncFrame frame,
    std::uint32_t wire_network_id) {
    EntityState* state = client.find_entity_state_by_wire_id(wire_network_id);
    if (state == nullptr) {
        const auto result = client.entity_store_->record_destroy_tombstone(wire_network_id, frame);
        if (result == ClientEntityStore::DestroyTombstoneRecordResult::Ignored) {
            return false;
        }
        if (result == ClientEntityStore::DestroyTombstoneRecordResult::Inserted) {
            client.advance_wire_network_id_version(wire_network_id);
        }
        return true;
    }
    const ClientEntityNetworkId client_entity_network_id = state->identity.client_entity_network_id;
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    const ashiato::Entity local_entity = state->identity.local;
    const SyncArchetypeId archetype = state->identity.archetype;
    auto trace_destroy = [&]() {
        if (client.tracer_ == nullptr || !client.tracer_->enabled()) {
            return;
        }
        SyncTraceEvent event = make_client_trace_event(SyncTraceEventType::EntityDestroyed, client.client_id_, frame);
        event.server_entity = ashiato::Entity{client_entity_network_id};
        event.local_entity = local_entity;
        event.client_network_id = client_entity_network_id;
        event.wire_network_id = wire_network_id;
        event.network_version = client_entity_network_id_version(client_entity_network_id);
        event.archetype = archetype;
        event.remove = true;
        client.tracer_->trace(event);
    };
#endif
    if (!apply_destroy_for_mode(client, registry, frame, *state, client_entity_network_id)) {
        return false;
    }
    client.record_destroy_tombstone(wire_network_id, frame);
    client.advance_wire_network_id_version(wire_network_id);
#ifdef ASHIATO_SYNC_ENABLE_TRACING
    trace_destroy();
#endif
    return true;
}

bool ClientUpdateRuntime::apply_destroy_for_mode(
    ReplicationClient& client,
    ashiato::Registry& registry,
    SyncFrame frame,
    EntityState& state,
    ClientEntityNetworkId client_entity_network_id) {
    switch (state.mode.current) {
    case ReplicationClientMode::BufferedInterpolation:
        return apply_buffered_destroy(client, registry, frame, client_entity_network_id);
    case ReplicationClientMode::Predict:
        return apply_predicted_destroy(client, registry, frame, client_entity_network_id);
    case ReplicationClientMode::Snap:
        return apply_snap_destroy(client, registry, frame, state, client_entity_network_id);
    }
    return false;
}

bool ClientUpdateRuntime::apply_snap_destroy(
    ReplicationClient& client,
    ashiato::Registry& registry,
    SyncFrame frame,
    EntityState& state,
    ClientEntityNetworkId client_entity_network_id) {
    if (frame <= state.replication.frame) {
        return false;
    }
    const std::uint32_t entity_index =
        client.entity_store_->entity_index_for_client_entity_network_id(client_entity_network_id);
    if (entity_index != ReplicationClient::invalid_entity_index) {
        client.erase_entity_state(registry, entity_index, true);
    }
    return true;
}

bool ClientUpdateRuntime::apply_upsert_for_mode(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const UpsertModeApplyContext& context) {
    switch (state.mode.current) {
    case ReplicationClientMode::BufferedInterpolation:
        if (!apply_buffered_upsert(
                client,
                registry,
                settings,
                context.frame,
                context.client_entity_network_id,
                context.archetype,
                context.authoritative)) {
            return false;
        }
        client.cue_runtime_->store_authoritative_buffered(
            client,
            registry,
            settings,
            context.entity_index,
            state,
            context.frame,
            context.received_cues);
        return true;
    case ReplicationClientMode::Predict:
        if (!apply_predicted_upsert(
                client,
                registry,
                settings,
                context.frame,
                context.client_entity_network_id,
                context.archetype,
                context.authoritative,
                context.full)) {
            return false;
        }
        client.cue_runtime_->reconcile_authoritative_predicted(
            client,
            registry,
            settings,
            context.entity_index,
            state,
            context.received_cues,
            context.frame);
        return true;
    case ReplicationClientMode::Snap:
        return apply_snap_upsert(client, registry, settings, state, context);
    }
    return false;
}

bool ClientUpdateRuntime::apply_snap_upsert(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    EntityState& state,
    const UpsertModeApplyContext& context) {
    if (context.full && state.identity.local && registry.alive(state.identity.local) &&
        state.identity.archetype != context.archetype &&
        state.identity.archetype.value < settings.archetypes.size()) {
        remove_archetype_tags(registry, state.identity.local, settings.archetypes[state.identity.archetype.value]);
    }
    state.identity.archetype = context.archetype;
    if (!client.apply_snap_sample(registry, settings, state, context.decoded, context.full)) {
        return false;
    }
    client.cue_runtime_->play_snap(client, registry, settings, state, context.received_cues);
    client.record_authoritative_present(
        state,
        context.frame,
        context.archetype,
        std::move(context.authoritative),
        context.full
            ? ReplicationClient::BaselineUpdate::KeepExisting
            : ReplicationClient::BaselineUpdate::ReplaceAndApplyMask);
    return true;
}

bool ClientUpdateRuntime::apply_buffered_upsert(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    ClientEntityNetworkId client_entity_network_id,
    SyncArchetypeId archetype,
    QuantizedFrameData& decoded) {
    if (!client.validate_buffered_archetype(settings, archetype)) {
        return false;
    }

    EntityState* ensured_state = client.find_entity_state(client_entity_network_id);
    if (ensured_state == nullptr) {
        return false;
    }
    EntityState& state = *ensured_state;
    client.mark_mode_auto_selected(state, ReplicationClientMode::BufferedInterpolation);
    state.identity.archetype = archetype;
    state.visual.snap_errors.clear();
    const std::uint32_t entity_index = client.entity_store_->index_of(state);
    client.buffered_runtime_->ensure_entity(entity_index);

    if (!client.fill_buffered_frames(settings, state, frame, true, decoded)) {
        return false;
    }

    client.record_authoritative_present(
        state,
        frame,
        archetype,
        decoded,
        ReplicationClient::BaselineUpdate::ReplaceAndApplyMask);
    if (client.buffered_runtime_->has_applied_frame() && frame <= client.buffered_runtime_->last_applied_frame()) {
        EntityFrameView sample;
        if (!client.buffered_runtime_->frames().view(entity_index, frame, sample) ||
            !client.apply_buffered_sample(registry, settings, state, sample)) {
            return false;
        }
    }
    return true;
}

bool ClientUpdateRuntime::apply_buffered_destroy(
    ReplicationClient& client,
    ashiato::Registry& registry,
    SyncFrame frame,
    ClientEntityNetworkId client_entity_network_id) {
    EntityState* state_ptr = client.find_entity_state(client_entity_network_id);
    if (state_ptr == nullptr) {
        return false;
    }
    EntityState& state = *state_ptr;
    client.mark_mode_auto_selected(state, ReplicationClientMode::BufferedInterpolation);
    state.visual.snap_errors.clear();
    const std::uint32_t entity_index = client.entity_store_->index_of(state);
    client.buffered_runtime_->ensure_entity(entity_index);
    QuantizedFrameData empty;
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (state.identity.archetype.value < settings.archetypes.size()) {
        (void)init_frame_data(settings.archetypes[state.identity.archetype.value], empty);
    }
    if (!client.fill_buffered_frames(settings, state, frame, false, empty)) {
        return false;
    }
    client.record_authoritative_absent(state, frame);
    if (client.buffered_runtime_->has_applied_frame() && frame <= client.buffered_runtime_->last_applied_frame()) {
        EntityFrameView sample;
        if (!client.buffered_runtime_->frames().view(entity_index, frame, sample) ||
            !client.apply_buffered_sample(registry, settings, state, sample)) {
            return false;
        }
        client.erase_entity_state(registry, entity_index, false);
        return true;
    }
    return true;
}

bool ClientUpdateRuntime::apply_predicted_upsert(
    ReplicationClient& client,
    ashiato::Registry& registry,
    const SyncSettings& settings,
    SyncFrame frame,
    ClientEntityNetworkId client_entity_network_id,
    SyncArchetypeId archetype,
    QuantizedFrameData& authoritative,
    bool full) {
    (void)full;
    if (!client.validate_predicted_archetype(settings, archetype)) {
        return false;
    }

    EntityState* state_ptr = client.find_entity_state(client_entity_network_id);
    if (state_ptr == nullptr) {
        return false;
    }
    EntityState& state = *state_ptr;
    const std::uint32_t entity_index = client.entity_store_->index_of(state);
    client.prediction_->ensure_entity(entity_index);
    if (state.identity.archetype.value < settings.archetypes.size() &&
        state.identity.local && registry.alive(state.identity.local) && state.identity.archetype != archetype) {
        return false;
    }
    client.mark_mode_auto_selected(state, ReplicationClientMode::Predict);
    state.identity.archetype = archetype;

    const bool first_authoritative = state.replication.frame == 0 || !state.identity.local || !registry.alive(state.identity.local);
    const bool has_prediction = client.prediction_->frames().contains(entity_index, frame);
    if (has_prediction && client.compare_predicted_frame(settings, state, frame, authoritative)) {
        client.prediction_->queue_rollback(client, state, frame);
    }

    client.record_authoritative_present(
        state,
        frame,
        archetype,
        authoritative,
        ReplicationClient::BaselineUpdate::ReplaceAndApplyMask);

    if (first_authoritative) {
        if (!client.apply_frame_data(registry, settings, state, frame, true, authoritative)) {
            return false;
        }
    }
    if (!client.prediction_->has_predicted_frame()) {
        if (!client.prediction_->seed_first_authoritative_frame(client, registry, settings, frame)) {
            return false;
        }
    }
    return true;
}

bool ClientUpdateRuntime::apply_predicted_destroy(
    ReplicationClient& client,
    ashiato::Registry& registry,
    SyncFrame frame,
    ClientEntityNetworkId client_entity_network_id) {
    EntityState* state_ptr = client.find_entity_state(client_entity_network_id);
    if (state_ptr == nullptr) {
        return false;
    }
    EntityState& state = *state_ptr;
    if (frame <= state.replication.frame) {
        return false;
    }
    const std::uint32_t entity_index = client.entity_store_->index_of(*state_ptr);
    client.erase_entity_state(registry, entity_index, true);
    return true;
}

}  // namespace ashiato::sync::client_detail
