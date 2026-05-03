#pragma once

#include "kage/sync/bit_buffer.hpp"
#include "kage/sync/protocol.hpp"

#include "ecs/ecs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace kage::sync {

using ClientId = std::uint64_t;
using SyncFrame = std::uint32_t;
using SyncCueTypeId = std::uint16_t;
using ClientEntityNetworkId = std::uint64_t;
using TransportFn = std::function<void(ClientId, const BitBuffer&)>;
using ConnectHandlerFn = std::function<bool(const std::string&, ClientId&, std::string&)>;

struct ReplicationPriorityObject {
    ecs::Entity entity;
};

struct ReplicationPriorityDecision {
    bool replicate = true;
    std::uint64_t priority = 0;
    std::uint64_t component_mask = std::numeric_limits<std::uint64_t>::max();
};

using ReplicationPrioritizerFn = std::function<void(
    ClientId,
    const std::vector<ReplicationPriorityObject>&,
    std::vector<ReplicationPriorityDecision>&)>;

inline constexpr ClientId invalid_client_id = std::numeric_limits<ClientId>::max();
inline constexpr ClientEntityNetworkId invalid_client_entity_network_id = 0;
inline constexpr ClientId max_client_entity_network_id_client = 0xfffULL;
inline constexpr std::uint32_t max_client_local_wire_network_id = (std::uint32_t{1} << 20U) - 1U;

inline constexpr ClientEntityNetworkId make_client_entity_network_id(
    ClientId client,
    std::uint32_t wire_network_id,
    std::uint32_t version) noexcept {
    return ((client & max_client_entity_network_id_client) << 52U) |
        ((static_cast<std::uint64_t>(wire_network_id) & 0xfffffULL) << 32U) |
        static_cast<std::uint64_t>(version);
}

inline constexpr ClientId client_entity_network_id_client(ClientEntityNetworkId id) noexcept {
    return (id >> 52U) & 0xfffULL;
}

inline constexpr std::uint32_t client_entity_network_id_wire_id(ClientEntityNetworkId id) noexcept {
    return static_cast<std::uint32_t>((id >> 32U) & 0xfffffULL);
}

inline constexpr std::uint32_t client_entity_network_id_version(ClientEntityNetworkId id) noexcept {
    return static_cast<std::uint32_t>(id & 0xffffffffULL);
}

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
    BufferedInterpolation,
    Predict
};

enum class ReplicationRollbackPolicy {
    All,
    OnlyAffected
};

#ifdef KAGE_SYNC_ENABLE_TRACING
struct SyncTraceStringBuilder {
    std::string value;

    void clear() noexcept {
        value.clear();
    }

    void append(const char* text) {
        if (text != nullptr) {
            value += text;
        }
    }

    void append(const std::string& text) {
        value += text;
    }

    template <typename T>
    void append_number(T number) {
        value += std::to_string(number);
    }
};
#endif

struct ComponentReplication {
    ecs::Entity component;
    ReplicationAudience audience = ReplicationAudience::All;
    ComponentInterpolation interpolation = ComponentInterpolation::Step;
};

struct SyncTagReplication {
    ecs::Entity tag;
    ReplicationAudience audience = ReplicationAudience::All;
};

struct SyncArchetypeDesc {
    std::string name;
    std::vector<SyncTagReplication> tags;
    std::vector<ComponentReplication> components;
};

class QuantizedBytes {
public:
    static constexpr std::size_t inline_capacity = 16;
    static constexpr std::size_t max_size = 1200;

    QuantizedBytes() = default;

    QuantizedBytes(const QuantizedBytes& other)
        : size_(other.size_),
          inline_(other.inline_) {
        if (other.overflow_) {
            overflow_ = std::make_unique<Overflow>(*other.overflow_);
        }
    }

    QuantizedBytes& operator=(const QuantizedBytes& other) {
        if (this != &other) {
            size_ = other.size_;
            inline_ = other.inline_;
            if (other.overflow_) {
                overflow_ = std::make_unique<Overflow>(*other.overflow_);
            } else {
                overflow_.reset();
            }
        }
        return *this;
    }

    QuantizedBytes(QuantizedBytes&&) noexcept = default;
    QuantizedBytes& operator=(QuantizedBytes&&) noexcept = default;

    std::size_t size() const noexcept {
        return size_;
    }

    bool empty() const noexcept {
        return size_ == 0;
    }

    std::uint8_t* data() noexcept {
        return overflow_ == nullptr ? inline_.data() : overflow_->data();
    }

    const std::uint8_t* data() const noexcept {
        return overflow_ == nullptr ? inline_.data() : overflow_->data();
    }

    std::uint8_t* begin() noexcept {
        return data();
    }

    std::uint8_t* end() noexcept {
        return data() + size_;
    }

    const std::uint8_t* begin() const noexcept {
        return data();
    }

    const std::uint8_t* end() const noexcept {
        return data() + size_;
    }

    void clear() noexcept {
        size_ = 0;
    }

