#pragma once

#include "game/components.hpp"
#include "game/cues.hpp"

#include "ashiato/sync/tracing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ashiato::sync {

using fps::FpsCombatState;
using fps::FpsDeathInfo;
using fps::FpsHitConfirmSuppression;
using fps::FpsHitEffect;
using fps::FpsHitSound;
using fps::FpsInput;
using fps::FpsShotEffect;
using fps::FpsSurfaceHitEffect;
using fps::FpsTransform;
using fps::FpsUniquePlayerId;
using fps::FpsVelocity;
using fps::FpsVisual;
using fps::HitConfirmCue;
using fps::PlayerDeathCue;
using fps::PlayerHitCue;
using fps::ShotCue;
using fps::SurfaceHitCue;
using fps::WallParticle;

namespace detail {

inline std::uint16_t encode_fps_signed_fixed_16(float value, float scale, int min_value, int max_value) {
    const int quantized = std::clamp(static_cast<int>(std::round(value * scale)), min_value, max_value);
    return static_cast<std::uint16_t>(quantized + 32768);
}

inline float decode_fps_signed_fixed_16(std::uint64_t bits, float scale) {
    const int quantized = static_cast<int>(static_cast<std::uint16_t>(bits)) - 32768;
    return static_cast<float>(quantized) / scale;
}

inline void serialize_fps_cue_position(Vector3 value, ashiato::BitBuffer& out) {
    constexpr float scale = 256.0f;
    out.push_unsigned_bits(encode_fps_signed_fixed_16(value.x, scale, -32768, 32767), 16U);
    out.push_unsigned_bits(encode_fps_signed_fixed_16(value.y, scale, -32768, 32767), 16U);
    out.push_unsigned_bits(encode_fps_signed_fixed_16(value.z, scale, -32768, 32767), 16U);
}

inline Vector3 deserialize_fps_cue_position(ashiato::BitBuffer& in) {
    constexpr float scale = 256.0f;
    return Vector3{
        decode_fps_signed_fixed_16(in.read_unsigned_bits(16U), scale),
        decode_fps_signed_fixed_16(in.read_unsigned_bits(16U), scale),
        decode_fps_signed_fixed_16(in.read_unsigned_bits(16U), scale)};
}

inline void serialize_fps_cue_normal(Vector3 value, ashiato::BitBuffer& out) {
    constexpr float scale = 32767.0f;
    out.push_unsigned_bits(encode_fps_signed_fixed_16(value.x, scale, -32767, 32767), 16U);
    out.push_unsigned_bits(encode_fps_signed_fixed_16(value.y, scale, -32767, 32767), 16U);
    out.push_unsigned_bits(encode_fps_signed_fixed_16(value.z, scale, -32767, 32767), 16U);
}

inline Vector3 deserialize_fps_cue_normal(ashiato::BitBuffer& in) {
    constexpr float scale = 32767.0f;
    return Vector3{
        std::clamp(decode_fps_signed_fixed_16(in.read_unsigned_bits(16U), scale), -1.0f, 1.0f),
        std::clamp(decode_fps_signed_fixed_16(in.read_unsigned_bits(16U), scale), -1.0f, 1.0f),
        std::clamp(decode_fps_signed_fixed_16(in.read_unsigned_bits(16U), scale), -1.0f, 1.0f)};
}

}  // namespace detail

template <>
struct SyncComponentTraits<FpsUniquePlayerId> {
    using Quantized = FpsUniquePlayerId;

    static Quantized quantize(const FpsUniquePlayerId& value) {
        return value;
    }

    static FpsUniquePlayerId dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.push_unsigned_bits(current.value, 64U);
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
        out.value = in.read_unsigned_bits(64U);
        return true;
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        TRACE_ROLLBACK_IF(predicted.value != authoritative.value, "FpsUniquePlayerId.value");
        return false;
    }
};

template <>
struct SyncComponentTraits<NetworkOwner> {
    using Quantized = NetworkOwner;

    static Quantized quantize(const NetworkOwner& value) {
        return value;
    }

    static NetworkOwner dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.push_unsigned_bits(current.client, 64U);
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
        out.client = static_cast<ClientId>(in.read_unsigned_bits(64U));
        return true;
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        TRACE_ROLLBACK_IF(predicted.client != authoritative.client, "NetworkOwner.client");
        return false;
    }
};

