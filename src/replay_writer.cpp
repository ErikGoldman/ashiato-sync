#include "ashiato/sync/replay_writer.hpp"

#include "detail/frame_data.hpp"

#include "ashiato/sync/components.hpp"
#include "ashiato/sync/server.hpp"

#include "server/dirty_slots.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
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
    std::vector<ashiato::BitBuffer>& out) {
    const std::size_t count = std::min<std::size_t>(cues.size, std::numeric_limits<std::uint16_t>::max());
    for (std::size_t index = 0; index < count; ++index) {
        const QueuedSyncCue& cue = cues.data[index];
        if (cue.type >= settings.cue_ops.size() || settings.cue_ops[cue.type].serialize == nullptr) {
            continue;
        }
        ashiato::BitBuffer payload = cue.payload;
        if (settings.cue_ops[cue.type].references_entities) {
            if (!cue.value) {
                continue;
            }
            payload.clear();
            settings.cue_ops[cue.type].serialize(cue.value.get(), payload, &reference_context);
        }
        const std::uint32_t slot = server.replicated_slot_for_entity(cue.entity);
        ashiato::BitBuffer entry;
        entry.push_bits(
            slot == invalid_replay_slot ? 0 : replay_id_for_slot(slot),
            32U);
        entry.push_bits(cue.frame, 32U);
        entry.push_bits(cue.type, 16U);
        entry.push_bytes(reinterpret_cast<const char*>(&cue.relevance_seconds), sizeof(cue.relevance_seconds));
        entry.push_bool(cue.only_replicate_to_owner);
        entry.push_bits(static_cast<std::int64_t>(std::min<std::size_t>(
                          payload.bit_size(),
                          std::numeric_limits<std::uint16_t>::max())),
            16U);
        entry.push_buffer_bits(payload);
        out.push_back(std::move(entry));
    }
}

void write_cue_entries(const std::vector<ashiato::BitBuffer>& cues, ashiato::BitBuffer& out) {
    const std::size_t count = std::min<std::size_t>(cues.size(), std::numeric_limits<std::uint16_t>::max());
    out.push_bool(count != 0U);
    if (count == 0U) {
        return;
    }
    out.push_bits(static_cast<std::int64_t>(count), 16U);
    for (std::size_t index = 0; index < count; ++index) {
        out.push_buffer_bits(cues[index]);
    }
}

}  // namespace

struct ReplicationReplayWriter::State {
    std::vector<std::uint8_t> pending_dirty_slots;
    std::vector<std::uint32_t> slots;
    std::vector<std::uint32_t> pending_slots;
    std::vector<ServerDestroyedReplicatedSlot> pending_destroyed_slots;
    std::vector<ashiato::BitBuffer> pending_cues;
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

    append_cue_entries(cues, settings, server, reference_context, state_->pending_cues);

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
    const std::size_t record_count_offset = payload.bit_size();
    payload.push_bits(0, 16U);
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
            payload.push_bool(true);
            payload.push_bits(replay_id_for_slot(destroyed.slot), 32U);
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
        payload.push_bool(!active);
        payload.push_bits(replay_id_for_slot(slot), 32U);
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
        payload.push_bits(archetype_id.value, 32U);
        payload.push_unsigned_bits(state_->scratch.tag_mask, 64U);
        payload.push_unsigned_bits(state_->scratch.present_mask, 64U);

        const std::size_t component_count_offset = payload.bit_size();
        payload.push_bits(0, 16U);
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
            ashiato::BitBuffer component_payload;
            ashiato::ComponentSerializationContext serialization_context{ops.references_entities ? &reference_context : nullptr};
            ops.serialization.serialize(
                nullptr,
                current,
                component_payload,
                ops.references_entities ? &serialization_context : nullptr);
            payload.push_bits(static_cast<std::int64_t>(component_index), 16U);
            payload.push_bits(static_cast<std::int64_t>(std::min<std::size_t>(
                                  component_payload.bit_size(),
                                  std::numeric_limits<std::uint16_t>::max())),
                16U);
            payload.push_buffer_bits(component_payload);
            ++component_count;
        }
        payload.overwrite_unsigned_bits(component_count_offset, component_count, 16U);
        ++record_count;
    }

    payload.overwrite_unsigned_bits(record_count_offset, record_count, 16U);
    write_cue_entries(state_->pending_cues, payload);

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
