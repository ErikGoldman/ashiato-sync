#pragma once

#include "ashiato/sync/component_traits.hpp"
#include "ashiato/sync/components.hpp"
#include "ashiato/sync/detail/component_trait_adapters.hpp"
#include "ashiato/sync/detail/type_name.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>

namespace ashiato::sync {

namespace detail {

template <typename T, typename Traits>
struct SyncComponentSerializationTraitsAdapter {
    using Quantized = typename Traits::Quantized;

    static Quantized quantize(const T& value) {
        return Traits::quantize(value);
    }

    static T dequantize(const Quantized& value) {
        return Traits::dequantize(value);
    }

    static void serialize(
        const Quantized* previous,
        const Quantized& current,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext& context) {
        serialize_quantized<Traits, Quantized>(
            previous,
            current,
            out,
            context);
    }

    static bool deserialize(
        ashiato::BitBuffer& in,
        const Quantized* previous,
        Quantized& out,
        ashiato::ComponentSerializationContext& context) {
        return deserialize_quantized<Traits, Quantized>(
            in,
            previous,
            out,
            context);
    }
};

}  // namespace detail

template <typename T>
ashiato::Entity register_sync_component(ashiato::Registry& registry, std::string name = {}) {
    using Traits = SyncComponentTraits<T>;
    using Quantized = typename Traits::Quantized;
    using SerializationTraits = detail::SyncComponentSerializationTraitsAdapter<T, Traits>;
    static_assert(
        std::is_trivially_copyable<Quantized>::value,
        "SyncComponentTraits<T>::Quantized must be trivially copyable");

    register_components(registry);

    std::string component_name = name;
    const ashiato::Entity component = registry.register_component<T>(std::move(name));

    ashiato::ComponentSerializationOps serialization_ops =
        ashiato::make_component_serialization_ops<T, SerializationTraits>(component_name);
    serialization_ops.component = component;

    SyncComponentOps ops;
    ops.serialization = std::move(serialization_ops);
    ops.references_entities = detail::references_entities<Traits>::value;
    if constexpr (detail::has_interpolate<Traits, Quantized>::value) {
        ops.interpolate = &detail::interpolate_quantized<Traits, Quantized>;
    }
    if constexpr (detail::has_error_blending<Traits, Quantized>::value) {
        using Error = typename Traits::Error;
        static_assert(
            std::is_trivially_copyable<Error>::value,
            "SyncComponentTraits<T>::Error must be trivially copyable");
        ops.error_size = sizeof(Error);
        ops.compute_error = &detail::compute_error_quantized<Traits, Quantized>;
        ops.apply_error = &detail::apply_error_quantized<Traits, Quantized>;
        ops.blend_out_error = &detail::blend_out_error_quantized<Traits, Quantized>;
    }
    if constexpr (detail::has_should_roll_back<Traits, Quantized>::value) {
        ops.should_roll_back = &detail::should_roll_back_quantized<Traits, Quantized>;
    }
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
    if constexpr (detail::has_trace_component<Traits, Quantized>::value) {
        ops.trace = &detail::trace_component_quantized<Traits, Quantized>;
    }
#endif

    registry.write<SyncSettings>().component_ops[component.value] = ops;
    return component;
}

template <typename T>
SyncCueTypeId register_sync_cue(ashiato::Registry& registry, std::string name = {}) {
    register_components(registry);

    SyncSettings& settings = registry.write<SyncSettings>();
    const std::type_index type = std::type_index(typeid(T));
    const auto found = settings.cue_type_ids.find(type);
    if (found != settings.cue_type_ids.end()) {
        return found->second;
    }
    if (settings.cue_ops.size() >= std::numeric_limits<SyncCueTypeId>::max()) {
        throw std::length_error("sync cue type id space exhausted");
    }

    const SyncCueTypeId id = static_cast<SyncCueTypeId>(settings.cue_ops.size());
    SyncCueOps ops;
    ops.name = name.empty() ? detail::default_type_name<T>() : std::move(name);
    ops.serialize = [](const void* value, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext& context) {
        detail::serialize_cue_payload<T>(*static_cast<const T*>(value), out, context);
    };
    ops.deserialize_value = [](SyncCueTypeId, void*, const ashiato::BitBuffer& payload, ashiato::ComponentSerializationContext& context) -> std::shared_ptr<void> {
        T value{};
        if (!detail::read_cue_payload(payload, value, context)) {
            return {};
        }
        return std::make_shared<T>(std::move(value));
    };
    ops.play = &detail::play_cue_payload<T>;
    ops.rollback = &detail::rollback_cue_payload<T>;
    ops.equals = &detail::equal_cue_payloads<T>;
    ops.references_entities = detail::references_entities<SyncCueTraits<T>>::value;
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
    if constexpr (detail::has_trace_cue<SyncCueTraits<T>, T>::value) {
        ops.trace = &detail::trace_cue_payload<T>;
    }
#endif
    settings.cue_ops.push_back(ops);
    settings.cue_type_ids[type] = id;
    return id;
}

inline SyncCueTypeId register_runtime_sync_cue(
    ashiato::Registry& registry,
    std::string key,
    SyncCueOps ops) {
    register_components(registry);

    SyncSettings& settings = registry.write<SyncSettings>();
    const auto found = settings.runtime_cue_type_ids.find(key);
    if (found != settings.runtime_cue_type_ids.end()) {
        if (found->second < settings.cue_ops.size()) {
            if (ops.name.empty()) {
                ops.name = key;
            }
            settings.cue_ops[found->second] = std::move(ops);
        }
        return found->second;
    }
    if (settings.cue_ops.size() >= std::numeric_limits<SyncCueTypeId>::max()) {
        throw std::length_error("sync cue type id space exhausted");
    }

    const SyncCueTypeId id = static_cast<SyncCueTypeId>(settings.cue_ops.size());
    if (ops.name.empty()) {
        ops.name = key;
    }
    settings.cue_ops.push_back(std::move(ops));
    settings.runtime_cue_type_ids.emplace(std::move(key), id);
    return id;
}

inline bool find_runtime_sync_cue(
    const ashiato::Registry& registry,
    const std::string& key,
    SyncCueTypeId& out) {
    const SyncSettings& settings = registry.get<SyncSettings>();
    const auto found = settings.runtime_cue_type_ids.find(key);
    if (found == settings.runtime_cue_type_ids.end()) {
        return false;
    }
    out = found->second;
    return true;
}

template <typename T>
bool CueDispatcher::emit(
    const SyncSettings& settings,
    const FrameInfo& frame,
    ashiato::Entity entity,
    const T& cue,
    float relevance_seconds,
    bool only_replicate_to_owner) {
    return emit(settings, frame.frame, entity, cue, relevance_seconds, only_replicate_to_owner);
}

template <typename T>
bool CueDispatcher::emit(
    const SyncSettings& settings,
    SyncFrame frame,
    ashiato::Entity entity,
    const T& cue,
    float relevance_seconds,
    bool only_replicate_to_owner) {
    if (!entity || frame == 0U || relevance_seconds < 0.0f) {
        return false;
    }

    const auto found_type = settings.cue_type_ids.find(std::type_index(typeid(T)));
    if (found_type == settings.cue_type_ids.end()) {
        return false;
    }
    const SyncCueTypeId type = found_type->second;
    if (type >= settings.cue_ops.size() || settings.cue_ops[type].serialize == nullptr) {
        return false;
    }

    QueuedSyncCue queued;
    queued.entity = entity;
    queued.frame = frame;
    queued.type = type;
    queued.relevance_seconds = relevance_seconds;
    queued.only_replicate_to_owner = only_replicate_to_owner;
    queued.value = std::make_shared<T>(cue);
    return enqueue(std::move(queued));
}

inline bool CueDispatcher::emit_raw(
    const SyncSettings& settings,
    SyncFrame frame,
    ashiato::Entity entity,
    SyncCueTypeId type,
    ashiato::BitBuffer payload,
    float relevance_seconds,
    bool only_replicate_to_owner) {
    if (!entity || frame == 0U || relevance_seconds < 0.0f) {
        return false;
    }
    if (type >= settings.cue_ops.size() || settings.cue_ops[type].play == nullptr) {
        return false;
    }

    QueuedSyncCue queued;
    queued.entity = entity;
    queued.frame = frame;
    queued.type = type;
    queued.relevance_seconds = relevance_seconds;
    queued.payload = std::move(payload);
    queued.only_replicate_to_owner = only_replicate_to_owner;
    return enqueue(std::move(queued));
}

}  // namespace ashiato::sync