template <>
struct SyncComponentTraits<FpsTransform> {
    using Quantized = FpsTransform;
    using Error = FpsTransform;

    static Quantized quantize(const FpsTransform& value) {
        return value;
    }

    static FpsTransform dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(current));
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(out));
        return true;
    }

    static Quantized interpolate(const Quantized& from, const Quantized& to, float alpha) {
        auto interpolate_angle = [](float a, float b, float t) {
            constexpr float pi = 3.14159265358979323846f;
            constexpr float two_pi = pi * 2.0f;
            float delta = std::fmod(b - a + pi, two_pi);
            if (delta < 0.0f) {
                delta += two_pi;
            }
            delta -= pi;
            return a + delta * t;
        };
        return Quantized{
            from.x + (to.x - from.x) * alpha,
            from.y + (to.y - from.y) * alpha,
            from.z + (to.z - from.z) * alpha,
            interpolate_angle(from.yaw, to.yaw, alpha),
            from.pitch + (to.pitch - from.pitch) * alpha,
        };
    }

    static Error compute_error(const Quantized& current, const Quantized& previous) {
        return Error{
            previous.x - current.x,
            previous.y - current.y,
            previous.z - current.z,
            previous.yaw - current.yaw,
            previous.pitch - current.pitch,
        };
    }

    static Quantized apply_error(const Quantized& current, const Error& error) {
        return Quantized{
            current.x + error.x,
            current.y + error.y,
            current.z + error.z,
            current.yaw + error.yaw,
            current.pitch + error.pitch,
        };
    }

    static Error blend_out_error(const Error& error, float dt_seconds) {
        const float scale = std::exp(-18.0f * dt_seconds);
        return Error{error.x * scale, error.y * scale, error.z * scale, error.yaw * scale, error.pitch * scale};
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        const float dx = predicted.x - authoritative.x;
        const float dy = predicted.y - authoritative.y;
        const float dz = predicted.z - authoritative.z;
        const float dyaw = predicted.yaw - authoritative.yaw;
        const float dpitch = predicted.pitch - authoritative.pitch;
        TRACE_ROLLBACK_IF(dx * dx + dy * dy + dz * dz > 0.0001f, "FpsTransform.position");
        TRACE_ROLLBACK_IF(dyaw * dyaw + dpitch * dpitch > 0.0001f, "FpsTransform.view");
        return false;
    }

#ifdef ASHIATO_SYNC_ENABLE_TRACING
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("x=");
        out.append_number(value.x);
        out.append(" y=");
        out.append_number(value.y);
        out.append(" z=");
        out.append_number(value.z);
        out.append(" yaw=");
        out.append_number(value.yaw);
        out.append(" pitch=");
        out.append_number(value.pitch);
    }
#endif
};

template <>
struct SyncComponentTraits<FpsVelocity> {
    using Quantized = FpsVelocity;

    static Quantized quantize(const FpsVelocity& value) {
        return value;
    }

    static FpsVelocity dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(current));
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(out));
        return true;
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        const float dx = predicted.x - authoritative.x;
        const float dy = predicted.y - authoritative.y;
        const float dz = predicted.z - authoritative.z;
        TRACE_ROLLBACK_IF(dx * dx + dy * dy + dz * dz > 0.0001f, "FpsVelocity.linear");
        TRACE_ROLLBACK_IF(predicted.grounded != authoritative.grounded, "FpsVelocity.grounded");
        return false;
    }

#ifdef ASHIATO_SYNC_ENABLE_TRACING
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("x=");
        out.append_number(value.x);
        out.append(" y=");
        out.append_number(value.y);
        out.append(" z=");
        out.append_number(value.z);
        out.append(" grounded=");
        out.append_number(value.grounded);
    }
#endif
};

template <>
struct SyncComponentTraits<FpsCombatState> {
    using Quantized = FpsCombatState;

    static Quantized quantize(const FpsCombatState& value) {
        return value;
    }

