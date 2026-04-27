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

}  // namespace kage::sync
