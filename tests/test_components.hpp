#pragma once

#include "kage/sync/sync.hpp"

#include <cstdint>

namespace kage_sync_tests {

struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

struct Health {
    std::int32_t value = 100;
};

struct NetworkedPosition {
    float x = 0.0f;
    float y = 0.0f;
};

struct QuantizedNetworkedPosition {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct NetworkedPayload {
    bool delta = false;
    std::int64_t x = 0;
    std::int64_t y = 0;
};

struct SmoothPosition {
    float x = 0.0f;
    float y = 0.0f;
};

struct PredictedPosition {
    float x = 0.0f;
    float y = 0.0f;
};

struct QuantizedPredictedPosition {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct BandwidthProbe {
    std::int32_t value = 0;
};

struct TargetReference {
    kage::sync::EntityReference target;
};

struct Visible {};
struct Secret {};
struct TestCue {
    std::int32_t id = 0;
};

struct ReferenceCue {
    kage::sync::EntityReference target;
};

struct CuePlayback {
    std::int32_t plays = 0;
    std::int32_t rollbacks = 0;
    std::int32_t last_id = 0;
    ecs::Entity last_target;
    kage::sync::ClientEntityNetworkId last_target_network_id = kage::sync::invalid_client_entity_network_id;
    float last_late_seconds = 0.0f;
};

inline NetworkedPayload read_networked_payload(ecs::BitBuffer payload) {
    return NetworkedPayload{
        payload.read_bool(),
        payload.read_bits(8U),
        payload.read_bits(8U),
    };
}

inline kage::sync::SyncArchetypeId define_position_archetype(ecs::Registry& registry) {
    const ecs::Entity position_component =
        kage::sync::register_sync_component<Position>(registry, "Position");
    return kage::sync::define_archetype(
        registry,
        "PositionActor",
        {{position_component, kage::sync::ReplicationAudience::All}});
}

}  // namespace kage_sync_tests

namespace kage::sync {

template <>
struct SyncCueTraits<kage_sync_tests::TestCue> {
    static void serialize(const kage_sync_tests::TestCue& cue, ecs::BitBuffer& out) {
        out.push_bits(cue.id, 16U);
    }

    static bool deserialize(ecs::BitBuffer& in, kage_sync_tests::TestCue& out) {
        out.id = static_cast<std::int32_t>(in.read_bits(16U));
        return true;
    }

    static bool play(
        ecs::Registry& registry,
        ecs::Entity owner,
        const kage_sync_tests::TestCue& cue,
        float late_seconds) {
        if (!registry.contains<kage_sync_tests::CuePlayback>(owner)) {
            registry.add<kage_sync_tests::CuePlayback>(owner);
        }
        if (!registry.contains<kage_sync_tests::CuePlayback>(owner)) {
            return false;
        }
        kage_sync_tests::CuePlayback& playback = registry.write<kage_sync_tests::CuePlayback>(owner);
        ++playback.plays;
        playback.last_id = cue.id;
        playback.last_late_seconds = late_seconds;
        return true;
    }

    static bool rollback(ecs::Registry& registry, ecs::Entity owner, const kage_sync_tests::TestCue&) {
        if (!registry.contains<kage_sync_tests::CuePlayback>(owner)) {
            registry.add<kage_sync_tests::CuePlayback>(owner);
        }
        if (!registry.contains<kage_sync_tests::CuePlayback>(owner)) {
            return false;
        }
        ++registry.write<kage_sync_tests::CuePlayback>(owner).rollbacks;
        return true;
    }

    static bool equals_cue(const kage_sync_tests::TestCue& lhs, const kage_sync_tests::TestCue& rhs) {
        return lhs.id == rhs.id;
    }

#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_COMPONENT_DATA)
    static void trace(const kage_sync_tests::TestCue& cue, SyncTraceStringBuilder& out) {
        out.append("id=");
        out.append_number(cue.id);
    }
#endif
};

template <>
struct SyncCueTraits<kage_sync_tests::ReferenceCue> {
    static void serialize(
        const kage_sync_tests::ReferenceCue& cue,
        ecs::BitBuffer& out,
        EntityReferenceContext& references) {
        (void)write_entity_reference(out, cue.target, references);
    }

    static bool deserialize(
        ecs::BitBuffer& in,
        kage_sync_tests::ReferenceCue& out,
        EntityReferenceContext& references) {
        return read_entity_reference(in, references, out.target);
    }