    static FpsCombatState dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(current));
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(out));
        return true;
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        TRACE_ROLLBACK_IF(predicted.health != authoritative.health, "FpsCombatState.health");
        TRACE_ROLLBACK_IF(predicted.ammo != authoritative.ammo, "FpsCombatState.ammo");
        TRACE_ROLLBACK_IF(predicted.dead != authoritative.dead, "FpsCombatState.dead");
        TRACE_ROLLBACK_IF(
            predicted.reload_remaining != authoritative.reload_remaining,
            "FpsCombatState.reload_remaining");
        TRACE_ROLLBACK_IF(
            predicted.respawn_remaining != authoritative.respawn_remaining,
            "FpsCombatState.respawn_remaining");
        TRACE_ROLLBACK_IF(predicted.last_jump_seq != authoritative.last_jump_seq, "FpsCombatState.last_jump_seq");
        TRACE_ROLLBACK_IF(predicted.last_fire_seq != authoritative.last_fire_seq, "FpsCombatState.last_fire_seq");
        TRACE_ROLLBACK_IF(
            predicted.last_reload_seq != authoritative.last_reload_seq,
            "FpsCombatState.last_reload_seq");
        return false;
    }

#ifdef ASHIATO_SYNC_ENABLE_TRACING
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("health=");
        out.append_number(value.health);
        out.append(" ammo=");
        out.append_number(value.ammo);
        out.append(" dead=");
        out.append_number(value.dead);
        out.append(" reload=");
        out.append_number(value.reload_remaining);
        out.append(" respawn=");
        out.append_number(value.respawn_remaining);
        out.append(" jump=");
        out.append_number(value.last_jump_seq);
        out.append(" fire=");
        out.append_number(value.last_fire_seq);
        out.append(" reload_seq=");
        out.append_number(value.last_reload_seq);
    }
#endif
};

template <>
struct SyncComponentTraits<FpsDeathInfo> {
    using Quantized = FpsDeathInfo;

    static Quantized quantize(const FpsDeathInfo& value) {
        return value;
    }

    static FpsDeathInfo dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.push_unsigned_bits(current.killer, 64U);
        out.push_bits(current.sequence, 32U);
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
        out.killer = static_cast<ClientId>(in.read_unsigned_bits(64U));
        out.sequence = static_cast<std::uint32_t>(in.read_bits(32U));
        return true;
    }

    static bool should_roll_back(const Quantized&, const Quantized&) {
        return false;
    }

#ifdef ASHIATO_SYNC_ENABLE_TRACING
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("killer=");
        out.append_number(value.killer);
        out.append(" sequence=");
        out.append_number(value.sequence);
    }
#endif
};

template <>
struct SyncComponentTraits<FpsVisual> {
    using Quantized = FpsVisual;

    static Quantized quantize(const FpsVisual& value) {
        return value;
    }

    static FpsVisual dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(current));
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(out));
        return true;
    }

    static bool should_roll_back(const Quantized& predicted, const Quantized& authoritative) {
        TRACE_ROLLBACK_IF(predicted.radius != authoritative.radius, "FpsVisual.radius");
        TRACE_ROLLBACK_IF(predicted.height != authoritative.height, "FpsVisual.height");
        TRACE_ROLLBACK_IF(predicted.r != authoritative.r, "FpsVisual.r");
        TRACE_ROLLBACK_IF(predicted.g != authoritative.g, "FpsVisual.g");
        TRACE_ROLLBACK_IF(predicted.b != authoritative.b, "FpsVisual.b");
        TRACE_ROLLBACK_IF(predicted.a != authoritative.a, "FpsVisual.a");
        return false;
    }

#ifdef ASHIATO_SYNC_ENABLE_TRACING
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("radius=");
        out.append_number(value.radius);
        out.append(" height=");
        out.append_number(value.height);
        out.append(" rgba=");
        out.append_number(value.r);
        out.append(",");
        out.append_number(value.g);
        out.append(",");
        out.append_number(value.b);
        out.append(",");
        out.append_number(value.a);
    }
#endif
};

template <>
struct SyncComponentTraits<FpsInput> {
    using Quantized = FpsInput;

    static Quantized quantize(const FpsInput& value) {
        return value;
    }

    static FpsInput dequantize(const Quantized& value) {
        return value;
    }

