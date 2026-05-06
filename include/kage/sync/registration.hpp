#pragma once

#include "kage/sync/component_traits.hpp"
#include "kage/sync/components.hpp"
#include "kage/sync/detail/component_trait_adapters.hpp"
#include "kage/sync/detail/type_name.hpp"

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

namespace kage::sync {

template <typename T>
ecs::Entity register_sync_component(ecs::Registry& registry, std::string name = {}) {
    using Traits = SyncComponentTraits<T>;
    using Quantized = typename Traits::Quantized;
    static_assert(
        std::is_trivially_copyable<Quantized>::value,
        "SyncComponentTraits<T>::Quantized must be trivially copyable");

    register_components(registry);

    std::string component_name = name;
    const ecs::Entity component = registry.register_component<T>(std::move(name));

    SyncComponentOps ops;
    ops.name = std::move(component_name);
    ops.quantized_size = sizeof(Quantized);
    ops.quantize = [](const void* value, std::uint8_t* out) {
        const Quantized quantized = Traits::quantize(*static_cast<const T*>(value));
        std::memcpy(out, &quantized, sizeof(Quantized));
    };
    ops.dequantize = [](const std::uint8_t* quantized_bytes, void* out) {
        Quantized quantized{};
        std::memcpy(&quantized, quantized_bytes, sizeof(Quantized));
        *static_cast<T*>(out) = Traits::dequantize(quantized);
    };
    ops.push_to_ecs = [](ecs::Registry& registry, ecs::Entity entity, const std::uint8_t* quantized_bytes) {
        Quantized quantized{};
        std::memcpy(&quantized, quantized_bytes, sizeof(Quantized));
        return registry.add<T>(entity, Traits::dequantize(quantized)) != nullptr;
    };
    ops.serialize = [](
        const std::uint8_t* previous_bytes,
        const std::uint8_t* current_bytes,
        ecs::BitBuffer& out,
        EntityReferenceContext* references) {
        Quantized current{};
        std::memcpy(&current, current_bytes, sizeof(Quantized));

        Quantized previous{};
        const Quantized* previous_ptr = nullptr;
        if (previous_bytes != nullptr) {
            std::memcpy(&previous, previous_bytes, sizeof(Quantized));
            previous_ptr = &previous;
        }

        detail::serialize_quantized<Traits, Quantized>(previous_ptr, current, out, references);
    };
    ops.deserialize = [](
        ecs::BitBuffer& in,
        const std::uint8_t* previous_bytes,
        std::uint8_t* out,
        EntityReferenceContext* references) {
        Quantized previous{};
        const Quantized* previous_ptr = nullptr;
        if (previous_bytes != nullptr) {
            std::memcpy(&previous, previous_bytes, sizeof(Quantized));
            previous_ptr = &previous;
        }

        Quantized quantized{};
        if (!detail::deserialize_quantized<Traits, Quantized>(in, previous_ptr, quantized, references)) {
            return false;
        }

        std::memcpy(out, &quantized, sizeof(Quantized));
        return true;
    };
    ops.references_entities =
        detail::has_context_serialize<Traits, Quantized>::value ||
        detail::has_context_deserialize<Traits, Quantized>::value;
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
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_COMPONENT_DATA)
    if constexpr (detail::has_trace_component<Traits, Quantized>::value) {
        ops.trace = &detail::trace_component_quantized<Traits, Quantized>;
    }
#endif

    registry.write<SyncSettings>().component_ops[component.value] = ops;
    return component;
}

template <typename T>
SyncCueTypeId register_sync_cue(ecs::Registry& registry, std::string name = {}) {
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
    ops.serialize = [](const void* value, ecs::BitBuffer& out, EntityReferenceContext* references) {
        detail::serialize_cue_payload<T>(*static_cast<const T*>(value), out, references);
    };
    ops.play = &detail::play_cue_payload<T>;
    ops.rollback = &detail::rollback_cue_payload<T>;
    ops.equals = &detail::equal_cue_payloads<T>;
    ops.references_entities =
        detail::has_context_cue_serialize<T>::value ||
        detail::has_context_cue_deserialize<T>::value;
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_COMPONENT_DATA)
    if constexpr (detail::has_trace_cue<SyncCueTraits<T>, T>::value) {
        ops.trace = &detail::trace_cue_payload<T>;
    }
#endif
    settings.cue_ops.push_back(ops);
    settings.cue_type_ids[type] = id;
    return id;
}

template <typename T>
bool emit_cue(
    SyncSettings& settings,
    ecs::Entity entity,
    SyncFrame frame,
    const T& cue,
    float relevance_seconds,
    bool only_replicate_to_owner = false) {
    if (!entity || relevance_seconds < 0.0f) {
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
    if (!settings.cue_queue) {
        settings.cue_queue = std::make_shared<SyncCueQueue>();
    }

    QueuedSyncCue queued;
    queued.entity = entity;
    queued.frame = frame;
    queued.type = type;
    queued.relevance_seconds = relevance_seconds;
    queued.only_replicate_to_owner = only_replicate_to_owner;
    if (settings.cue_ops[type].references_entities) {
        queued.value = std::make_shared<T>(cue);
    } else {
        settings.cue_ops[type].serialize(&cue, queued.payload, nullptr);
    }
    std::lock_guard<std::mutex> lock(settings.cue_queue->mutex);
    settings.cue_queue->cues.push_back(std::move(queued));
    return true;
}

template <typename T>
bool emit_cue(
    SyncSettings& settings,
    ecs::Entity entity,
    const T& cue,
    float relevance_seconds,
    bool only_replicate_to_owner = false) {
    return emit_cue(settings, entity, SyncFrame{0}, cue, relevance_seconds, only_replicate_to_owner);
}

template <typename T>
bool emit_cue(
    ecs::Registry& registry,
    ecs::Entity entity,
    SyncFrame frame,
    const T& cue,
    float relevance_seconds,
    bool only_replicate_to_owner = false) {
    register_components(registry);
    if (!registry.alive(entity)) {
        return false;
    }
    return emit_cue(registry.write<SyncSettings>(), entity, frame, cue, relevance_seconds, only_replicate_to_owner);
}

template <typename T>
bool emit_cue(
    ecs::Registry& registry,
    ecs::Entity entity,
    const T& cue,
    float relevance_seconds,
    bool only_replicate_to_owner = false) {
    return emit_cue(registry, entity, SyncFrame{0}, cue, relevance_seconds, only_replicate_to_owner);
}

}  // namespace kage::sync