    void resize(std::size_t size) {
        if (size > max_size) {
            throw std::length_error("sync quantized component payload exceeds maximum size");
        }
        size_ = size;
        if (size <= inline_capacity) {
            overflow_.reset();
            return;
        }
        if (overflow_ == nullptr) {
            overflow_ = std::make_unique<Overflow>();
        }
    }

    void assign(const void* data, std::size_t size) {
        resize(size);
        if (size != 0U) {
            std::memcpy(this->data(), data, size);
        }
    }

    friend bool operator==(const QuantizedBytes& lhs, const QuantizedBytes& rhs) noexcept {
        return lhs.size_ == rhs.size_ && std::equal(lhs.data(), lhs.data() + lhs.size_, rhs.data());
    }

    friend bool operator!=(const QuantizedBytes& lhs, const QuantizedBytes& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    using Overflow = std::array<std::uint8_t, max_size>;

    std::size_t size_ = 0;
    std::array<std::uint8_t, inline_capacity> inline_{};
    std::unique_ptr<Overflow> overflow_;
};

struct EntityReference {
    ecs::Entity entity;
    ClientEntityNetworkId client_entity_network_id = invalid_client_entity_network_id;
};

struct EntityReferenceContext {
    using ServerNetworkIdForEntityFn = std::uint32_t (*)(void*, ecs::Entity);
    using ClientEntityNetworkIdForWireFn = ClientEntityNetworkId (*)(void*, std::uint32_t);
    using ClientLocalEntityFn = ecs::Entity (*)(void*, ClientEntityNetworkId);

    void* user = nullptr;
    std::size_t network_entity_id_tier0_bits = protocol::default_network_entity_id_tier0_bits;
    ServerNetworkIdForEntityFn server_network_id_for_entity = nullptr;
    ClientEntityNetworkIdForWireFn client_entity_network_id_for_wire = nullptr;
    ClientLocalEntityFn client_local_entity = nullptr;

    std::uint32_t network_id_for_entity(ecs::Entity entity) const {
        if (!entity || server_network_id_for_entity == nullptr) {
            return 0U;
        }
        return server_network_id_for_entity(user, entity);
    }

    ClientEntityNetworkId network_id_for_wire(std::uint32_t wire_network_id) const {
        if (wire_network_id == 0U || client_entity_network_id_for_wire == nullptr) {
            return invalid_client_entity_network_id;
        }
        return client_entity_network_id_for_wire(user, wire_network_id);
    }

    ecs::Entity local_entity(ClientEntityNetworkId network_id) const {
        if (network_id == invalid_client_entity_network_id || client_local_entity == nullptr) {
            return ecs::Entity{};
        }
        return client_local_entity(user, network_id);
    }
};

inline bool write_entity_reference(
    BitBuffer& out,
    ecs::Entity entity,
    const EntityReferenceContext& context) {
    const std::uint32_t network_id = context.network_id_for_entity(entity);
    out.push_bool(network_id != 0U);
    if (network_id == 0U) {
        return false;
    }
    protocol::write_network_entity_id(out, network_id, context.network_entity_id_tier0_bits);
    return true;
}

inline bool write_entity_reference(
    BitBuffer& out,
    const EntityReference& reference,
    const EntityReferenceContext& context) {
    return write_entity_reference(out, reference.entity, context);
}

inline bool read_entity_reference(
    BitBuffer& in,
    EntityReferenceContext& context,
    EntityReference& out) {
    if (in.remaining_bits() < 1U) {
        return false;
    }
    const bool has_reference = in.read_bool();
    if (!has_reference) {
        out = EntityReference{};
        return true;
    }

    std::uint32_t wire_network_id = 0;
    if (!protocol::read_network_entity_id(in, wire_network_id, context.network_entity_id_tier0_bits) ||
        wire_network_id == 0U) {
        return false;
    }

    const ClientEntityNetworkId client_network_id = context.network_id_for_wire(wire_network_id);
    if (client_network_id == invalid_client_entity_network_id) {
        return false;
    }
    out.client_entity_network_id = client_network_id;
    out.entity = context.local_entity(client_network_id);
    return true;
}

struct SyncComponentOps {
    using QuantizedBytes = kage::sync::QuantizedBytes;
    using QuantizeFn = void (*)(const void*, std::uint8_t*);
    using DequantizeFn = void (*)(const std::uint8_t*, void*);
    using ApplyFn = bool (*)(ecs::Registry&, ecs::Entity, const std::uint8_t*);
    using SerializeFn = void (*)(const std::uint8_t*, const std::uint8_t*, BitBuffer&, EntityReferenceContext*);
    using DeserializeFn = bool (*)(BitBuffer&, const std::uint8_t*, std::uint8_t*, EntityReferenceContext*);
    using InterpolateFn = bool (*)(const std::uint8_t*, const std::uint8_t*, float, std::uint8_t*);
    using ComputeErrorFn = bool (*)(const std::uint8_t*, const std::uint8_t*, QuantizedBytes&);
    using ApplyErrorFn = bool (*)(const std::uint8_t*, const QuantizedBytes&, QuantizedBytes&);
    using BlendOutErrorFn = bool (*)(const QuantizedBytes&, float, QuantizedBytes&);
    using ShouldRollBackFn = bool (*)(const std::uint8_t*, const std::uint8_t*);
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_COMPONENT_DATA)
    using TraceFn = void (*)(const std::uint8_t*, SyncTraceStringBuilder&);
#endif

