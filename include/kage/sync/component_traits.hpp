#pragma once

#include "kage/sync/types.hpp"

#include <cstring>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace kage::sync {

#ifdef KAGE_SYNC_ENABLE_TRACING
void trace_rollback_reason(const char* reason);
void trace_rollback_reason(const std::string& reason);
#define TRACE_ROLLBACK_IF(condition, reason)    \
    do {                                        \
        if (condition) {                        \
            ::kage::sync::trace_rollback_reason(reason); \
            return true;                        \
        }                                       \
    } while (false)
#else
#define TRACE_ROLLBACK_IF(condition, reason) \
    do {                                     \
        if (condition) {                     \
            return true;                     \
        }                                    \
    } while (false)
#endif

template <typename T>
struct SyncComponentTraits {
    using Quantized = T;

    static Quantized quantize(const T& value) {
        static_assert(
            std::is_trivially_copyable<T>::value,
            "default SyncComponentTraits require a trivially copyable component");
        return value;
    }

    static T dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized* /*previous*/, const Quantized& current, BitBuffer& out) {
        static_assert(
            std::is_trivially_copyable<Quantized>::value,
            "default SyncComponentTraits serialization requires a trivially copyable quantized state");
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(BitBuffer& in, const Quantized* /*previous*/, Quantized& out) {
        static_assert(
            std::is_trivially_copyable<Quantized>::value,
            "default SyncComponentTraits deserialization requires a trivially copyable quantized state");
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
        return true;
    }
};

template <typename T>
struct SyncCueTraits;

void register_components(ecs::Registry& registry);

const SyncComponentOps* find_component_ops(const ecs::Registry& registry, ecs::Entity component);
bool set_display_interpolated(ecs::Registry& registry, ecs::Entity component, bool enabled = true);
bool is_display_interpolated(const ecs::Registry& registry, ecs::Entity component);

template <typename T>
bool set_display_interpolated(ecs::Registry& registry, bool enabled = true) {
    return set_display_interpolated(registry, registry.component<T>(), enabled);
}

template <typename T>
bool is_display_interpolated(const ecs::Registry& registry) {
    return is_display_interpolated(registry, registry.component<T>());
}

namespace detail {

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
#endif

#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_COMPONENT_DATA)
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

}  // namespace detail

}  // namespace kage::sync

namespace ecs {

template <>
struct is_singleton_component<kage::sync::SyncSettings> : std::true_type {};

template <>
struct is_singleton_component<kage::sync::SyncAuthority> : std::true_type {};

}  // namespace ecs

namespace kage::sync {

namespace detail {

inline bool remove_prefix(std::string& value, const char* prefix) {
    const std::size_t length = std::char_traits<char>::length(prefix);
    if (value.compare(0U, length, prefix) != 0) {
        return false;
    }
    value.erase(0U, length);
    return true;
}

inline std::string short_type_name(std::string name) {
    (void)remove_prefix(name, "struct ");
    (void)remove_prefix(name, "class ");

    const std::string anonymous_namespace = "anonymous namespace";
    const std::size_t anonymous = name.find(anonymous_namespace);
    if (anonymous != std::string::npos) {
        std::size_t begin = anonymous + anonymous_namespace.size();
        while (begin < name.size() && (name[begin] == ')' || name[begin] == '}' ||
               name[begin] == '\'' || name[begin] == '`' || name[begin] == ':' ||
               name[begin] == ' ')) {
            ++begin;
        }
        name.erase(0U, begin);
    }

    const std::size_t scope = name.rfind("::");
    if (scope != std::string::npos) {
        name = name.substr(scope + 2U);
    }
    return name;
}

template <typename T>
std::string default_type_name() {
#if defined(__clang__) || defined(__GNUC__)
    std::string pretty = __PRETTY_FUNCTION__;
    const std::string marker = "T = ";
    const std::size_t begin = pretty.find(marker);
    if (begin != std::string::npos) {
        std::size_t end = pretty.find_first_of(";]", begin + marker.size());
        if (end == std::string::npos) {
            end = pretty.size();
        }
        return short_type_name(pretty.substr(begin + marker.size(), end - (begin + marker.size())));
    }
#endif
    return short_type_name(typeid(T).name());
}

}  // namespace detail

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
    ops.apply = [](ecs::Registry& registry, ecs::Entity entity, const std::uint8_t* quantized_bytes) {
        Quantized quantized{};
        std::memcpy(&quantized, quantized_bytes, sizeof(Quantized));
        return registry.add<T>(entity, Traits::dequantize(quantized)) != nullptr;
    };
    ops.serialize = [](
        const std::uint8_t* previous_bytes,
        const std::uint8_t* current_bytes,
        BitBuffer& out,
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
        BitBuffer& in,
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
    ops.serialize = [](const void* value, BitBuffer& out, EntityReferenceContext* references) {
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

void configure_server(ecs::Registry& registry);
void configure_client(ecs::Registry& registry, ClientId local_client);

SyncArchetypeId define_archetype(ecs::Registry& registry, SyncArchetypeDesc desc);
SyncArchetypeId define_archetype(
    ecs::Registry& registry,
    std::string name,
    std::vector<ComponentReplication> components);

const SyncArchetype* find_archetype(const ecs::Registry& registry, SyncArchetypeId id);

bool set_owner(ecs::Registry& registry, ecs::Entity entity, ClientId client);
bool set_client_input_component(ecs::Registry& registry, ecs::Entity component);

template <typename T>
bool set_client_input_component(ecs::Registry& registry) {
    register_components(registry);
    return set_client_input_component(registry, registry.component<T>());
}

}  // namespace kage::sync
