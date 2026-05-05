#include "kage/sync/components.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace kage::sync {
namespace {

constexpr std::size_t max_archetype_tags = 64;
constexpr std::size_t max_archetype_components = 63;
constexpr std::size_t max_archetype_quantized_bytes = 1200;

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

void validate_tag_replication(const ecs::Registry& registry, const SyncTagReplication& replication) {
    const ecs::ComponentInfo* info = registry.component_info(replication.tag);
    if (!replication.tag || info == nullptr || !info->tag) {
        throw std::invalid_argument("sync archetype references an unregistered tag");
    }
}

}  // namespace

void register_components(ecs::Registry& registry) {
    registry.register_component<SyncSettings>("kage.sync.SyncSettings");
    registry.register_component<SyncAuthority>("kage.sync.SyncAuthority");
    registry.register_component<Replicated>("kage.sync.Replicated");
    registry.register_component<NetworkOwner>("kage.sync.NetworkOwner");
    registry.register_component<FractionalTickSampled>("kage.sync.FractionalTickSampled");
    registry.register_component<NoResim>("kage.sync.NoResim");
    registry.register_component<NoSimulate>("kage.sync.NoSimulate");
}

const SyncComponentOps* find_component_ops(const ecs::Registry& registry, ecs::Entity component) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    const auto found = settings.component_ops.find(component.value);
    return found != settings.component_ops.end() ? &found->second : nullptr;
}

bool set_fractional_tick_sampled(ecs::Registry& registry, ecs::Entity component, bool enabled) {
    register_components(registry);
    if (!component || registry.component_info(component) == nullptr) {
        return false;
    }

    const ecs::Entity tag = registry.component<FractionalTickSampled>();
    if (!enabled) {
        if (!registry.has<FractionalTickSampled>(component)) {
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

bool is_fractional_tick_sampled(const ecs::Registry& registry, ecs::Entity component) {
    if (!component || registry.component_info(component) == nullptr) {
        return false;
    }
    return registry.has<FractionalTickSampled>(component);
}

void configure_server(ecs::Registry& registry) {
    register_components(registry);

    SyncSettings& settings = registry.write<SyncSettings>();
    settings.role = SyncRole::Server;
    settings.local_client = invalid_client_id;
    registry.write<SyncAuthority>().authoritative = true;
}

void configure_client(ecs::Registry& registry, ClientId local_client) {
    register_components(registry);
    if (local_client > max_client_entity_network_id_client) {
        throw std::invalid_argument("client id exceeds ClientEntityNetworkId client field");
    }

    SyncSettings& settings = registry.write<SyncSettings>();
    settings.role = SyncRole::Client;
    settings.local_client = local_client;
    registry.write<SyncAuthority>().authoritative = false;
}

SyncArchetypeId define_archetype(ecs::Registry& registry, SyncArchetypeDesc desc) {
    register_components(registry);

    if (desc.tags.size() > max_archetype_tags) {
        throw std::invalid_argument("sync archetypes may not contain more than 64 tags");
    }
    if (desc.components.size() > max_archetype_components) {
        throw std::invalid_argument("sync archetypes may not contain more than 63 components");
    }

    for (const SyncTagReplication& replication : desc.tags) {
        validate_tag_replication(registry, replication);
    }
    for (std::size_t lhs = 0; lhs < desc.tags.size(); ++lhs) {
        for (std::size_t rhs = lhs + 1U; rhs < desc.tags.size(); ++rhs) {
            if (desc.tags[lhs].tag == desc.tags[rhs].tag) {
                throw std::invalid_argument("sync archetype contains duplicate tags");
            }
        }
    }

    for (const ComponentReplication& replication : desc.components) {
        validate_component_replication(registry, replication);
    }
    for (std::size_t lhs = 0; lhs < desc.components.size(); ++lhs) {
        for (std::size_t rhs = lhs + 1U; rhs < desc.components.size(); ++rhs) {
            if (desc.components[lhs].component == desc.components[rhs].component) {
                throw std::invalid_argument("sync archetype contains duplicate components");
            }
        }
    }

    SyncSettings& settings = registry.write<SyncSettings>();
    std::vector<SyncComponentOps> component_ops;
    std::vector<std::uint32_t> component_offsets;
    component_ops.reserve(desc.components.size());
    component_offsets.reserve(desc.components.size());
    std::size_t total_quantized_bytes = 0;
    for (const ComponentReplication& replication : desc.components) {
        const auto found_ops = settings.component_ops.find(replication.component.value);
        if (found_ops == settings.component_ops.end()) {
            throw std::invalid_argument("sync archetype references a component without sync traits");
        }
        if (found_ops->second.quantized_size == 0 ||
            found_ops->second.quantized_size > SyncComponentOps::QuantizedBytes::max_size) {
            throw std::invalid_argument("sync archetype references a component with invalid quantized size");
        }
        component_offsets.push_back(static_cast<std::uint32_t>(total_quantized_bytes));
        total_quantized_bytes += found_ops->second.quantized_size;
        if (total_quantized_bytes > max_archetype_quantized_bytes) {
            throw std::invalid_argument("sync archetype quantized state exceeds maximum size");
        }
        component_ops.push_back(found_ops->second);
    }
    const SyncArchetypeId id{static_cast<std::uint32_t>(settings.archetypes.size())};
    settings.archetypes.push_back(SyncArchetype{
        std::move(desc.name),
        std::move(desc.tags),
        std::move(desc.components),
        std::move(component_ops),
        std::move(component_offsets),
        static_cast<std::uint32_t>(total_quantized_bytes)});
    return id;
}

const SyncArchetype* find_archetype(const ecs::Registry& registry, SyncArchetypeId id) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!valid_archetype_id(settings, id)) {
        return nullptr;
    }

    return &settings.archetypes[id.value];
}

SyncArchetypeId define_archetype(
    ecs::Registry& registry,
    std::string name,
    std::vector<ComponentReplication> components) {
    return define_archetype(registry, SyncArchetypeDesc{std::move(name), {}, std::move(components)});
}

bool set_owner(ecs::Registry& registry, ecs::Entity entity, ClientId client) {
    register_components(registry);

    if (!registry.alive(entity)) {
        return false;
    }

    return registry.add<NetworkOwner>(entity, NetworkOwner{client}) != nullptr;
}

bool set_client_input_component(ecs::Registry& registry, ecs::Entity component) {
    register_components(registry);
    if (!component || registry.component_info(component) == nullptr) {
        return false;
    }
    if (find_component_ops(registry, component) == nullptr) {
        return false;
    }
    registry.write<SyncSettings>().input_component = component;
    return true;
}

}  // namespace kage::sync
