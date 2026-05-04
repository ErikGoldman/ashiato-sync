#pragma once

#include "kage/sync/types.hpp"

#include <string>
#include <vector>

namespace kage::sync {

void register_components(ecs::Registry& registry);

const SyncComponentOps* find_component_ops(const ecs::Registry& registry, ecs::Entity component);
bool set_display_interpolated(ecs::Registry& registry, ecs::Entity component, bool enabled = true);
bool is_display_interpolated(const ecs::Registry& registry, ecs::Entity component);

template <typename T>
bool set_display_interpolated(ecs::Registry& registry, bool enabled = true) {
    return set_display_interpolated(registry, registry.component<T>(), enabled);
}

template <typename T>
bool is_display_interpolated(const ecs::Registry& registry) {
    return is_display_interpolated(registry, registry.component<T>());
}

void configure_server(ecs::Registry& registry);
void configure_client(ecs::Registry& registry, ClientId local_client);

SyncArchetypeId define_archetype(ecs::Registry& registry, SyncArchetypeDesc desc);
SyncArchetypeId define_archetype(
    ecs::Registry& registry,
    std::string name,
    std::vector<ComponentReplication> components);

const SyncArchetype* find_archetype(const ecs::Registry& registry, SyncArchetypeId id);

bool set_owner(ecs::Registry& registry, ecs::Entity entity, ClientId client);
bool set_client_input_component(ecs::Registry& registry, ecs::Entity component);

template <typename T>
bool set_client_input_component(ecs::Registry& registry) {
    register_components(registry);
    return set_client_input_component(registry, registry.component<T>());
}

}  // namespace kage::sync