    static void serialize(const Quantized*, const Quantized& current, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext&) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(current));
    }

    static bool deserialize(ashiato::BitBuffer& in, const Quantized*, Quantized& out, ashiato::ComponentSerializationContext&) {
        in.read_bytes(reinterpret_cast<char*>(&out), sizeof(out));
        return true;
    }

#ifdef ASHIATO_SYNC_ENABLE_TRACING
    static void trace(const Quantized& value, SyncTraceStringBuilder& out) {
        out.append("move=");
        out.append_number(value.move_x);
        out.append(",");
        out.append_number(value.move_y);
        out.append(" yaw=");
        out.append_number(value.yaw);
        out.append(" pitch=");
        out.append_number(value.pitch);
        out.append(" jump=");
        out.append_number(value.jump_seq);
        out.append(" fire=");
        out.append_number(value.fire_seq);
        out.append(" reload=");
        out.append_number(value.reload_seq);
        out.append(" shot_frame=");
        out.append_number(value.shot_interpolation_frame);
    }
#endif
};

template <>
struct SyncCueTraits<ShotCue> {
    static void serialize(const ShotCue&, ashiato::BitBuffer&, ashiato::ComponentSerializationContext&) {
    }

    static bool deserialize(ashiato::BitBuffer&, ShotCue&, ashiato::ComponentSerializationContext&) {
        return true;
    }

    static bool play(ashiato::Registry& registry, ashiato::Entity owner, const ShotCue&, float late_seconds) {
        const float seconds = std::max(0.03f, 0.12f - late_seconds);
        if (registry.contains<FpsShotEffect>(owner)) {
            FpsShotEffect& effect = registry.write<FpsShotEffect>(owner);
            effect.seconds = std::max(effect.seconds, seconds);
            return true;
        }
        registry.add<FpsShotEffect>(owner, FpsShotEffect{seconds, 0});
        return true;
    }

    static bool rollback(ashiato::Registry& registry, ashiato::Entity owner, const ShotCue&) {
        registry.remove<FpsShotEffect>(owner);
        return true;
    }

    static bool equals_cue(const ShotCue&, const ShotCue&) {
        return true;
    }
};

template <>
struct SyncCueTraits<SurfaceHitCue> {
    static void serialize(const SurfaceHitCue& cue, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext& context) {
        {
            ASHIATO_SERIALIZATION_TRACE_SCOPE("position");
            detail::serialize_fps_cue_position(cue.position, out);
        }
        {
            ASHIATO_SERIALIZATION_TRACE_SCOPE("normal");
            detail::serialize_fps_cue_normal(cue.normal, out);
        }
    }

    static bool deserialize(ashiato::BitBuffer& in, SurfaceHitCue& out, ashiato::ComponentSerializationContext& context) {
        {
            ASHIATO_SERIALIZATION_TRACE_SCOPE("position");
            out.position = detail::deserialize_fps_cue_position(in);
        }
        {
            ASHIATO_SERIALIZATION_TRACE_SCOPE("normal");
            out.normal = detail::deserialize_fps_cue_normal(in);
        }
        return true;
    }

    static bool play(ashiato::Registry& registry, ashiato::Entity owner, const SurfaceHitCue& cue, float late_seconds) {
        if (!registry.contains<FpsSurfaceHitEffect>(owner)) {
            registry.add<FpsSurfaceHitEffect>(owner);
        }
        if (!registry.contains<FpsSurfaceHitEffect>(owner)) {
            return false;
        }
        FpsSurfaceHitEffect& effect = registry.write<FpsSurfaceHitEffect>(owner);
        for (int i = 0; i < 10; ++i) {
            const float a = static_cast<float>(i) * 2.399f;
            Vector3 scatter{std::cos(a) * 0.8f, 0.4f + 0.07f * static_cast<float>(i), std::sin(a) * 0.8f};
            const Vector3 velocity{
                cue.normal.x * 1.8f + scatter.x,
                cue.normal.y * 1.8f + scatter.y,
                cue.normal.z * 1.8f + scatter.z};
            effect.particles.push_back(WallParticle{
                cue.position,
                velocity,
                std::max(0.03f, 0.35f - late_seconds)});
        }
        return true;
    }

