#pragma once

#include "ashiato/sync/component_traits.hpp"
#include "ashiato/sync/assert.hpp"
#include "ashiato/sync/components.hpp"
#include "ashiato/sync/detail/component_trait_adapters.hpp"
#include "ashiato/sync/detail/type_name.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>

namespace ashiato::sync {

namespace detail {

template <typename T, typename ComponentTraits, typename Serializer>
struct SyncComponentSerializationTraitsAdapter {
    using Quantized = typename ComponentTraits::Quantized;

	static void quantize(const T& value, Quantized& out) {
		Serializer::quantize(value, out);
	}

	static T dequantize(const Quantized& value) {
		return Serializer::dequantize(value);
	}

    static void serialize(
        const Quantized* previous,
        const Quantized& current,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext& context) {
        serialize_quantized<Serializer, Quantized>(
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
        return deserialize_quantized<Serializer, Quantized>(
            in,
            previous,
            out,
            context);
    }
};

template <typename Traits, typename = void>
struct SyncComponentDefaultSerializer {
    using Type = Traits;
};

template <typename Traits>
struct SyncComponentDefaultSerializer<
    Traits,
    std::void_t<typename Traits::Settings, typename Traits::template Serializer<typename Traits::Settings>>> {
    using Type = typename Traits::template Serializer<typename Traits::Settings>;
};

template <typename T, typename Serializer>
SyncComponentOps make_sync_component_ops(ashiato::Entity component, std::string name) {
    using Traits = SyncComponentTraits<T>;
    using Quantized = typename Traits::Quantized;
    using SerializationTraits = SyncComponentSerializationTraitsAdapter<T, Traits, Serializer>;
    static_assert(
        std::is_trivially_copyable<Quantized>::value,
        "SyncComponentTraits<T>::Quantized must be trivially copyable");

    ashiato::ComponentSerializationOps serialization_ops =
        ashiato::make_component_serialization_ops<T, SerializationTraits>(std::move(name));
    serialization_ops.component = component;

    SyncComponentOps ops;
    ops.serialization = std::move(serialization_ops);
    ops.references_entities = references_entities<Serializer>::value;
    if constexpr (has_interpolate<Serializer, Quantized>::value) {
        ops.interpolate = &interpolate_quantized<Serializer, Quantized>;
    }
    if constexpr (has_error_blending<Serializer, Quantized>::value) {
        using Error = typename Traits::Error;
        static_assert(
            std::is_trivially_copyable<Error>::value,
            "SyncComponentTraits<T>::Error must be trivially copyable");
        ops.error_size = sizeof(Error);
        ops.compute_error = &compute_error_quantized<Serializer, Quantized>;
        ops.apply_error = &apply_error_quantized<Serializer, Quantized>;
        ops.blend_out_error = &blend_out_error_quantized<Serializer, Quantized>;
    }
    if constexpr (has_should_roll_back<Serializer, Quantized>::value) {
        ops.should_roll_back = &should_roll_back_quantized<Serializer, Quantized>;
    }
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
    if constexpr (has_trace_component<Serializer, Quantized>::value) {
        ops.trace = &trace_component_quantized<Serializer, Quantized>;
    }
#endif
    return ops;
}

inline bool component_profile_capabilities_match(
    const SyncComponentOps& lhs,
    const SyncComponentOps& rhs) noexcept {
    const bool capabilities_match =
        lhs.references_entities == rhs.references_entities &&
        (lhs.interpolate != nullptr) == (rhs.interpolate != nullptr) &&
        (lhs.compute_error != nullptr) == (rhs.compute_error != nullptr) &&
        (lhs.apply_error != nullptr) == (rhs.apply_error != nullptr) &&
        (lhs.blend_out_error != nullptr) == (rhs.blend_out_error != nullptr) &&
        (lhs.should_roll_back != nullptr) == (rhs.should_roll_back != nullptr) &&
        lhs.error_size == rhs.error_size;
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
    return capabilities_match && (lhs.trace != nullptr) == (rhs.trace != nullptr);
#else
    return capabilities_match;
#endif
}

inline void validate_component_profile_capabilities(
    const SyncSettings& settings,
    ashiato::Entity component,
    const SyncComponentOps& candidate) {
    const auto default_ops = settings.component_ops.find(component.value);
    if (default_ops != settings.component_ops.end() &&
        !component_profile_capabilities_match(default_ops->second, candidate)) {
        throw std::invalid_argument("sync component serializer profiles must have homogeneous capabilities");
    }

    for (const SyncComponentOps& profile : settings.component_serializers) {
        if (profile.serialization.component == component &&
            !component_profile_capabilities_match(profile, candidate)) {
            throw std::invalid_argument("sync component serializer profiles must have homogeneous capabilities");
        }
    }
}

}  // namespace detail

template <typename T>
ashiato::Entity register_sync_component(ashiato::Registry& registry, std::string name = {}) {
    using Traits = SyncComponentTraits<T>;
    using Serializer = typename detail::SyncComponentDefaultSerializer<Traits>::Type;

    register_components(registry);

    std::string component_name = name;
    const ashiato::Entity component = registry.register_component<T>(std::move(name));

    SyncComponentOps ops = detail::make_sync_component_ops<T, Serializer>(component, std::move(component_name));
    SyncSettings& settings = registry.write<SyncSettings>();
    detail::validate_component_profile_capabilities(settings, component, ops);
    settings.component_ops[component.value] = std::move(ops);
    return component;
}

template <typename T, typename Serializer>
SyncComponentSerializerId register_sync_component_serializer(ashiato::Registry& registry, std::string name = {}) {
    register_components(registry);

    const ashiato::Entity component = registry.register_component<T>();
    SyncComponentOps ops = detail::make_sync_component_ops<T, Serializer>(component, std::move(name));

    SyncSettings& settings = registry.write<SyncSettings>();
    detail::validate_component_profile_capabilities(settings, component, ops);
    const SyncComponentSerializerId id{static_cast<std::uint32_t>(settings.component_serializers.size())};
    settings.component_serializers.push_back(std::move(ops));
    return id;
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
    ops.deserialize_into = &detail::deserialize_cue_value<T>;
    ops.play = &detail::play_cue_value<T>;
    ops.rollback = &detail::rollback_cue_value<T>;
    ops.equals = &detail::equal_cue_values<T>;
    ops.references_entities = detail::references_entities<SyncCueTraits<T>>::value;
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
    if constexpr (detail::has_trace_cue<SyncCueTraits<T>, T>::value) {
        ops.trace = &detail::trace_cue_payload<T>;
        ops.trace_value = &detail::trace_cue_value<T>;
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
    if (!entity || frame == 0U) {
        return false;
    }
    if (!detail::cue_relevance_fits_frame_range(frame, relevance_seconds, settings.fixed_dt_seconds)) {
        ASHIATO_SYNC_ASSERT_FAIL("cue relevance must be finite, non-negative, and fit in the frame range");
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
    queued.value.emplace<T>(cue);
    return enqueue(std::move(queued));
}

inline bool CueDispatcher::emit_raw(
    const SyncSettings& settings,
    SyncFrame frame,
    ashiato::Entity entity,
    SyncCueTypeId type,
    ashiato::BitBuffer payload,
    float relevance_seconds,
    bool only_replicate_to_owner
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
    ,
    std::vector<ashiato::SerializationTraceScope> payload_trace_scopes
#endif
) {
    if (!entity || frame == 0U) {
        return false;
    }
    if (!detail::cue_relevance_fits_frame_range(frame, relevance_seconds, settings.fixed_dt_seconds)) {
        ASHIATO_SYNC_ASSERT_FAIL("cue relevance must be finite, non-negative, and fit in the frame range");
        return false;
    }
    if (payload.bit_size() > protocol::max_cue_payload_bits) {
        ASHIATO_SYNC_ASSERT_FAIL("cue payload exceeds the protocol limit");
        return false;
    }
    if (type >= settings.cue_ops.size() ||
        settings.cue_ops[type].deserialize_into == nullptr ||
        settings.cue_ops[type].play == nullptr) {
        return false;
    }

    QueuedSyncCue queued;
    queued.entity = entity;
    queued.frame = frame;
    queued.type = type;
    queued.relevance_seconds = relevance_seconds;
    queued.payload = std::move(payload);
#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
    queued.payload_trace_scopes = std::move(payload_trace_scopes);
#endif
    queued.only_replicate_to_owner = only_replicate_to_owner;
    return enqueue(std::move(queued));
}

}  // namespace ashiato::sync