    std::string name;
    std::size_t quantized_size = 0;
    std::size_t error_size = 0;
    QuantizeFn quantize = nullptr;
    DequantizeFn dequantize = nullptr;
    ApplyFn apply = nullptr;
    SerializeFn serialize = nullptr;
    DeserializeFn deserialize = nullptr;
    bool references_entities = false;
    InterpolateFn interpolate = nullptr;
    ComputeErrorFn compute_error = nullptr;
    ApplyErrorFn apply_error = nullptr;
    BlendOutErrorFn blend_out_error = nullptr;
    ShouldRollBackFn should_roll_back = nullptr;
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_COMPONENT_DATA)
    TraceFn trace = nullptr;
#endif
};

struct SyncCueOps {
    using SerializeFn = void (*)(const void*, BitBuffer&, EntityReferenceContext*);
    using PlayFn = bool (*)(ecs::Registry&, ecs::Entity, const BitBuffer&, float, SyncFrame, EntityReferenceContext*);
    using RollbackFn = bool (*)(ecs::Registry&, ecs::Entity, const BitBuffer&, EntityReferenceContext*);
    using EqualsFn = bool (*)(const BitBuffer&, const BitBuffer&, EntityReferenceContext*);
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_COMPONENT_DATA)
    using TraceFn = bool (*)(const BitBuffer&, SyncTraceStringBuilder&);
#endif

    SerializeFn serialize = nullptr;
    PlayFn play = nullptr;
    RollbackFn rollback = nullptr;
    EqualsFn equals = nullptr;
    std::string name;
    bool references_entities = false;
#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_COMPONENT_DATA)
    TraceFn trace = nullptr;
#endif
};

struct QueuedSyncCue {
    ecs::Entity entity;
    SyncFrame frame = 0;
    SyncCueTypeId type = 0;
    float relevance_seconds = 0.0f;
    BitBuffer payload;
    std::shared_ptr<void> value;
    bool only_replicate_to_owner = false;
};

struct SyncCueQueue {
    mutable std::mutex mutex;
    std::vector<QueuedSyncCue> cues;
};

struct SyncArchetype {
    std::string name;
    std::vector<SyncTagReplication> tags;
    std::vector<ComponentReplication> components;
    std::vector<SyncComponentOps> component_ops;
    std::vector<std::uint32_t> component_offsets;
    std::uint32_t total_quantized_bytes = 0;
};

struct QuantizedFrameData {
    std::uint64_t tag_mask = 0;
    std::uint64_t present_mask = 0;
    std::vector<std::uint8_t> bytes;

    void clear() {
        tag_mask = 0;
        present_mask = 0;
        bytes.clear();
    }
};

struct SyncSettings {
    SyncRole role = SyncRole::Server;
    ClientId local_client = invalid_client_id;
    ecs::Entity input_component;
    std::vector<SyncArchetype> archetypes;
    std::unordered_map<std::uint64_t, SyncComponentOps> component_ops;
    std::vector<SyncCueOps> cue_ops;
    std::unordered_map<std::type_index, SyncCueTypeId> cue_type_ids;
    std::shared_ptr<SyncCueQueue> cue_queue = std::make_shared<SyncCueQueue>();
};

struct SyncAuthority {
    bool authoritative = true;

    bool is_authoritative() const noexcept {
        return authoritative;
    }
};

struct Replicated {
    SyncArchetypeId archetype;
};

struct NetworkOwner {
    ClientId client = invalid_client_id;
};

struct DisplayInterpolated {};
struct NoResim {};
struct NoSimulate {};

struct ReplicationServerOptions {
    std::size_t bandwidth_limit_bytes_per_tick = 1024;
    std::size_t fixed_entity_replication_cost_bytes = 128;
    std::size_t mtu_bytes = 1200;
    std::size_t serialized_worker_threads = 1;
    std::size_t max_pending_packet_acks_per_client = protocol::default_max_pending_packet_acks_per_client;
    double fixed_dt_seconds = 1.0 / 60.0;
    double connect_resend_interval_seconds = 0.25;
    double idle_client_timeout_seconds = 0.0;
    std::size_t network_entity_id_tier0_bits = protocol::default_network_entity_id_tier0_bits;
    std::size_t input_buffer_capacity_frames = 64;
    SyncFrame prioritizer_interval_frames = 4;
    ReplicationPrioritizerFn prioritizer;
    ConnectHandlerFn connect_handler;
    TransportFn transport;
};

}  // namespace kage::sync