    static bool rollback(ashiato::Registry& registry, ashiato::Entity owner, const SurfaceHitCue&) {
        registry.remove<FpsSurfaceHitEffect>(owner);
        return true;
    }

    static bool equals_cue(const SurfaceHitCue& lhs, const SurfaceHitCue& rhs) {
        const Vector3 delta{
            lhs.position.x - rhs.position.x,
            lhs.position.y - rhs.position.y,
            lhs.position.z - rhs.position.z};
        const float normal_dot =
            lhs.normal.x * rhs.normal.x + lhs.normal.y * rhs.normal.y + lhs.normal.z * rhs.normal.z;
        return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z <= 0.25f * 0.25f &&
            normal_dot >= 0.92f;
    }

#ifdef ASHIATO_SYNC_ENABLE_TRACING
    static void trace(const SurfaceHitCue& cue, SyncTraceStringBuilder& out) {
        out.append("position=");
        out.append_number(cue.position.x);
        out.append("/");
        out.append_number(cue.position.y);
        out.append("/");
        out.append_number(cue.position.z);
        out.append(" normal=");
        out.append_number(cue.normal.x);
        out.append("/");
        out.append_number(cue.normal.y);
        out.append("/");
        out.append_number(cue.normal.z);
    }
#endif
};

template <>
struct SyncCueTraits<PlayerHitCue> {
    static constexpr bool references_entities = true;

    static void serialize(const PlayerHitCue& cue, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext& context) {
        EntityReferenceContext& references = *static_cast<EntityReferenceContext*>(context.userContext);
        ASHIATO_SYNC_TRACE_SCOPE("shooter");
        (void)write_entity_reference(out, cue.shooter, references);
    }

    static bool deserialize(ashiato::BitBuffer& in, PlayerHitCue& out, ashiato::ComponentSerializationContext& context) {
        EntityReferenceContext& references = *static_cast<EntityReferenceContext*>(context.userContext);
        ASHIATO_SERIALIZATION_TRACE_SCOPE("shooter");
        return read_entity_reference(in, references, out.shooter);
    }

    static bool play(ashiato::Registry& registry, ashiato::Entity owner, const PlayerHitCue&, float late_seconds, SyncFrame frame) {
        bool suppressed = false;
        std::vector<ashiato::Entity> remove_empty;
        registry.view<FpsHitConfirmSuppression>().each([&suppressed, &remove_empty, frame, owner](ashiato::Entity entity, FpsHitConfirmSuppression& suppressions) {
            for (auto entry = suppressions.entries.begin(); entry != suppressions.entries.end();) {
                const bool match =
                    entry->frame == frame &&
                    (entry->victim == owner || (entry->victim && entry->victim.value == owner.value));
                if (match) {
                    suppressed = true;
                    entry = suppressions.entries.erase(entry);
                } else {
                    ++entry;
                }
            }
            if (suppressions.entries.empty()) {
                remove_empty.push_back(entity);
            }
        });
        for (ashiato::Entity entity : remove_empty) {
            registry.remove<FpsHitConfirmSuppression>(entity);
        }
        if (suppressed) {
            return true;
        }
        registry.add<FpsHitEffect>(
            owner,
            FpsHitEffect{std::max(0.04f, 0.25f - late_seconds), 0, FpsHitSound::TookDamage});
        return true;
    }

    static bool rollback(ashiato::Registry& registry, ashiato::Entity owner, const PlayerHitCue&) {
        registry.remove<FpsHitEffect>(owner);
        return true;
    }

    static bool equals_cue(const PlayerHitCue& lhs, const PlayerHitCue& rhs) {
        if (lhs.shooter.client_entity_network_id != ashiato::sync::invalid_client_entity_network_id ||
            rhs.shooter.client_entity_network_id != ashiato::sync::invalid_client_entity_network_id) {
            return lhs.shooter.client_entity_network_id == rhs.shooter.client_entity_network_id;
        }
        return lhs.shooter.entity == rhs.shooter.entity;
    }
};

template <>
struct SyncCueTraits<HitConfirmCue> {
    static constexpr bool references_entities = true;

