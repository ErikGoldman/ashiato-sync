#pragma once

#include "client/frame_data.hpp"
#include "client/state.hpp"

#include <algorithm>

namespace kage::sync::client_detail {

inline bool valid_frame_archetype(const SyncArchetype& archetype) noexcept {
    return archetype.tags.size() <= 64U &&
        archetype.components.size() <= 63U &&
        archetype.component_offsets.size() == archetype.components.size() &&
        archetype.component_ops.size() == archetype.components.size();
}

template <typename FrameData, typename Fn>
bool for_each_present_component(const SyncArchetype& archetype, const FrameData& frame, Fn&& fn) {
    for (std::size_t component_index = 0; component_index < archetype.components.size(); ++component_index) {
        if (!frame_has_component(frame, component_index)) {
            continue;
        }
        if (component_index >= archetype.component_ops.size()) {
            return false;
        }
        const SyncComponentOps& ops = archetype.component_ops[component_index];
        const std::uint8_t* bytes = frame_component_data(archetype, frame, component_index);
        if (bytes == nullptr) {
            return false;
        }
        if (!fn(component_index, archetype.components[component_index], ops, bytes)) {
            return false;
        }
    }
    return true;
}

inline std::vector<EntityComponentError>::iterator find_snap_error(
    EntityState& state,
    ecs::Entity component) {
    return std::find_if(
        state.visual.snap_errors.begin(),
        state.visual.snap_errors.end(),
        [component](const EntityComponentError& existing) {
            return existing.component == component;
        });
}

inline std::vector<EntityComponentError>::const_iterator find_snap_error(
    const EntityState& state,
    ecs::Entity component) {
    return std::find_if(
        state.visual.snap_errors.begin(),
        state.visual.snap_errors.end(),
        [component](const EntityComponentError& existing) {
            return existing.component == component;
        });
}

}  // namespace kage::sync::client_detail
