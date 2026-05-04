#pragma once

#include "kage/sync/component_traits.hpp"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace kage::sync::detail {

template <typename T, typename = void>
struct has_context_cue_serialize : std::false_type {};

template <typename T>
struct has_context_cue_serialize<
    T,
    std::void_t<decltype(SyncCueTraits<T>::serialize(
        std::declval<const T&>(),
        std::declval<BitBuffer&>(),
        std::declval<EntityReferenceContext&>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_context_cue_deserialize : std::false_type {};

template <typename T>
struct has_context_cue_deserialize<
    T,
    std::void_t<decltype(SyncCueTraits<T>::deserialize(
        std::declval<BitBuffer&>(),
        std::declval<T&>(),
        std::declval<EntityReferenceContext&>()))>> : std::true_type {};

template <typename T, typename = void>
struct has_frame_cue_play : std::false_type {};

template <typename T>
struct has_frame_cue_play<
    T,
    std::void_t<decltype(SyncCueTraits<T>::play(
        std::declval<ecs::Registry&>(),
        std::declval<ecs::Entity>(),
        std::declval<const T&>(),
        std::declval<float>(),
        std::declval<SyncFrame>()))>> : std::true_type {};

template <typename T>
void serialize_cue_payload(const T& cue, BitBuffer& out, EntityReferenceContext* references) {
    if constexpr (has_context_cue_serialize<T>::value) {
        EntityReferenceContext empty_references;
        SyncCueTraits<T>::serialize(cue, out, references != nullptr ? *references : empty_references);
    } else {
        (void)references;
        SyncCueTraits<T>::serialize(cue, out);
    }
}

template <typename T>
bool read_cue_payload(const BitBuffer& payload, T& out, EntityReferenceContext* references = nullptr) {
    BitBuffer copy = payload;
    if constexpr (has_context_cue_deserialize<T>::value) {
        EntityReferenceContext empty_references;
        return SyncCueTraits<T>::deserialize(copy, out, references != nullptr ? *references : empty_references);
    } else {
        (void)references;
        return SyncCueTraits<T>::deserialize(copy, out);
    }
}

template <typename T>
bool play_cue_payload(
    ecs::Registry& registry,
    ecs::Entity owner,
    const BitBuffer& payload,
    float late_seconds,
    SyncFrame frame,
    EntityReferenceContext* references) {
    T value{};
    if (!read_cue_payload(payload, value, references)) {
        return false;
    }
    if constexpr (has_frame_cue_play<T>::value) {
        return SyncCueTraits<T>::play(registry, owner, value, late_seconds, frame);
    } else {
        (void)frame;
        return SyncCueTraits<T>::play(registry, owner, value, late_seconds);
    }
}

template <typename T>
bool rollback_cue_payload(
    ecs::Registry& registry,
    ecs::Entity owner,
    const BitBuffer& payload,
    EntityReferenceContext* references) {
    T value{};
    if (!read_cue_payload(payload, value, references)) {
        return false;
    }
    return SyncCueTraits<T>::rollback(registry, owner, value);
}

template <typename T>
bool equal_cue_payloads(
    const BitBuffer& lhs_payload,
    const BitBuffer& rhs_payload,
    EntityReferenceContext* references) {
    T lhs{};
    T rhs{};
    if (!read_cue_payload(lhs_payload, lhs, references) || !read_cue_payload(rhs_payload, rhs, references)) {
        return lhs_payload == rhs_payload;
    }
    return SyncCueTraits<T>::equals_cue(lhs, rhs);
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
        std::declval<BitBuffer&>(),
        std::declval<EntityReferenceContext&>()))>> : std::true_type {};

template <typename Traits, typename Quantized>
void serialize_quantized(
    const Quantized* previous,
    const Quantized& current,
    BitBuffer& out,
    EntityReferenceContext* references) {
    if constexpr (has_context_serialize<Traits, Quantized>::value) {
        EntityReferenceContext empty_references;
        Traits::serialize(previous, current, out, references != nullptr ? *references : empty_references);
    } else {
        (void)references;
        Traits::serialize(previous, current, out);
    }
}

template <typename Traits, typename Quantized, typename = void>
struct has_context_deserialize : std::false_type {};

template <typename Traits, typename Quantized>
struct has_context_deserialize<
    Traits,
    Quantized,
    std::void_t<decltype(Traits::deserialize(
        std::declval<BitBuffer&>(),
        std::declval<const Quantized*>(),
        std::declval<Quantized&>(),
        std::declval<EntityReferenceContext&>()))>> : std::true_type {};

template <typename Traits, typename Quantized>
bool deserialize_quantized(
    BitBuffer& in,
    const Quantized* previous,
    Quantized& out,
    EntityReferenceContext* references) {
    if constexpr (has_context_deserialize<Traits, Quantized>::value) {
        EntityReferenceContext empty_references;
        return Traits::deserialize(in, previous, out, references != nullptr ? *references : empty_references);
    } else {
        (void)references;
        return Traits::deserialize(in, previous, out);
    }
}

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

#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_COMPONENT_DATA)
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
bool trace_cue_payload(const BitBuffer& payload, SyncTraceStringBuilder& out) {
    T value{};
    if (!read_cue_payload(payload, value)) {
        return false;
    }
    SyncCueTraits<T>::trace(value, out);
    return true;
}
#endif

}  // namespace kage::sync::detail
