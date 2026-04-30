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

struct SyncComponentOps {
    using QuantizedBytes = kage::sync::QuantizedBytes;
    using QuantizeFn = void (*)(const void*, std::uint8_t*);
    using DequantizeFn = void (*)(const std::uint8_t*, void*);
    using ApplyFn = bool (*)(ecs::Registry&, ecs::Entity, const std::uint8_t*);
    using SerializeFn = void (*)(const std::uint8_t*, const std::uint8_t*, BitBuffer&);
    using DeserializeFn = bool (*)(BitBuffer&, const std::uint8_t*, std::uint8_t*);
    using InterpolateFn = bool (*)(const std::uint8_t*, const std::uint8_t*, float, std::uint8_t*);
    using ComputeErrorFn = bool (*)(const std::uint8_t*, const std::uint8_t*, QuantizedBytes&);
    using ApplyErrorFn = bool (*)(const std::uint8_t*, const QuantizedBytes&, QuantizedBytes&);
    using BlendOutErrorFn = bool (*)(const QuantizedBytes&, float, QuantizedBytes&);
    using ShouldRollBackFn = bool (*)(const std::uint8_t*, const std::uint8_t*);

    std::size_t quantized_size = 0;
    std::size_t error_size = 0;
    QuantizeFn quantize = nullptr;
    DequantizeFn dequantize = nullptr;
    ApplyFn apply = nullptr;
    SerializeFn serialize = nullptr;
    DeserializeFn deserialize = nullptr;
    InterpolateFn interpolate = nullptr;
    ComputeErrorFn compute_error = nullptr;
    ApplyErrorFn apply_error = nullptr;
    BlendOutErrorFn blend_out_error = nullptr;
    ShouldRollBackFn should_roll_back = nullptr;
};

struct SyncCueOps {
    using SerializeFn = void (*)(const void*, BitBuffer&);
    using PlayFn = bool (*)(ecs::Registry&, ecs::Entity, const BitBuffer&, float);
    using RollbackFn = bool (*)(ecs::Registry&, ecs::Entity, const BitBuffer&);
    using EqualsFn = bool (*)(const BitBuffer&, const BitBuffer&);

    SerializeFn serialize = nullptr;
    PlayFn play = nullptr;
    RollbackFn rollback = nullptr;
    EqualsFn equals = nullptr;
};

struct QueuedSyncCue {
    ecs::Entity entity;
    SyncFrame frame = 0;
    SyncCueTypeId type = 0;
    float relevance_seconds = 0.0f;
    BitBuffer payload;
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
    std::vector<SyncArchetype> archetypes;
    std::unordered_map<std::uint64_t, SyncComponentOps> component_ops;
    std::vector<SyncCueOps> cue_ops;
    std::unordered_map<std::type_index, SyncCueTypeId> cue_type_ids;
    std::shared_ptr<SyncCueQueue> cue_queue = std::make_shared<SyncCueQueue>();
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
    SyncFrame prioritizer_interval_frames = 4;
    ReplicationPrioritizerFn prioritizer;
    ConnectHandlerFn connect_handler;
    TransportFn transport;
};

}  // namespace kage::sync
