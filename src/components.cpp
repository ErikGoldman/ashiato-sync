#include "kage/sync/component_traits.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace kage::sync {
namespace {

bool valid_archetype_id(const SyncSettings& settings, SyncArchetypeId id) {
    return id.value < settings.archetypes.size();
}

void validate_component_replication(const ecs::Registry& registry, const ComponentReplication& replication) {
    if (!replication.component || registry.component_info(replication.component) == nullptr) {
        throw std::invalid_argument("sync archetype references an unregistered component");
    }
    if (find_component_ops(registry, replication.component) == nullptr) {
        throw std::invalid_argument("sync archetype references a component without sync traits");
    }
}

}  // namespace

void register_components(ecs::Registry& registry) {
    registry.register_component<SyncSettings>("kage.sync.SyncSettings");
    registry.register_component<Replicated>("kage.sync.Replicated");
    registry.register_component<NetworkOwner>("kage.sync.NetworkOwner");
    registry.register_component<DisplayInterpolated>("kage.sync.DisplayInterpolated");
}

const SyncComponentOps* find_component_ops(const ecs::Registry& registry, ecs::Entity component) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    const auto found = settings.component_ops.find(component.value);
    return found != settings.component_ops.end() ? &found->second : nullptr;
}

bool set_display_interpolated(ecs::Registry& registry, ecs::Entity component, bool enabled) {
    register_components(registry);
    if (!component || registry.component_info(component) == nullptr) {
        return false;
    }

    const ecs::Entity tag = registry.component<DisplayInterpolated>();
    if (!enabled) {
        if (!registry.has<DisplayInterpolated>(component)) {
            return true;
        }
        return registry.remove_tag(component, tag);
    }

    const SyncComponentOps* ops = find_component_ops(registry, component);
    if (ops == nullptr || ops->interpolate == nullptr) {
        return false;
    }

    return registry.add_tag(component, tag);
}

bool is_display_interpolated(const ecs::Registry& registry, ecs::Entity component) {
    if (!component || registry.component_info(component) == nullptr) {
        return false;
    }
    return registry.has<DisplayInterpolated>(component);
}

void configure_server(ecs::Registry& registry) {
    register_components(registry);

    SyncSettings& settings = registry.write<SyncSettings>();
    settings.role = SyncRole::Server;
    settings.local_client = invalid_client_id;
}

void configure_client(ecs::Registry& registry, ClientId local_client) {
    register_components(registry);

    SyncSettings& settings = registry.write<SyncSettings>();
    settings.role = SyncRole::Client;
    settings.local_client = local_client;
}

SyncArchetypeId define_archetype(
    ecs::Registry& registry,
    std::string name,
    std::vector<ComponentReplication> components) {
    register_components(registry);

    for (const ComponentReplication& replication : components) {
        validate_component_replication(registry, replication);
    }

    SyncSettings& settings = registry.write<SyncSettings>();
    const SyncArchetypeId id{static_cast<std::uint32_t>(settings.archetypes.size())};
    settings.archetypes.push_back(SyncArchetype{std::move(name), std::move(components)});
    return id;
}

const SyncArchetype* find_archetype(const ecs::Registry& registry, SyncArchetypeId id) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!valid_archetype_id(settings, id)) {
        return nullptr;
    }

    return &settings.archetypes[id.value];
}

bool set_owner(ecs::Registry& registry, ecs::Entity entity, ClientId client) {
    register_components(registry);

    if (!registry.alive(entity)) {
        return false;
    }

    return registry.add<NetworkOwner>(entity, NetworkOwner{client}) != nullptr;
}

}  // namespace kage::sync
