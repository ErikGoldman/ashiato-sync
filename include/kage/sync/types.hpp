#pragma once

#include "kage/sync/bit_buffer.hpp"

#include "ecs/ecs.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace kage::sync {

using ClientId = std::uint64_t;
using SyncFrame = std::uint32_t;
using TransportFn = std::function<void(ClientId, const BitBuffer&)>;

inline constexpr ClientId invalid_client_id = std::numeric_limits<ClientId>::max();

enum class SyncRole {
    Server,
    Client
};

struct SyncArchetypeId {
    std::uint32_t value = std::numeric_limits<std::uint32_t>::max();
};

inline constexpr SyncArchetypeId invalid_sync_archetype_id{};

constexpr bool operator==(SyncArchetypeId lhs, SyncArchetypeId rhs) noexcept {
    return lhs.value == rhs.value;
}

constexpr bool operator!=(SyncArchetypeId lhs, SyncArchetypeId rhs) noexcept {
    return !(lhs == rhs);
}

enum class ReplicationAudience {
    Owner,
    All
};

enum class ComponentInterpolation {
    Step,
    Interpolate
};

enum class ReplicationClientMode {
    Snap,
    BufferedInterpolation
};

struct ComponentReplication {
    ecs::Entity component;
    ReplicationAudience audience = ReplicationAudience::All;
    ComponentInterpolation interpolation = ComponentInterpolation::Step;
};

struct SyncArchetype {
    std::string name;
    std::vector<ComponentReplication> components;
};

struct SyncComponentOps {
    using QuantizedBytes = std::vector<std::uint8_t>;
    using QuantizeFn = void (*)(const void*, QuantizedBytes&);
    using DequantizeFn = void (*)(const QuantizedBytes&, void*);
    using ApplyFn = bool (*)(ecs::Registry&, ecs::Entity, const QuantizedBytes&);
    using SerializeFn = void (*)(const QuantizedBytes*, const QuantizedBytes&, BitBuffer&);
    using DeserializeFn = bool (*)(BitBuffer&, const QuantizedBytes*, QuantizedBytes&);
    using InterpolateFn = bool (*)(const QuantizedBytes&, const QuantizedBytes&, float, QuantizedBytes&);

    std::size_t quantized_size = 0;
    QuantizeFn quantize = nullptr;
    DequantizeFn dequantize = nullptr;
    ApplyFn apply = nullptr;
    SerializeFn serialize = nullptr;
    DeserializeFn deserialize = nullptr;
    InterpolateFn interpolate = nullptr;
};

struct SyncSettings {
    SyncRole role = SyncRole::Server;
    ClientId local_client = invalid_client_id;
    std::vector<SyncArchetype> archetypes;
    std::unordered_map<std::uint64_t, SyncComponentOps> component_ops;
};

struct Replicated {
    SyncArchetypeId archetype;
};

struct NetworkOwner {
    ClientId client = invalid_client_id;
};

struct ReplicationServerOptions {
    std::size_t bandwidth_limit_bytes_per_tick = 1024;
    std::size_t fixed_entity_replication_cost_bytes = 128;
    std::size_t mtu_bytes = 1200;
    TransportFn transport;
};

}  // namespace kage::sync
