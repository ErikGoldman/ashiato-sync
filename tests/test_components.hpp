#pragma once

#include "ashiato/sync/sync.hpp"
#include "test_setup.hpp"

#include <cstdint>

namespace ashiato_sync_tests {

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
    ashiato::sync::EntityReference target;
};

struct Visible {};
struct Secret {};
struct TestCue {
    std::int32_t id = 0;
};

struct ReferenceCue {
    ashiato::sync::EntityReference target;
};

struct CuePlayback {
    std::int32_t plays = 0;
    std::int32_t rollbacks = 0;
    std::int32_t last_id = 0;
    ashiato::sync::SyncFrame last_frame = 0;
    ashiato::Entity last_target;
    ashiato::sync::ClientEntityNetworkId last_target_network_id = ashiato::sync::invalid_client_entity_network_id;
    float last_late_seconds = 0.0f;
};

inline NetworkedPayload read_networked_payload(ashiato::BitBuffer payload) {
    return NetworkedPayload{
        payload.read_bool(),
        payload.read_bits(8U),
        payload.read_bits(8U),
    };
}

inline ashiato::sync::SyncArchetypeId define_position_archetype(ashiato::Registry& registry) {
    const ashiato::Entity position_component =
        ashiato::sync::register_sync_component<Position>(registry, "Position");
    return ashiato::sync::define_archetype(
        registry,
        "PositionActor",
        {{position_component, ashiato::sync::ReplicationAudience::All}});
}

template <typename T>
bool emit_test_cue(
    ashiato::Registry& registry,
    ashiato::Entity entity,
    ashiato::sync::SyncFrame frame,
    const T& cue,
    float relevance_seconds,
    bool only_replicate_to_owner = false) {
    ashiato::sync::register_components(registry);
    if (!registry.alive(entity)) {
        return false;
    }
    return registry.write<ashiato::sync::CueDispatcher>().emit(
        registry.get<ashiato::sync::SyncSettings>(),
        frame,
        entity,
        cue,
        relevance_seconds,
        only_replicate_to_owner);
}

template <typename T>
bool emit_test_cue(
    ashiato::Registry& registry,
    ashiato::Entity entity,
    const T& cue,
    float relevance_seconds,
    bool only_replicate_to_owner = false) {
    ashiato::sync::register_components(registry);
    if (!registry.alive(entity)) {
        return false;
    }
    return registry.write<ashiato::sync::CueDispatcher>().emit(
        registry.get<ashiato::sync::SyncSettings>(),
        registry.get<ashiato::sync::FrameInfo>(),
        entity,
        cue,
        relevance_seconds,
        only_replicate_to_owner);
}

}  // namespace ashiato_sync_tests

namespace ashiato::sync {

template <>
struct SyncCueTraits<ashiato_sync_tests::TestCue> {
    static void serialize(
        const ashiato_sync_tests::TestCue& cue,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext& context) {
        ASHIATO_SERIALIZE_TRACE(out, cue.id, 16U, "id");
    }

    static bool deserialize(
        ashiato::BitBuffer& in,
        ashiato_sync_tests::TestCue& out,
        ashiato::ComponentSerializationContext& context) {
        ASHIATO_SERIALIZATION_TRACE_SCOPE("id");
        out.id = static_cast<std::int32_t>(in.read_bits(16U));
        return true;
    }

    static bool play(
        ashiato::Registry& registry,
        ashiato::Entity owner,
        const ashiato_sync_tests::TestCue& cue,
        float late_seconds,
        SyncFrame frame) {
        if (!registry.contains<ashiato_sync_tests::CuePlayback>(owner)) {
            registry.add<ashiato_sync_tests::CuePlayback>(owner);
        }
        if (!registry.contains<ashiato_sync_tests::CuePlayback>(owner)) {
            return false;
        }
        ashiato_sync_tests::CuePlayback& playback = registry.write<ashiato_sync_tests::CuePlayback>(owner);
        ++playback.plays;
        playback.last_id = cue.id;
        playback.last_frame = frame;
        playback.last_late_seconds = late_seconds;
        return true;
    }

