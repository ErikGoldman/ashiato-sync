#pragma once

#include "kage/sync/types.hpp"

#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace kage::sync {

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
    const SyncComponentOps::QuantizedBytes& from_bytes,
    const SyncComponentOps::QuantizedBytes& to_bytes,
    float alpha,
    SyncComponentOps::QuantizedBytes& out) {
    if (from_bytes.size() != sizeof(Quantized) || to_bytes.size() != sizeof(Quantized)) {
        return false;
    }

    Quantized from{};
    Quantized to{};
    std::memcpy(&from, from_bytes.data(), sizeof(Quantized));
    std::memcpy(&to, to_bytes.data(), sizeof(Quantized));
    const Quantized interpolated = Traits::interpolate(from, to, alpha);

    out.resize(sizeof(Quantized));
    std::memcpy(out.data(), &interpolated, sizeof(Quantized));
    return true;
}

template <typename Traits, typename Quantized>
typename std::enable_if<!has_interpolate<Traits, Quantized>::value, bool>::type interpolate_quantized(
    const SyncComponentOps::QuantizedBytes&,
    const SyncComponentOps::QuantizedBytes&,
    float,
    SyncComponentOps::QuantizedBytes&) {
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
    const SyncComponentOps::QuantizedBytes& current_bytes,
    const SyncComponentOps::QuantizedBytes& previous_bytes,
    SyncComponentOps::QuantizedBytes& out) {
    using Error = typename Traits::Error;
    static_assert(
        std::is_trivially_copyable<Error>::value,
        "SyncComponentTraits<T>::Error must be trivially copyable");
    if (current_bytes.size() != sizeof(Quantized) || previous_bytes.size() != sizeof(Quantized)) {
        return false;
    }

    Quantized current{};
    Quantized previous{};
    std::memcpy(&current, current_bytes.data(), sizeof(Quantized));
    std::memcpy(&previous, previous_bytes.data(), sizeof(Quantized));
    const Error error = Traits::compute_error(current, previous);

    out.resize(sizeof(Error));
    std::memcpy(out.data(), &error, sizeof(Error));
    return true;
}

template <typename Traits, typename Quantized>
typename std::enable_if<!has_error_blending<Traits, Quantized>::value, bool>::type compute_error_quantized(
    const SyncComponentOps::QuantizedBytes&,
    const SyncComponentOps::QuantizedBytes&,
    SyncComponentOps::QuantizedBytes&) {
    return false;
}

template <typename Traits, typename Quantized>
typename std::enable_if<has_error_blending<Traits, Quantized>::value, bool>::type apply_error_quantized(
    const SyncComponentOps::QuantizedBytes& current_bytes,
    const SyncComponentOps::QuantizedBytes& error_bytes,
    SyncComponentOps::QuantizedBytes& out) {
    using Error = typename Traits::Error;
    if (current_bytes.size() != sizeof(Quantized) || error_bytes.size() != sizeof(Error)) {
        return false;
    }

    Quantized current{};
    Error error{};
    std::memcpy(&current, current_bytes.data(), sizeof(Quantized));
    std::memcpy(&error, error_bytes.data(), sizeof(Error));
    const Quantized display = Traits::apply_error(current, error);

    out.resize(sizeof(Quantized));
    std::memcpy(out.data(), &display, sizeof(Quantized));
    return true;
}

template <typename Traits, typename Quantized>
typename std::enable_if<!has_error_blending<Traits, Quantized>::value, bool>::type apply_error_quantized(
    const SyncComponentOps::QuantizedBytes&,
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

}  // namespace detail

}  // namespace kage::sync

namespace ecs {

template <>
struct is_singleton_component<kage::sync::SyncSettings> : std::true_type {};

}  // namespace ecs

namespace kage::sync {

template <typename T>
ecs::Entity register_sync_component(ecs::Registry& registry, std::string name = {}) {
    using Traits = SyncComponentTraits<T>;
    using Quantized = typename Traits::Quantized;
    static_assert(
        std::is_trivially_copyable<Quantized>::value,
        "SyncComponentTraits<T>::Quantized must be trivially copyable");

    register_components(registry);

    const ecs::Entity component = registry.register_component<T>(std::move(name));

    SyncComponentOps ops;
    ops.quantized_size = sizeof(Quantized);
    ops.quantize = [](const void* value, SyncComponentOps::QuantizedBytes& out) {
        const Quantized quantized = Traits::quantize(*static_cast<const T*>(value));
        out.resize(sizeof(Quantized));
        std::memcpy(out.data(), &quantized, sizeof(Quantized));
    };
    ops.dequantize = [](const SyncComponentOps::QuantizedBytes& quantized_bytes, void* out) {
        if (quantized_bytes.size() != sizeof(Quantized)) {
            return;
        }
        Quantized quantized{};
        std::memcpy(&quantized, quantized_bytes.data(), sizeof(Quantized));
        *static_cast<T*>(out) = Traits::dequantize(quantized);
    };
    ops.apply = [](ecs::Registry& registry, ecs::Entity entity, const SyncComponentOps::QuantizedBytes& quantized_bytes) {
        if (quantized_bytes.size() != sizeof(Quantized)) {
            return false;
        }
        Quantized quantized{};
        std::memcpy(&quantized, quantized_bytes.data(), sizeof(Quantized));
        return registry.add<T>(entity, Traits::dequantize(quantized)) != nullptr;
    };
    ops.serialize = [](const SyncComponentOps::QuantizedBytes* previous_bytes,
                       const SyncComponentOps::QuantizedBytes& current_bytes,
                       BitBuffer& out) {
        Quantized current{};
        std::memcpy(&current, current_bytes.data(), sizeof(Quantized));

        Quantized previous{};
        const Quantized* previous_ptr = nullptr;
        if (previous_bytes != nullptr && previous_bytes->size() == sizeof(Quantized)) {
            std::memcpy(&previous, previous_bytes->data(), sizeof(Quantized));
            previous_ptr = &previous;
        }

        Traits::serialize(previous_ptr, current, out);
    };
    ops.deserialize = [](BitBuffer& in,
                         const SyncComponentOps::QuantizedBytes* previous_bytes,
                         SyncComponentOps::QuantizedBytes& out) {
        Quantized previous{};
        const Quantized* previous_ptr = nullptr;
        if (previous_bytes != nullptr && previous_bytes->size() == sizeof(Quantized)) {
            std::memcpy(&previous, previous_bytes->data(), sizeof(Quantized));
            previous_ptr = &previous;
        }

        Quantized quantized{};
        if (!Traits::deserialize(in, previous_ptr, quantized)) {
            return false;
        }

        out.resize(sizeof(Quantized));
        std::memcpy(out.data(), &quantized, sizeof(Quantized));
        return true;
    };
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

    registry.write<SyncSettings>().component_ops[component.value] = ops;
    return component;
}

void configure_server(ecs::Registry& registry);
void configure_client(ecs::Registry& registry, ClientId local_client);

SyncArchetypeId define_archetype(
    ecs::Registry& registry,
    std::string name,
    std::vector<ComponentReplication> components);

const SyncArchetype* find_archetype(const ecs::Registry& registry, SyncArchetypeId id);

bool set_owner(ecs::Registry& registry, ecs::Entity entity, ClientId client);

}  // namespace kage::sync
