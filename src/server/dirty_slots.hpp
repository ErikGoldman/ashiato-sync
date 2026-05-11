#pragma once

#include "ashiato/sync/server.hpp"
#include "ashiato/sync/server_frame_consumer.hpp"

#include "server/server_client_replicator.hpp"

namespace ashiato::sync::server_detail {

template <typename MarkSlotFn>
void each_dirty_replicated_slot(
    const ServerRegistryDirtyFrame& frame,
    const SyncSettings& settings,
    MarkSlotFn&& mark_slot) {
    auto mark_entity = [&](ashiato::Entity entity) {
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

    frame.dirty.each_dirty<Replicated>([&](ashiato::Entity entity, const void*) {
        mark_entity(entity);
    });
    frame.dirty.each_removed<Replicated>([&](ashiato::Registry::ComponentRemoval removal) {
        mark_entity_index(removal.entity_index);
    });

    for (const SyncArchetype& archetype : settings.archetypes) {
        for (const SyncTagReplication& tag_replication : archetype.tags) {
            frame.dirty.each_dirty(tag_replication.tag, [&](ashiato::Entity entity, const void*) {
                mark_entity(entity);
            });
            frame.dirty.each_removed(tag_replication.tag, [&](ashiato::Registry::ComponentRemoval removal) {
                mark_entity_index(removal.entity_index);
            });
        }
    }

    for (const auto& component_ops : settings.component_ops) {
        const ashiato::Entity component{component_ops.first};
        frame.dirty.each_dirty(component, [&](ashiato::Entity entity, const void*) {
            mark_entity(entity);
        });
        frame.dirty.each_removed(component, [&](ashiato::Registry::ComponentRemoval removal) {
            mark_entity_index(removal.entity_index);
        });
    }

    frame.dirty.each_dirty<NetworkOwner>([&](ashiato::Entity entity, const void*) {
        mark_entity(entity);
    });
    frame.dirty.each_removed<NetworkOwner>([&](ashiato::Registry::ComponentRemoval removal) {
        mark_entity_index(removal.entity_index);
    });
}

}  // namespace ashiato::sync::server_detail
