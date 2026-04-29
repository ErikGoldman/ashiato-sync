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

struct BandwidthProbe {
    std::int32_t value = 0;
};

struct Visible {};
struct Secret {};

inline NetworkedPayload read_networked_payload(kage::sync::BitBuffer payload) {
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

    static void serialize(const Quantized* previous, const Quantized& current, BitBuffer& out) {
        out.push_bool(previous != nullptr);
        const std::int32_t x = previous == nullptr ? current.x : current.x - previous->x;
        const std::int32_t y = previous == nullptr ? current.y : current.y - previous->y;
        out.push_bits(x, 8U);
        out.push_bits(y, 8U);
    }

    static bool deserialize(BitBuffer& in, const Quantized* previous, Quantized& out) {
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

    static void serialize(const Quantized*, const Quantized& current, BitBuffer& out) {
        out.push_bytes(reinterpret_cast<const char*>(&current), sizeof(Quantized));
    }

    static bool deserialize(BitBuffer& in, const Quantized*, Quantized& out) {
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
struct SyncComponentTraits<kage_sync_tests::BandwidthProbe> {
    using Quantized = std::int32_t;

    static Quantized quantize(const kage_sync_tests::BandwidthProbe& value) {
        return value.value;
    }

    static kage_sync_tests::BandwidthProbe dequantize(const Quantized& value) {
        return kage_sync_tests::BandwidthProbe{value};
    }

    static void serialize(const Quantized* previous, const Quantized& current, BitBuffer& out) {
        out.push_bool(previous != nullptr);
        if (previous == nullptr) {
            out.push_bits(current, 32U);
            return;
        }
        out.push_bits(current - *previous, 8U);
    }

    static bool deserialize(BitBuffer& in, const Quantized* previous, Quantized& out) {
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
