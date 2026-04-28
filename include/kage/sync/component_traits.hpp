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
