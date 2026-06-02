#pragma once

#include "ashiato/sync/component_traits.hpp"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace ashiato::sync::detail {

template <typename T, typename = void>
struct has_context_cue_serialize : std::false_type {};

template <typename T>
struct has_context_cue_serialize<
    T,
    std::void_t<decltype(SyncCueTraits<T>::serialize(
        std::declval<const T&>(),
        std::declval<ashiato::BitBuffer&>(),
        std::declval<ashiato::ComponentSerializationContext&>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_context_cue_deserialize : std::false_type {};

template <typename T>
struct has_context_cue_deserialize<
    T,
    std::void_t<decltype(SyncCueTraits<T>::deserialize(
        std::declval<ashiato::BitBuffer&>(),
        std::declval<T&>(),
        std::declval<ashiato::ComponentSerializationContext&>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_frame_cue_play : std::false_type {};

template <typename T>
struct has_frame_cue_play<
    T,
    std::void_t<decltype(SyncCueTraits<T>::play(
        std::declval<ashiato::Registry&>(),
        std::declval<ashiato::Entity>(),
        std::declval<const T&>(),
        std::declval<float>(),
        std::declval<SyncFrame>()))>> : std::true_type {};

template <typename T>
void serialize_cue_payload(const T& cue, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext& context) {
    static_assert(
        has_context_cue_serialize<T>::value,
        "SyncCueTraits must implement serialize(cue, out, ComponentSerializationContext&)");
    SyncCueTraits<T>::serialize(cue, out, context);
}

template <typename T>
bool read_cue_payload(const ashiato::BitBuffer& payload, T& out, ashiato::ComponentSerializationContext& context) {
    static_assert(
        has_context_cue_deserialize<T>::value,
        "SyncCueTraits must implement deserialize(in, out, ComponentSerializationContext&)");
    ashiato::BitBuffer copy = payload;
    return SyncCueTraits<T>::deserialize(copy, out, context);
}

template <typename T>
bool deserialize_cue_value(
    SyncCueTypeId,
    void*,
    ashiato::BitBuffer& payload,
    CueValue& out,
    ashiato::ComponentSerializationContext& context) {
    static_assert(
        has_context_cue_deserialize<T>::value,
        "SyncCueTraits must implement deserialize(in, out, ComponentSerializationContext&)");
    T value{};
    if (!SyncCueTraits<T>::deserialize(payload, value, context)) {
        return false;
    }
    out.emplace<T>(std::move(value));
    return true;
}

template <typename T>
bool play_cue_value(
    SyncCueTypeId,
    void*,
    ashiato::Registry& registry,
    ashiato::Entity owner,
    const void* value,
    float late_seconds,
    SyncFrame frame) {
    if (value == nullptr) {
        return false;
    }
    if constexpr (has_frame_cue_play<T>::value) {
        return SyncCueTraits<T>::play(registry, owner, *static_cast<const T*>(value), late_seconds, frame);
    } else {
        (void)frame;
        return SyncCueTraits<T>::play(registry, owner, *static_cast<const T*>(value), late_seconds);
    }
}

template <typename T>
bool rollback_cue_value(
    SyncCueTypeId,
    void*,
    ashiato::Registry& registry,
    ashiato::Entity owner,
    const void* value) {
    if (value == nullptr) {
        return false;
    }
    return SyncCueTraits<T>::rollback(registry, owner, *static_cast<const T*>(value));
}

template <typename T>
bool equal_cue_values(SyncCueTypeId, void*, const void* lhs, const void* rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    }
    return SyncCueTraits<T>::equals_cue(*static_cast<const T*>(lhs), *static_cast<const T*>(rhs));
}

template <typename Traits, typename Quantized, typename = void>
struct has_context_serialize : std::false_type {};

template <typename Traits, typename Quantized>
struct has_context_serialize<
    Traits,
    Quantized,
    std::void_t<decltype(Traits::serialize(
        std::declval<const Quantized*>(),
        std::declval<const Quantized&>(),
        std::declval<ashiato::BitBuffer&>(),
        std::declval<ashiato::ComponentSerializationContext&>()))>> : std::true_type {};

template <typename Traits, typename Quantized>
void serialize_quantized(
    const Quantized* previous,
    const Quantized& current,
    ashiato::BitBuffer& out,
    ashiato::ComponentSerializationContext& context) {
    static_assert(
        has_context_serialize<Traits, Quantized>::value,
        "SyncComponentTraits must implement serialize(previous, current, out, ComponentSerializationContext&)");
    Traits::serialize(previous, current, out, context);
}

template <typename Traits, typename Quantized, typename = void>
struct has_context_deserialize : std::false_type {};

template <typename Traits, typename Quantized>
struct has_context_deserialize<
    Traits,
    Quantized,
    std::void_t<decltype(Traits::deserialize(
        std::declval<ashiato::BitBuffer&>(),
        std::declval<const Quantized*>(),
        std::declval<Quantized&>(),
        std::declval<ashiato::ComponentSerializationContext&>()))>> : std::true_type {};

template <typename Traits, typename Quantized>
bool deserialize_quantized(
    ashiato::BitBuffer& in,
    const Quantized* previous,
    Quantized& out,
    ashiato::ComponentSerializationContext& context) {
    static_assert(
        has_context_deserialize<Traits, Quantized>::value,
        "SyncComponentTraits must implement deserialize(in, previous, out, ComponentSerializationContext&)");
    return Traits::deserialize(in, previous, out, context);
}

template <typename Traits, typename = void>
struct references_entities : std::false_type {};

template <typename Traits>
struct references_entities<Traits, std::void_t<decltype(Traits::references_entities)>> :
    std::bool_constant<Traits::references_entities> {};

template <typename Traits, typename Quantized, typename = void>
struct has_interpolate : std::false_type {};

template <typename Traits, typename Quantized>
struct has_interpolate<
    Traits,
    Quantized,
    std::void_t<decltype(Traits::interpolate(
        std::declval<const Quantized&>(),
        std::declval<const Quantized&>(),
        std::declval<float>()))>> : std::true_type {};

template <typename Traits, typename Quantized>
typename std::enable_if<has_interpolate<Traits, Quantized>::value, bool>::type interpolate_quantized(
    const std::uint8_t* from_bytes,
    const std::uint8_t* to_bytes,
    float alpha,
    std::uint8_t* out) {
    Quantized from{};
    Quantized to{};
    std::memcpy(&from, from_bytes, sizeof(Quantized));
    std::memcpy(&to, to_bytes, sizeof(Quantized));
    const Quantized interpolated = Traits::interpolate(from, to, alpha);
    std::memcpy(out, &interpolated, sizeof(Quantized));
    return true;
}

template <typename Traits, typename Quantized>
typename std::enable_if<!has_interpolate<Traits, Quantized>::value, bool>::type interpolate_quantized(
    const std::uint8_t*,
    const std::uint8_t*,
    float,
    std::uint8_t*) {
    return false;
}

template <typename Traits, typename Quantized, typename = void>
struct has_error_blending : std::false_type {};

template <typename Traits, typename Quantized>
struct has_error_blending<
    Traits,
    Quantized,
    std::void_t<
        typename Traits::Error,
        decltype(Traits::compute_error(std::declval<const Quantized&>(), std::declval<const Quantized&>())),
        decltype(Traits::apply_error(
            std::declval<const Quantized&>(),
            std::declval<const typename Traits::Error&>())),
        decltype(Traits::blend_out_error(
            std::declval<const typename Traits::Error&>(),
            std::declval<float>()))>> : std::true_type {};

template <typename Traits, typename Quantized>
typename std::enable_if<has_error_blending<Traits, Quantized>::value, bool>::type compute_error_quantized(
    const std::uint8_t* current_bytes,
    const std::uint8_t* previous_bytes,
    SyncComponentOps::QuantizedBytes& out) {
    using Error = typename Traits::Error;
    static_assert(
        std::is_trivially_copyable<Error>::value,
        "SyncComponentTraits<T>::Error must be trivially copyable");

    Quantized current{};
    Quantized previous{};
    std::memcpy(&current, current_bytes, sizeof(Quantized));
    std::memcpy(&previous, previous_bytes, sizeof(Quantized));
    const Error error = Traits::compute_error(current, previous);

    out.resize(sizeof(Error));
    std::memcpy(out.data(), &error, sizeof(Error));
    return true;
}

template <typename Traits, typename Quantized>
typename std::enable_if<!has_error_blending<Traits, Quantized>::value, bool>::type compute_error_quantized(
    const std::uint8_t*,
    const std::uint8_t*,
    SyncComponentOps::QuantizedBytes&) {
    return false;
}

template <typename Traits, typename Quantized>
typename std::enable_if<has_error_blending<Traits, Quantized>::value, bool>::type apply_error_quantized(
    const std::uint8_t* current_bytes,
    const SyncComponentOps::QuantizedBytes& error_bytes,
    SyncComponentOps::QuantizedBytes& out) {
    using Error = typename Traits::Error;
    if (error_bytes.size() != sizeof(Error)) {
        return false;
    }

    Quantized current{};
    Error error{};
    std::memcpy(&current, current_bytes, sizeof(Quantized));
    std::memcpy(&error, error_bytes.data(), sizeof(Error));
    const Quantized display = Traits::apply_error(current, error);

    out.resize(sizeof(Quantized));
    std::memcpy(out.data(), &display, sizeof(Quantized));
    return true;
}

template <typename Traits, typename Quantized>
typename std::enable_if<!has_error_blending<Traits, Quantized>::value, bool>::type apply_error_quantized(
    const std::uint8_t*,
    const SyncComponentOps::QuantizedBytes&,
    SyncComponentOps::QuantizedBytes&) {
    return false;
}

template <typename Traits, typename Quantized>
typename std::enable_if<has_error_blending<Traits, Quantized>::value, bool>::type blend_out_error_quantized(
    const SyncComponentOps::QuantizedBytes& error_bytes,
    float dt_seconds,
    SyncComponentOps::QuantizedBytes& out) {
    using Error = typename Traits::Error;
    if (error_bytes.size() != sizeof(Error)) {
        return false;
    }

    Error error{};
    std::memcpy(&error, error_bytes.data(), sizeof(Error));
    const Error blended = Traits::blend_out_error(error, dt_seconds);

    out.resize(sizeof(Error));
    std::memcpy(out.data(), &blended, sizeof(Error));
    return true;
}

template <typename Traits, typename Quantized>
typename std::enable_if<!has_error_blending<Traits, Quantized>::value, bool>::type blend_out_error_quantized(
    const SyncComponentOps::QuantizedBytes&,
    float,
    SyncComponentOps::QuantizedBytes&) {
    return false;
}

template <typename Traits, typename Quantized, typename = void>
struct has_should_roll_back : std::false_type {};

template <typename Traits, typename Quantized>
struct has_should_roll_back<
    Traits,
    Quantized,
    std::void_t<decltype(Traits::should_roll_back(
        std::declval<const Quantized&>(),
        std::declval<const Quantized&>()))>> : std::true_type {};

template <typename Traits, typename Quantized>
typename std::enable_if<has_should_roll_back<Traits, Quantized>::value, bool>::type should_roll_back_quantized(
    const std::uint8_t* predicted_bytes,
    const std::uint8_t* authoritative_bytes) {
    Quantized predicted{};
    Quantized authoritative{};
    std::memcpy(&predicted, predicted_bytes, sizeof(Quantized));
    std::memcpy(&authoritative, authoritative_bytes, sizeof(Quantized));
    return Traits::should_roll_back(predicted, authoritative);
}

template <typename Traits, typename Quantized>
typename std::enable_if<!has_should_roll_back<Traits, Quantized>::value, bool>::type should_roll_back_quantized(
    const std::uint8_t*,
    const std::uint8_t*) {
    return false;
}

#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
template <typename Traits, typename Quantized, typename = void>
struct has_trace_component : std::false_type {};

template <typename Traits, typename Quantized>
struct has_trace_component<
    Traits,
    Quantized,
    std::void_t<decltype(Traits::trace(
        std::declval<const Quantized&>(),
        std::declval<SyncTraceStringBuilder&>()))>> : std::true_type {};

template <typename Traits, typename Quantized>
void trace_component_quantized(const std::uint8_t* quantized_bytes, SyncTraceStringBuilder& out) {
    Quantized quantized{};
    std::memcpy(&quantized, quantized_bytes, sizeof(Quantized));
    Traits::trace(quantized, out);
}

template <typename Traits, typename Cue, typename = void>
struct has_trace_cue : std::false_type {};

template <typename Traits, typename Cue>
struct has_trace_cue<
    Traits,
    Cue,
    std::void_t<decltype(Traits::trace(
        std::declval<const Cue&>(),
        std::declval<SyncTraceStringBuilder&>()))>> : std::true_type {};

template <typename T>
bool trace_cue_payload(SyncCueTypeId, void*, const ashiato::BitBuffer& payload, SyncTraceStringBuilder& out) {
    T value{};
    ashiato::ComponentSerializationContext context;
    try {
        if (!read_cue_payload(payload, value, context)) {
            return false;
        }
    } catch (...) {
        return false;
    }
    SyncCueTraits<T>::trace(value, out);
    return true;
}

template <typename T>
bool trace_cue_value(SyncCueTypeId, void*, const void* value, SyncTraceStringBuilder& out) {
    if (value == nullptr) {
        return false;
    }
    SyncCueTraits<T>::trace(*static_cast<const T*>(value), out);
    return true;
}
#endif

}  // namespace ashiato::sync::detail