    static void serialize(const HitConfirmCue& cue, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext& context) {
        EntityReferenceContext& references = *static_cast<EntityReferenceContext*>(context.userContext);
        ASHIATO_SYNC_TRACE_SCOPE("victim");
        (void)write_entity_reference(out, cue.victim, references);
    }

    static bool deserialize(ashiato::BitBuffer& in, HitConfirmCue& out, ashiato::ComponentSerializationContext& context) {
        EntityReferenceContext& references = *static_cast<EntityReferenceContext*>(context.userContext);
        ASHIATO_SERIALIZATION_TRACE_SCOPE("victim");
        return read_entity_reference(in, references, out.victim);
    }

    static bool play(ashiato::Registry& registry, ashiato::Entity owner, const HitConfirmCue& cue, float late_seconds, SyncFrame frame) {
        if (!cue.victim.entity || !registry.alive(cue.victim.entity)) {
            return true;
        }
        registry.add<FpsHitEffect>(
            cue.victim.entity,
            FpsHitEffect{std::max(0.04f, 0.25f - late_seconds), 0, FpsHitSound::None});
        registry.add<FpsHitEffect>(
            owner,
            FpsHitEffect{std::max(0.04f, 0.12f - late_seconds), 0, FpsHitSound::ConfirmedHit});
        if (!registry.contains<FpsHitConfirmSuppression>(owner)) {
            registry.add<FpsHitConfirmSuppression>(owner);
        }
        if (registry.contains<FpsHitConfirmSuppression>(owner)) {
            registry.write<FpsHitConfirmSuppression>(owner).entries.push_back(FpsHitConfirmSuppression::Entry{
                cue.victim.entity,
                cue.victim.client_entity_network_id,
                frame,
                1.0f});
        }
        return true;
    }

    static bool rollback(ashiato::Registry& registry, ashiato::Entity owner, const HitConfirmCue& cue) {
        if (cue.victim.entity) {
            registry.remove<FpsHitEffect>(cue.victim.entity);
        }
        registry.remove<FpsHitEffect>(owner);
        registry.remove<FpsHitConfirmSuppression>(owner);
        return true;
    }

    static bool equals_cue(const HitConfirmCue& lhs, const HitConfirmCue& rhs) {
        return lhs.victim.client_entity_network_id == rhs.victim.client_entity_network_id;
    }
};

template <>
struct SyncCueTraits<PlayerDeathCue> {
    static constexpr bool references_entities = true;

    static void serialize(const PlayerDeathCue& cue, ashiato::BitBuffer& out, ashiato::ComponentSerializationContext& context) {
        EntityReferenceContext& references = *static_cast<EntityReferenceContext*>(context.userContext);
        ASHIATO_SYNC_TRACE_SCOPE("shooter");
        (void)write_entity_reference(out, cue.shooter, references);
    }

    static bool deserialize(ashiato::BitBuffer& in, PlayerDeathCue& out, ashiato::ComponentSerializationContext& context) {
        EntityReferenceContext& references = *static_cast<EntityReferenceContext*>(context.userContext);
        ASHIATO_SERIALIZATION_TRACE_SCOPE("shooter");
        return read_entity_reference(in, references, out.shooter);
    }

    static bool play(ashiato::Registry& registry, ashiato::Entity owner, const PlayerDeathCue&, float late_seconds, SyncFrame) {
        registry.add<FpsHitEffect>(
            owner,
            FpsHitEffect{std::max(0.04f, 0.45f - late_seconds), 0, FpsHitSound::Died});
        return true;
    }

    static bool rollback(ashiato::Registry& registry, ashiato::Entity owner, const PlayerDeathCue&) {
        registry.remove<FpsHitEffect>(owner);
        return true;
    }

    static bool equals_cue(const PlayerDeathCue& lhs, const PlayerDeathCue& rhs) {
        if (lhs.shooter.client_entity_network_id != ashiato::sync::invalid_client_entity_network_id ||
            rhs.shooter.client_entity_network_id != ashiato::sync::invalid_client_entity_network_id) {
            return lhs.shooter.client_entity_network_id == rhs.shooter.client_entity_network_id;
        }
        return lhs.shooter.entity == rhs.shooter.entity;
    }
};

}  // namespace ashiato::sync