    static bool play(
        ecs::Registry& registry,
        ecs::Entity owner,
        const kage_sync_tests::ReferenceCue& cue,
        float late_seconds) {
        if (!registry.contains<kage_sync_tests::CuePlayback>(owner)) {
            registry.add<kage_sync_tests::CuePlayback>(owner);
        }
        if (!registry.contains<kage_sync_tests::CuePlayback>(owner)) {
            return false;
        }
        kage_sync_tests::CuePlayback& playback = registry.write<kage_sync_tests::CuePlayback>(owner);
        ++playback.plays;
        playback.last_target = cue.target.entity;
        playback.last_target_network_id = cue.target.client_entity_network_id;
        playback.last_late_seconds = late_seconds;
        return true;
    }

    static bool rollback(ecs::Registry& registry, ecs::Entity owner, const kage_sync_tests::ReferenceCue&) {
        if (!registry.contains<kage_sync_tests::CuePlayback>(owner)) {
            registry.add<kage_sync_tests::CuePlayback>(owner);
        }
        if (!registry.contains<kage_sync_tests::CuePlayback>(owner)) {
            return false;
        }
        ++registry.write<kage_sync_tests::CuePlayback>(owner).rollbacks;
        return true;
    }

    static bool equals_cue(const kage_sync_tests::ReferenceCue& lhs, const kage_sync_tests::ReferenceCue& rhs) {
        return lhs.target.client_entity_network_id == rhs.target.client_entity_network_id &&
            lhs.target.entity == rhs.target.entity;
    }
};

template <>
struct SyncComponentTraits<kage_sync_tests::PredictedPosition> {
    using Quantized = kage_sync_tests::QuantizedPredictedPosition;
    using Error = kage_sync_tests::QuantizedPredictedPosition;

    static Quantized quantize(const kage_sync_tests::PredictedPosition& value) {
        return Quantized{
            static_cast<std::int32_t>(value.x * 10.0f),
            static_cast<std::int32_t>(value.y * 10.0f),
        };
    }

    static kage_sync_tests::PredictedPosition dequantize(const Quantized& value) {
        return kage_sync_tests::PredictedPosition{
            static_cast<float>(value.x) / 10.0f,
            static_cast<float>(value.y) / 10.0f,
        };
    }

    static void serialize(const Quantized*, const Quantized& current, ecs::BitBuffer& out) {
        out.push_bits(current.x, 16U);
        out.push_bits(current.y, 16U);
    }

    static bool deserialize(ecs::BitBuffer& in, const Quantized*, Quantized& out) {
        out.x = static_cast<std::int32_t>(in.read_bits(16U));
        out.y = static_cast<std::int32_t>(in.read_bits(16U));
        return true;
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        TRACE_ROLLBACK_IF(predicted.x != authoritative.x, "PredictedPosition.x mismatch");
        TRACE_ROLLBACK_IF(predicted.y != authoritative.y, "PredictedPosition.y mismatch");
        return false;
    }

#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_COMPONENT_DATA)
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("x=");
        out.append_number(value.x);
        out.append(",y=");
        out.append_number(value.y);
    }
#endif

    static Quantized interpolate(const Quantized& from, const Quantized& to, float alpha) {
        return Quantized{
            static_cast<std::int32_t>(static_cast<float>(from.x) + static_cast<float>(to.x - from.x) * alpha),
            static_cast<std::int32_t>(static_cast<float>(from.y) + static_cast<float>(to.y - from.y) * alpha),
        };
    }

    static Error compute_error(const Quantized& current, const Quantized& previous) {
        return Error{previous.x - current.x, previous.y - current.y};
    }

    static Quantized apply_error(const Quantized& current, const Error& error) {
        return Quantized{current.x + error.x, current.y + error.y};
    }

    static Error blend_out_error(const Error& error, float dt_seconds) {
        if (dt_seconds <= 0.0f) {
            return error;
        }
        if (dt_seconds >= 1.0f) {
            return Error{};
        }
        const float scale = 1.0f - dt_seconds;
        return Error{
            static_cast<std::int32_t>(static_cast<float>(error.x) * scale),
            static_cast<std::int32_t>(static_cast<float>(error.y) * scale),
        };
    }
};

template <>
struct SyncComponentTraits<kage_sync_tests::NetworkedPosition> {
    using Quantized = kage_sync_tests::QuantizedNetworkedPosition;

    static Quantized quantize(const kage_sync_tests::NetworkedPosition& value) {
        return Quantized{
            static_cast<std::int32_t>(value.x * 10.0f),
            static_cast<std::int32_t>(value.y * 10.0f),
        };
    }

