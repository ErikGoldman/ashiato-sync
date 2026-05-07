#include "detail/frame_data.hpp"

namespace kage::sync::detail {

bool init_frame_data(const SyncArchetype& archetype, QuantizedFrameData& frame) {
    if (archetype.tags.size() > 64U ||
        archetype.components.size() > 63U ||
        archetype.component_offsets.size() != archetype.components.size() ||
        archetype.component_ops.size() != archetype.components.size()) {
        return false;
    }
    frame.tag_mask = 0;
    frame.present_mask = 0;
    frame.bytes.assign(archetype.total_quantized_bytes, 0U);
    return true;
}

std::size_t sync_slot_count(const SyncArchetype& archetype) noexcept {
    return archetype.components.size() + 1U;
}

bool has_tag_slot(const SyncArchetype& archetype) noexcept {
    return !archetype.tags.empty();
}

std::uint64_t sync_slot_bit(std::size_t slot) noexcept {
    return std::uint64_t{1} << slot;
}

bool frame_has_component(const QuantizedFrameData& frame, std::size_t component_index) {
    return component_index < 64U && (frame.present_mask & (std::uint64_t{1} << component_index)) != 0U;
}

const std::uint8_t* frame_component_data(
    const SyncArchetype& archetype,
    const QuantizedFrameData& frame,
    std::size_t component_index) {
    if (!frame_has_component(frame, component_index) ||
        component_index >= archetype.component_offsets.size() ||
        component_index >= archetype.component_ops.size()) {
        return nullptr;
    }
    const std::size_t offset = archetype.component_offsets[component_index];
    const std::size_t size = archetype.component_ops[component_index].serialization.quantized_size;
    if (offset + size > frame.bytes.size()) {
        return nullptr;
    }
    return frame.bytes.data() + offset;
}

const std::uint8_t* unchecked_frame_component_data(
    const SyncArchetype& archetype,
    const QuantizedFrameData& frame,
    std::size_t component_index) noexcept {
    return frame.bytes.data() + archetype.component_offsets[component_index];
}

std::uint8_t* mutable_frame_component_data(
    const SyncArchetype& archetype,
    QuantizedFrameData& frame,
    std::size_t component_index) {
    if (component_index >= 64U ||
        component_index >= archetype.component_offsets.size() ||
        component_index >= archetype.component_ops.size()) {
        return nullptr;
    }
    const std::size_t offset = archetype.component_offsets[component_index];
    const std::size_t size = archetype.component_ops[component_index].serialization.quantized_size;
    if (offset + size > frame.bytes.size()) {
        return nullptr;
    }
    frame.present_mask |= (std::uint64_t{1} << component_index);
    return frame.bytes.data() + offset;
}

std::uint8_t* unchecked_mutable_frame_component_data(
    const SyncArchetype& archetype,
    QuantizedFrameData& frame,
    std::size_t component_index) noexcept {
    frame.present_mask |= (std::uint64_t{1} << component_index);
    return frame.bytes.data() + archetype.component_offsets[component_index];
}

bool tag_bit_set(std::uint64_t tag_mask, std::size_t tag_index) noexcept {
    return tag_index < 64U && (tag_mask & (std::uint64_t{1} << tag_index)) != 0U;
}

bool apply_archetype_tags(
    ecs::Registry& registry,
    ecs::Entity entity,
    const SyncArchetype& archetype,
    std::uint64_t tag_mask) {
    for (std::size_t tag_index = 0; tag_index < archetype.tags.size(); ++tag_index) {
        const ecs::Entity tag = archetype.tags[tag_index].tag;
        if (tag_bit_set(tag_mask, tag_index)) {
            if (!registry.add_tag(entity, tag)) {
                return false;
            }
        } else {
            registry.remove_tag(entity, tag);
        }
    }
    return true;
}

void remove_archetype_tags(ecs::Registry& registry, ecs::Entity entity, const SyncArchetype& archetype) {
    for (const SyncTagReplication& replication : archetype.tags) {
        registry.remove_tag(entity, replication.tag);
    }
}

}  // namespace kage::sync::detail
