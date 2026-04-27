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
}

const SyncComponentOps* find_component_ops(const ecs::Registry& registry, ecs::Entity component) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    const auto found = settings.component_ops.find(component.value);
    return found != settings.component_ops.end() ? &found->second : nullptr;
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

bool mark_replicated(ecs::Registry& registry, ecs::Entity entity, SyncArchetypeId archetype) {
    register_components(registry);

    if (!registry.alive(entity)) {
        return false;
    }

    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!valid_archetype_id(settings, archetype)) {
        return false;
    }

    return registry.add<Replicated>(entity, Replicated{archetype}) != nullptr;
}

bool unmark_replicated(ecs::Registry& registry, ecs::Entity entity) {
    register_components(registry);
    return registry.remove<Replicated>(entity);
}

bool set_owner(ecs::Registry& registry, ecs::Entity entity, ClientId client) {
    register_components(registry);

    if (!registry.alive(entity)) {
        return false;
    }

    return registry.add<NetworkOwner>(entity, NetworkOwner{client}) != nullptr;
}

}  // namespace kage::sync
