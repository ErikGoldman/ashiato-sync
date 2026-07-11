#include "ashiato/sync/components.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace ashiato::sync {
namespace {

constexpr std::size_t max_archetype_tags = 64;
constexpr std::size_t max_archetype_components = 63;

static_assert(sizeof(SyncRole) == sizeof(std::int32_t), "SyncRole debug field metadata assumes a 32-bit enum");
static_assert(sizeof(ClientId) == sizeof(std::uint8_t), "ClientId debug field metadata assumes an 8-bit id");

void register_debug_fields(
    ashiato::Registry& registry,
    ashiato::Entity component,
    std::vector<ashiato::ComponentField> fields) {
    if (!registry.set_component_fields(component, std::move(fields))) {
        throw std::logic_error("failed to register sync component debug fields");
    }
}

void register_sync_settings_debug_fields(ashiato::Registry& registry, ashiato::Entity component) {
    register_debug_fields(
        registry,
        component,
        {
            {"role", offsetof(SyncSettings, role), registry.primitive_type(ashiato::PrimitiveType::I32), 1},
            {"local_client", offsetof(SyncSettings, local_client), registry.primitive_type(ashiato::PrimitiveType::U8), 1},
            {
                "input_component.value",
                offsetof(SyncSettings, input_component) + offsetof(ashiato::Entity, value),
                registry.primitive_type(ashiato::PrimitiveType::U64),
                1,
            },
            {
                "fixed_dt_seconds",
                offsetof(SyncSettings, fixed_dt_seconds),
                registry.primitive_type(ashiato::PrimitiveType::F64),
                1,
            },
        });
}

void register_frame_info_debug_fields(ashiato::Registry& registry, ashiato::Entity component) {
    register_debug_fields(
        registry,
        component,
        {{"frame", offsetof(FrameInfo, frame), registry.primitive_type(ashiato::PrimitiveType::U32), 1}});
}

void register_sync_authority_debug_fields(ashiato::Registry& registry, ashiato::Entity component) {
    register_debug_fields(
        registry,
        component,
        {
            {
                "authoritative",
                offsetof(SyncAuthority, authoritative),
                registry.primitive_type(ashiato::PrimitiveType::Bool),
                1,
            },
        });
}

bool valid_archetype_id(const SyncSettings& settings, SyncArchetypeId id) {
    return id.value < settings.archetypes.size();
}

void validate_component_replication(const ashiato::Registry& registry, const ComponentReplication& replication) {
    if (!replication.component || registry.component_info(replication.component) == nullptr) {
        throw std::invalid_argument("sync archetype references an unregistered component");
    }
    if (replication.serializer != invalid_sync_component_serializer_id) {
        const SyncComponentOps* ops = find_component_serializer_ops(registry, replication.serializer);
        if (ops == nullptr || ops->serialization.component != replication.component) {
            throw std::invalid_argument("sync archetype references an invalid component serializer");
        }
        return;
    }
    if (find_component_ops(registry, replication.component) == nullptr) {
        throw std::invalid_argument("sync archetype references a component without sync traits");
    }
}

void validate_tag_replication(const ashiato::Registry& registry, const SyncTagReplication& replication) {
    const ashiato::ComponentInfo* info = registry.component_info(replication.tag);
    if (!replication.tag || info == nullptr || !info->tag) {
        throw std::invalid_argument("sync archetype references an unregistered tag");
    }
}

}  // namespace

void register_components(ashiato::Registry& registry) {
    const ashiato::Entity sync_settings = registry.register_component<SyncSettings>("ashiato.sync.SyncSettings");
    const ashiato::Entity frame_info = registry.register_component<FrameInfo>("ashiato.sync.FrameInfo");
    registry.register_component<CueDispatcher>("ashiato.sync.CueDispatcher");
    const ashiato::Entity sync_authority = registry.register_component<SyncAuthority>("ashiato.sync.SyncAuthority");
    registry.register_component<Replicated>("ashiato.sync.Replicated");
    registry.register_component<NetworkOwner>("ashiato.sync.NetworkOwner");
    registry.register_component<FractionalTickSampled>("ashiato.sync.FractionalTickSampled");
    registry.register_component<NoResim>("ashiato.sync.NoResim");
    registry.register_component<NoSimulate>("ashiato.sync.NoSimulate");

    register_sync_settings_debug_fields(registry, sync_settings);
    register_frame_info_debug_fields(registry, frame_info);
    register_sync_authority_debug_fields(registry, sync_authority);
}

CueDispatcher::CueDispatcher(const CueDispatcher& other) {
    std::lock_guard<std::mutex> lock(other.mutex_);
    cues_ = other.cues_;
}

CueDispatcher& CueDispatcher::operator=(const CueDispatcher& other) {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        cues_ = other.cues_;
    }
    return *this;
}

