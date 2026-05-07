#pragma once

#include "kage/sync/server.hpp"
#include "kage/sync/server_frame_consumer.hpp"

#include "server/server_client_replicator.hpp"

namespace kage::sync::server_detail {

template <typename MarkSlotFn>
void each_dirty_replicated_slot(
    const ServerRegistryDirtyFrame& frame,
    const SyncSettings& settings,
    MarkSlotFn&& mark_slot) {
    auto mark_entity = [&](ecs::Entity entity) {
        const std::uint32_t slot = frame.server.replicated_slot_for_entity(entity);
        if (slot != invalid_quantized_frame_id) {
            mark_slot(slot);
        }
    };
    auto mark_entity_index = [&](std::uint32_t entity_index) {
        const std::uint32_t slot = frame.server.replicated_slot_for_entity_index(entity_index);
        if (slot != invalid_quantized_frame_id) {
            mark_slot(slot);
        }
    };

    frame.dirty.each_dirty<Replicated>([&](ecs::Entity entity, const void*) {
        mark_entity(entity);
    });
    frame.dirty.each_removed<Replicated>([&](ecs::Registry::ComponentRemoval removal) {
        mark_entity_index(removal.entity_index);
    });

    for (const SyncArchetype& archetype : settings.archetypes) {
        for (const SyncTagReplication& tag_replication : archetype.tags) {
            frame.dirty.each_dirty(tag_replication.tag, [&](ecs::Entity entity, const void*) {
                mark_entity(entity);
            });
            frame.dirty.each_removed(tag_replication.tag, [&](ecs::Registry::ComponentRemoval removal) {
                mark_entity_index(removal.entity_index);
            });
        }
    }

    for (const auto& component_ops : settings.component_ops) {
        const ecs::Entity component{component_ops.first};
        frame.dirty.each_dirty(component, [&](ecs::Entity entity, const void*) {
            mark_entity(entity);
        });
        frame.dirty.each_removed(component, [&](ecs::Registry::ComponentRemoval removal) {
            mark_entity_index(removal.entity_index);
        });
    }

    frame.dirty.each_dirty<NetworkOwner>([&](ecs::Entity entity, const void*) {
        mark_entity(entity);
    });
    frame.dirty.each_removed<NetworkOwner>([&](ecs::Registry::ComponentRemoval removal) {
        mark_entity_index(removal.entity_index);
    });
}

}  // namespace kage::sync::server_detail