    static bool rollback(ashiato::Registry& registry, ashiato::Entity owner, const ashiato_sync_tests::TestCue&) {
        if (!registry.contains<ashiato_sync_tests::CuePlayback>(owner)) {
            registry.add<ashiato_sync_tests::CuePlayback>(owner);
        }
        if (!registry.contains<ashiato_sync_tests::CuePlayback>(owner)) {
            return false;
        }
        ++registry.write<ashiato_sync_tests::CuePlayback>(owner).rollbacks;
        return true;
    }

    static bool equals_cue(const ashiato_sync_tests::TestCue& lhs, const ashiato_sync_tests::TestCue& rhs) {
        return lhs.id == rhs.id;
    }

#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
    static void trace(const ashiato_sync_tests::TestCue& cue, SyncTraceStringBuilder& out) {
        out.append("id=");
        out.append_number(cue.id);
    }
#endif
};

template <>
struct SyncCueTraits<ashiato_sync_tests::ReferenceCue> {
    static constexpr bool references_entities = true;

    static void serialize(
        const ashiato_sync_tests::ReferenceCue& cue,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext& context) {
        EntityReferenceContext& references = *static_cast<EntityReferenceContext*>(context.userContext);
        ASHIATO_SYNC_TRACE_SCOPE("target");
        (void)write_entity_reference(out, cue.target, references);
    }

    static bool deserialize(
        ashiato::BitBuffer& in,
        ashiato_sync_tests::ReferenceCue& out,
        ashiato::ComponentSerializationContext& context) {
        EntityReferenceContext& references = *static_cast<EntityReferenceContext*>(context.userContext);
        ASHIATO_SERIALIZATION_TRACE_SCOPE("target");
        return read_entity_reference(in, references, out.target);
    }

    static bool play(
        ashiato::Registry& registry,
        ashiato::Entity owner,
        const ashiato_sync_tests::ReferenceCue& cue,
        float late_seconds) {
        if (!registry.contains<ashiato_sync_tests::CuePlayback>(owner)) {
            registry.add<ashiato_sync_tests::CuePlayback>(owner);
        }
        if (!registry.contains<ashiato_sync_tests::CuePlayback>(owner)) {
            return false;
        }
        ashiato_sync_tests::CuePlayback& playback = registry.write<ashiato_sync_tests::CuePlayback>(owner);
        ++playback.plays;
        playback.last_target = cue.target.entity;
        playback.last_target_network_id = cue.target.client_entity_network_id;
        playback.last_late_seconds = late_seconds;
        return true;
    }

    static bool rollback(ashiato::Registry& registry, ashiato::Entity owner, const ashiato_sync_tests::ReferenceCue&) {
        if (!registry.contains<ashiato_sync_tests::CuePlayback>(owner)) {
            registry.add<ashiato_sync_tests::CuePlayback>(owner);
        }
        if (!registry.contains<ashiato_sync_tests::CuePlayback>(owner)) {
            return false;
        }
        ++registry.write<ashiato_sync_tests::CuePlayback>(owner).rollbacks;
        return true;
    }

    static bool equals_cue(const ashiato_sync_tests::ReferenceCue& lhs, const ashiato_sync_tests::ReferenceCue& rhs) {
        return lhs.target.client_entity_network_id == rhs.target.client_entity_network_id &&
            lhs.target.entity == rhs.target.entity;
    }
};

template <>
struct SyncComponentTraits<ashiato_sync_tests::PredictedPosition> {
    using Quantized = ashiato_sync_tests::QuantizedPredictedPosition;
    using Error = ashiato_sync_tests::QuantizedPredictedPosition;

    static Quantized quantize(const ashiato_sync_tests::PredictedPosition& value) {
        return Quantized{
            static_cast<std::int32_t>(value.x * 10.0f),
            static_cast<std::int32_t>(value.y * 10.0f),
        };
    }

