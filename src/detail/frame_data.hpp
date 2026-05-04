#pragma once

#include "kage/sync/types.hpp"

#include <cstddef>
#include <cstdint>

namespace kage::sync::detail {

bool init_frame_data(const SyncArchetype& archetype, QuantizedFrameData& frame);
std::size_t sync_slot_count(const SyncArchetype& archetype) noexcept;
bool has_tag_slot(const SyncArchetype& archetype) noexcept;
std::uint64_t sync_slot_bit(std::size_t slot) noexcept;
bool frame_has_component(const QuantizedFrameData& frame, std::size_t component_index);
const std::uint8_t* frame_component_data(
    const SyncArchetype& archetype,
    const QuantizedFrameData& frame,
    std::size_t component_index);
const std::uint8_t* unchecked_frame_component_data(
    const SyncArchetype& archetype,
    const QuantizedFrameData& frame,
    std::size_t component_index) noexcept;
std::uint8_t* mutable_frame_component_data(
    const SyncArchetype& archetype,
    QuantizedFrameData& frame,
    std::size_t component_index);
std::uint8_t* unchecked_mutable_frame_component_data(
    const SyncArchetype& archetype,
    QuantizedFrameData& frame,
    std::size_t component_index) noexcept;
bool tag_bit_set(std::uint64_t tag_mask, std::size_t tag_index) noexcept;
bool apply_archetype_tags(
    ecs::Registry& registry,
    ecs::Entity entity,
    const SyncArchetype& archetype,
    std::uint64_t tag_mask);
void remove_archetype_tags(ecs::Registry& registry, ecs::Entity entity, const SyncArchetype& archetype);

}  // namespace kage::sync::detail
