#pragma once

#include "ashiato/sync/types.hpp"

#include <string>
#include <vector>

namespace ashiato::sync {

void register_components(ashiato::Registry& registry);

const SyncComponentOps* find_component_ops(const ashiato::Registry& registry, ashiato::Entity component);
bool set_fractional_tick_sampled(ashiato::Registry& registry, ashiato::Entity component, bool enabled = true);
bool is_fractional_tick_sampled(const ashiato::Registry& registry, ashiato::Entity component);

template <typename T>
bool set_fractional_tick_sampled(ashiato::Registry& registry, bool enabled = true) {
    return set_fractional_tick_sampled(registry, registry.component<T>(), enabled);
}

template <typename T>
bool is_fractional_tick_sampled(const ashiato::Registry& registry) {
    return is_fractional_tick_sampled(registry, registry.component<T>());
}

SyncArchetypeId define_archetype(ashiato::Registry& registry, SyncArchetypeDesc desc);
SyncArchetypeId define_archetype(
    ashiato::Registry& registry,
    std::string name,
    std::vector<ComponentReplication> components);

const SyncArchetype* find_archetype(const ashiato::Registry& registry, SyncArchetypeId id);

bool set_owner(ashiato::Registry& registry, ashiato::Entity entity, ClientId client);
bool set_client_input_component(ashiato::Registry& registry, ashiato::Entity component);

template <typename T>
bool set_client_input_component(ashiato::Registry& registry) {
    register_components(registry);
    return set_client_input_component(registry, registry.component<T>());
}

}  // namespace ashiato::sync