CueDispatcher::CueDispatcher(CueDispatcher&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mutex_);
    cues_ = std::move(other.cues_);
}

CueDispatcher& CueDispatcher::operator=(CueDispatcher&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        cues_ = std::move(other.cues_);
    }
    return *this;
}

bool CueDispatcher::enqueue(QueuedSyncCue cue) {
    if (!cue.entity || cue.relevance_seconds < 0.0f) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    cues_.push_back(std::move(cue));
    return true;
}

std::vector<QueuedSyncCue> CueDispatcher::drain() {
    std::vector<QueuedSyncCue> drained;
    std::lock_guard<std::mutex> lock(mutex_);
    drained.swap(cues_);
    return drained;
}

void CueDispatcher::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cues_.clear();
}

QueuedSyncCueView CueDispatcher::view() const noexcept {
    return QueuedSyncCueView{
        cues_.empty() ? nullptr : cues_.data(),
        cues_.size()};
}

bool CueDispatcher::empty() const noexcept {
    return cues_.empty();
}

std::size_t CueDispatcher::size() const noexcept {
    return cues_.size();
}

const SyncComponentOps* find_component_ops(const ashiato::Registry& registry, ashiato::Entity component) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    const auto found = settings.component_ops.find(component.value);
    return found != settings.component_ops.end() ? &found->second : nullptr;
}

const SyncComponentOps* find_component_serializer_ops(
    const ashiato::Registry& registry,
    SyncComponentSerializerId serializer) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    return serializer.value < settings.component_serializers.size()
        ? &settings.component_serializers[serializer.value]
        : nullptr;
}

bool set_fractional_tick_sampled(ashiato::Registry& registry, ashiato::Entity component, bool enabled) {
    register_components(registry);
    if (!component || registry.component_info(component) == nullptr) {
        return false;
    }

    const ashiato::Entity tag = registry.component<FractionalTickSampled>();
    if (!enabled) {
        if (!registry.has<FractionalTickSampled>(component)) {
            return true;
        }
        return registry.remove_tag(component, tag);
    }

    const SyncComponentOps* ops = find_component_ops(registry, component);
    if (ops == nullptr ||
        ops->interpolate == nullptr ||
        ops->compute_error == nullptr ||
        ops->apply_error == nullptr ||
        ops->blend_out_error == nullptr) {
        return false;
    }

    return registry.add_tag(component, tag);
}

bool is_fractional_tick_sampled(const ashiato::Registry& registry, ashiato::Entity component) {
    if (!component || registry.component_info(component) == nullptr) {
        return false;
    }
    return registry.has<FractionalTickSampled>(component);
}

SyncArchetypeId define_archetype(ashiato::Registry& registry, SyncArchetypeDesc desc) {
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
        const SyncComponentOps* ops = nullptr;
        if (replication.serializer == invalid_sync_component_serializer_id) {
            const auto found_ops = settings.component_ops.find(replication.component.value);
            if (found_ops != settings.component_ops.end()) {
                ops = &found_ops->second;
            }
        } else if (replication.serializer.value < settings.component_serializers.size()) {
            ops = &settings.component_serializers[replication.serializer.value];
        }
        if (ops == nullptr) {
            throw std::invalid_argument("sync archetype references a component without sync traits");
        }
        if (ops->serialization.component != replication.component) {
            throw std::invalid_argument("sync archetype references a serializer for a different component");
        }
        if (ops->serialization.quantized_size == 0 ||
            ops->serialization.quantized_size > SyncComponentOps::QuantizedBytes::max_size) {
            throw std::invalid_argument("sync archetype references a component with invalid quantized size");
        }
        component_offsets.push_back(static_cast<std::uint32_t>(total_quantized_bytes));
        total_quantized_bytes += ops->serialization.quantized_size;
        component_ops.push_back(*ops);
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

const SyncArchetype* find_archetype(const ashiato::Registry& registry, SyncArchetypeId id) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    if (!valid_archetype_id(settings, id)) {
        return nullptr;
    }

    return &settings.archetypes[id.value];
}

SyncArchetypeId define_archetype(
    ashiato::Registry& registry,
    std::string name,
    std::vector<ComponentReplication> components) {
    return define_archetype(registry, SyncArchetypeDesc{std::move(name), {}, std::move(components)});
}

bool set_owner(ashiato::Registry& registry, ashiato::Entity entity, ClientId client) {
    register_components(registry);

    if (!registry.alive(entity)) {
        return false;
    }

    return registry.add<NetworkOwner>(entity, NetworkOwner{client}) != nullptr;
}

bool set_client_input_component(ashiato::Registry& registry, ashiato::Entity component) {
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

}  // namespace ashiato::sync