    static kage_sync_tests::NetworkedPosition dequantize(const Quantized& value) {
        return kage_sync_tests::NetworkedPosition{
            static_cast<float>(value.x) / 10.0f,
            static_cast<float>(value.y) / 10.0f,
        };
    }

    static void serialize(const Quantized* previous, const Quantized& current, ecs::BitBuffer& out) {
        out.push_bool(previous != nullptr);
        const std::int32_t x = previous == nullptr ? current.x : current.x - previous->x;
        const std::int32_t y = previous == nullptr ? current.y : current.y - previous->y;
        out.push_bits(x, 8U);
        out.push_bits(y, 8U);
    }

    static bool deserialize(ecs::BitBuffer& in, const Quantized* previous, Quantized& out) {
        const bool delta = in.read_bool();
        const auto x = static_cast<std::int32_t>(in.read_bits(8U));
        const auto y = static_cast<std::int32_t>(in.read_bits(8U));
        if (!delta) {
            out = Quantized{x, y};
            return true;
        }
        if (previous != nullptr) {
            out = Quantized{previous->x + x, previous->y + y};
            return true;
        }
        return false;
    }

#if defined(KAGE_SYNC_ENABLE_TRACING) && defined(KAGE_SYNC_TRACE_COMPONENT_DATA)
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("x=");
        out.append_number(value.x);
        out.append(",y=");
        out.append_number(value.y);
    }
#endif
};

template <>
struct SyncComponentTraits<kage_sync_tests::SmoothPosition> {
    using Quantized = kage_sync_tests::SmoothPosition;
    using Error = kage_sync_tests::SmoothPosition;

    static Quantized quantize(const kage_sync_tests::SmoothPosition& value) {
        return value;
    }

    static kage_sync_tests::SmoothPosition dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ecs::BitBuffer& out) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(ecs::BitBuffer& in, const Quantized*, Quantized& out) {
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(Quantized));
        return true;
    }

    static Quantized interpolate(const Quantized& from, const Quantized& to, float alpha) {
        return Quantized{
            from.x + (to.x - from.x) * alpha,
            from.y + (to.y - from.y) * alpha,
        };
    }

    static Error compute_error(const Quantized& current, const Quantized& previous) {
        return Error{
            previous.x - current.x,
            previous.y - current.y,
        };
    }

    static Quantized apply_error(const Quantized& current, const Error& error) {
        return Quantized{
            current.x + error.x,
            current.y + error.y,
        };
    }

    static Error blend_out_error(const Error& error, float dt_seconds) {
        if (dt_seconds <= 0.0f) {
            return error;
        }
        if (dt_seconds >= 1.0f) {
            return Error{};
        }
        const float scale = 1.0f - dt_seconds;
        return Error{
            error.x * scale,
            error.y * scale,
        };
    }
};

template <>
struct SyncComponentTraits<kage_sync_tests::TargetReference> {
    using Quantized = kage_sync_tests::TargetReference;

    static Quantized quantize(const kage_sync_tests::TargetReference& value) {
        return value;
    }

    static kage_sync_tests::TargetReference dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(
        const Quantized*,
        const Quantized& current,
        ecs::BitBuffer& out,
        EntityReferenceContext& references) {
        (void)write_entity_reference(out, current.target, references);
    }

    static bool deserialize(
        ecs::BitBuffer& in,
        const Quantized*,
        Quantized& out,
        EntityReferenceContext& references) {
        return read_entity_reference(in, references, out.target);
    }
};

template <>
struct SyncComponentTraits<kage_sync_tests::BandwidthProbe> {
    using Quantized = std::int32_t;

    static Quantized quantize(const kage_sync_tests::BandwidthProbe& value) {
        return value.value;
    }

    static kage_sync_tests::BandwidthProbe dequantize(const Quantized& value) {
        return kage_sync_tests::BandwidthProbe{value};
    }

    static void serialize(const Quantized* previous, const Quantized& current, ecs::BitBuffer& out) {
        out.push_bool(previous != nullptr);
        if (previous == nullptr) {
            out.push_bits(current, 32U);
            return;
        }
        out.push_bits(current - *previous, 8U);
    }

    static bool deserialize(ecs::BitBuffer& in, const Quantized* previous, Quantized& out) {
        const bool delta = in.read_bool();
        if (!delta) {
            out = static_cast<std::int32_t>(in.read_bits(32U));
            return true;
        }
        if (previous == nullptr) {
            return false;
        }
        out = *previous + static_cast<std::int32_t>(in.read_bits(8U));
        return true;
    }
};

}  // namespace kage::sync