    static ashiato_sync_tests::PredictedPosition dequantize(const Quantized& value) {
        return ashiato_sync_tests::PredictedPosition{
            static_cast<float>(value.x) / 10.0f,
            static_cast<float>(value.y) / 10.0f,
        };
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.write_bits(current.x, 16U);
        out.write_bits(current.y, 16U);
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
        out.x = static_cast<std::int32_t>(in.read_bits(16U));
        out.y = static_cast<std::int32_t>(in.read_bits(16U));
        return true;
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        TRACE_ROLLBACK_IF(predicted.x != authoritative.x, "PredictedPosition.x mismatch");
        TRACE_ROLLBACK_IF(predicted.y != authoritative.y, "PredictedPosition.y mismatch");
        return false;
    }

#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
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
struct SyncComponentTraits<ashiato_sync_tests::NetworkedPosition> {
    using Quantized = ashiato_sync_tests::QuantizedNetworkedPosition;

    static Quantized quantize(const ashiato_sync_tests::NetworkedPosition& value) {
        return Quantized{
            static_cast<std::int32_t>(value.x * 10.0f),
            static_cast<std::int32_t>(value.y * 10.0f),
        };
    }

    static ashiato_sync_tests::NetworkedPosition dequantize(const Quantized& value) {
        return ashiato_sync_tests::NetworkedPosition{
            static_cast<float>(value.x) / 10.0f,
            static_cast<float>(value.y) / 10.0f,
        };
    }

    static void serialize(const Quantized* previous, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.write_bool(previous != nullptr);
        const std::int32_t x = previous == nullptr ? current.x : current.x - previous->x;
        const std::int32_t y = previous == nullptr ? current.y : current.y - previous->y;
        out.write_bits(x, 8U);
        out.write_bits(y, 8U);
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized* previous, Quantized& out, ashiato::ComponentSerializationContext&) {
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

#if defined(ASHIATO_SYNC_ENABLE_TRACING) && defined(ASHIATO_SYNC_TRACE_COMPONENT_DATA)
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("x=");
        out.append_number(value.x);
        out.append(",y=");
        out.append_number(value.y);
    }
#endif
};

template <>
struct SyncComponentTraits<ashiato_sync_tests::SmoothPosition> {
    using Quantized = ashiato_sync_tests::SmoothPosition;
    using Error = ashiato_sync_tests::SmoothPosition;

    static Quantized quantize(const ashiato_sync_tests::SmoothPosition& value) {
        return value;
    }

    static ashiato_sync_tests::SmoothPosition dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.write_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
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
struct SyncComponentTraits<ashiato_sync_tests::TargetReference> {
    using Quantized = ashiato_sync_tests::TargetReference;
    static constexpr bool references_entities = true;

    static Quantized quantize(const ashiato_sync_tests::TargetReference& value) {
        return value;
    }

    static ashiato_sync_tests::TargetReference dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(
        const Quantized*,
        const Quantized& current,
        ashiato::BitBuffer& out,
        ashiato::ComponentSerializationContext& context) {
        EntityReferenceContext& references = *static_cast<EntityReferenceContext*>(context.userContext);
        (void)write_entity_reference(out, current.target, references);
    }

    static bool deserialize(
        ashiato::BitBuffer& in,
        const Quantized*,
        Quantized& out,
        ashiato::ComponentSerializationContext& context) {
        EntityReferenceContext& references = *static_cast<EntityReferenceContext*>(context.userContext);
        return read_entity_reference(in, references, out.target);
    }
};

template <>
struct SyncComponentTraits<ashiato_sync_tests::BandwidthProbe> {
    using Quantized = std::int32_t;

    static Quantized quantize(const ashiato_sync_tests::BandwidthProbe& value) {
        return value.value;
    }

    static ashiato_sync_tests::BandwidthProbe dequantize(const Quantized& value) {
        return ashiato_sync_tests::BandwidthProbe{value};
    }

    static void serialize(const Quantized* previous, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.write_bool(previous != nullptr);
        if (previous == nullptr) {
            out.write_bits(current, 32U);
            return;
        }
        out.write_bits(current - *previous, 8U);
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized* previous, Quantized& out, ashiato::ComponentSerializationContext&) {
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

}  // namespace ashiato::sync
