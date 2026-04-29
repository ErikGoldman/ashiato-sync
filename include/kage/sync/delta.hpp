#pragma once

#include "kage/sync/bit_buffer.hpp"
#include "kage/sync/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace kage::sync::delta {

struct FloatConfig {
    float min = 0.0f;
    float max = 1.0f;
    float precision = 0.01f;
};

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

inline void validate(FloatConfig config) {
    if (!std::isfinite(config.min) ||
        !std::isfinite(config.max) ||
        !std::isfinite(config.precision) ||
        config.max <= config.min ||
        config.precision <= 0.0f) {
        throw std::invalid_argument("invalid delta float config");
    }
}

inline std::uint32_t quantized_float_steps(FloatConfig config) {
    validate(config);
    const double range = static_cast<double>(config.max) - static_cast<double>(config.min);
    const double steps = std::ceil(range / static_cast<double>(config.precision));
    if (steps > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::out_of_range("delta float config requires more than 32 bits");
    }
    return static_cast<std::uint32_t>(steps);
}

inline std::size_t float_bits(FloatConfig config) {
    return protocol::bits_for_range(static_cast<std::size_t>(quantized_float_steps(config)) + 1U);
}

inline std::uint32_t quantize_float(float value, FloatConfig config) {
    const std::uint32_t steps = quantized_float_steps(config);
    const float clamped = std::min(std::max(value, config.min), config.max);
    const double normalized =
        (static_cast<double>(clamped) - static_cast<double>(config.min)) /
        static_cast<double>(config.precision);
    const auto quantized = static_cast<std::uint32_t>(std::llround(normalized));
    return quantized > steps ? steps : quantized;
}

inline float dequantize_float(std::uint32_t quantized, FloatConfig config) {
    const std::uint32_t steps = quantized_float_steps(config);
    const std::uint32_t clamped = quantized > steps ? steps : quantized;
    return config.min + static_cast<float>(clamped) * config.precision;
}

inline void write_float(BitBuffer& out, float value, FloatConfig config) {
    out.push_unsigned_bits(quantize_float(value, config), float_bits(config));
}

inline bool read_float(BitBuffer& in, FloatConfig config, float& out) {
    const std::size_t bits = float_bits(config);
    if (in.remaining_bits() < bits) {
        return false;
    }
    out = dequantize_float(static_cast<std::uint32_t>(in.read_unsigned_bits(bits)), config);
    return true;
}

inline void write_delta_float(BitBuffer& out, float previous, float current, FloatConfig config) {
    const std::uint32_t previous_quantized = quantize_float(previous, config);
    const std::uint32_t current_quantized = quantize_float(current, config);
    const bool changed = previous_quantized != current_quantized;
    out.push_bool(changed);
    if (changed) {
        out.push_unsigned_bits(current_quantized, float_bits(config));
    }
}

inline bool read_delta_float(BitBuffer& in, float previous, FloatConfig config, float& out) {
    if (in.remaining_bits() < 1U) {
        return false;
    }
    if (!in.read_bool()) {
        out = dequantize_float(quantize_float(previous, config), config);
        return true;
    }
    return read_float(in, config, out);
}

inline std::uint64_t zigzag_encode(std::int64_t value) noexcept {
    return (static_cast<std::uint64_t>(value) << 1U) ^
        static_cast<std::uint64_t>(value >> 63U);
}

inline std::int64_t zigzag_decode(std::uint64_t value) noexcept {
    return static_cast<std::int64_t>((value >> 1U) ^ (~(value & 1U) + 1U));
}

inline void write_delta_int(BitBuffer& out, std::int64_t previous, std::int64_t current, std::size_t bits) {
    if (bits == 0U || bits > 64U) {
        throw std::invalid_argument("delta integer bits must be in [1, 64]");
    }
    const bool changed = previous != current;
    out.push_bool(changed);
    if (changed) {
        out.push_unsigned_bits(zigzag_encode(current - previous), bits);
    }
}

inline bool read_delta_int(BitBuffer& in, std::int64_t previous, std::size_t bits, std::int64_t& out) {
    if (bits == 0U || bits > 64U) {
        throw std::invalid_argument("delta integer bits must be in [1, 64]");
    }
    if (in.remaining_bits() < 1U) {
        return false;
    }
    if (!in.read_bool()) {
        out = previous;
        return true;
    }
    if (in.remaining_bits() < bits) {
        return false;
    }
    out = previous + zigzag_decode(in.read_unsigned_bits(bits));
    return true;
}

inline void write_vec2(BitBuffer& out, Vec2 value, FloatConfig config) {
    write_float(out, value.x, config);
    write_float(out, value.y, config);
}

inline bool read_vec2(BitBuffer& in, FloatConfig config, Vec2& out) {
    return read_float(in, config, out.x) && read_float(in, config, out.y);
}

inline void write_delta_vec2(BitBuffer& out, Vec2 previous, Vec2 current, FloatConfig config) {
    write_delta_float(out, previous.x, current.x, config);
    write_delta_float(out, previous.y, current.y, config);
}

inline bool read_delta_vec2(BitBuffer& in, Vec2 previous, FloatConfig config, Vec2& out) {
    return read_delta_float(in, previous.x, config, out.x) &&
        read_delta_float(in, previous.y, config, out.y);
}

inline void write_vec3(BitBuffer& out, Vec3 value, FloatConfig config) {
    write_float(out, value.x, config);
    write_float(out, value.y, config);
    write_float(out, value.z, config);
}

inline bool read_vec3(BitBuffer& in, FloatConfig config, Vec3& out) {
    return read_float(in, config, out.x) &&
        read_float(in, config, out.y) &&
        read_float(in, config, out.z);
}

inline void write_delta_vec3(BitBuffer& out, Vec3 previous, Vec3 current, FloatConfig config) {
    write_delta_float(out, previous.x, current.x, config);
    write_delta_float(out, previous.y, current.y, config);
    write_delta_float(out, previous.z, current.z, config);
}

inline bool read_delta_vec3(BitBuffer& in, Vec3 previous, FloatConfig config, Vec3& out) {
    return read_delta_float(in, previous.x, config, out.x) &&
        read_delta_float(in, previous.y, config, out.y) &&
        read_delta_float(in, previous.z, config, out.z);
}

inline void write_quaternion(BitBuffer& out, Quaternion value, FloatConfig config) {
    write_float(out, value.x, config);
    write_float(out, value.y, config);
    write_float(out, value.z, config);
    write_float(out, value.w, config);
}

inline bool read_quaternion(BitBuffer& in, FloatConfig config, Quaternion& out) {
    return read_float(in, config, out.x) &&
        read_float(in, config, out.y) &&
        read_float(in, config, out.z) &&
        read_float(in, config, out.w);
}

inline void write_delta_quaternion(BitBuffer& out, Quaternion previous, Quaternion current, FloatConfig config) {
    write_delta_float(out, previous.x, current.x, config);
    write_delta_float(out, previous.y, current.y, config);
    write_delta_float(out, previous.z, current.z, config);
    write_delta_float(out, previous.w, current.w, config);
}

inline bool read_delta_quaternion(BitBuffer& in, Quaternion previous, FloatConfig config, Quaternion& out) {
    return read_delta_float(in, previous.x, config, out.x) &&
        read_delta_float(in, previous.y, config, out.y) &&
        read_delta_float(in, previous.z, config, out.z) &&
        read_delta_float(in, previous.w, config, out.w);
}

}  // namespace kage::sync::delta
