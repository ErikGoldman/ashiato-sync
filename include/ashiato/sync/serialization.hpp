#pragma once

#include "ashiato/bit_buffer.hpp"
#include "ashiato/sync/detail/bit_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace ashiato::sync::serialization {

struct QuantizedFloatConfig {
    float min = 0.0f;
    float max = 1.0f;
    float resolution = 0.01f;
};

struct VariableQuantizedFloatConfig {
    QuantizedFloatConfig short_range;
    QuantizedFloatConfig long_range;
};

struct QuantizedIntConfig {
    std::int64_t min = 0;
    std::int64_t max = 1;
};

struct VariableQuantizedIntConfig {
    QuantizedIntConfig short_range;
    QuantizedIntConfig long_range;
};

struct VarInt2Config {
    QuantizedIntConfig short_range;
    QuantizedIntConfig full_range;
};

struct VarInt3Config {
    QuantizedIntConfig short_range;
    QuantizedIntConfig medium_range;
    QuantizedIntConfig full_range;
};

using VarIntConfig = QuantizedIntConfig;
using VariableVarIntConfig = VarInt2Config;

inline std::size_t bits_for_max_value(std::uint64_t max_value) noexcept {
    std::size_t bits = 0;
    do {
        ++bits;
        max_value >>= 1U;
    } while (max_value != 0U);
    return bits;
}

inline void validate_quantized_float_config(QuantizedFloatConfig config) {
    if (!std::isfinite(config.min) ||
        !std::isfinite(config.max) ||
        !std::isfinite(config.resolution) ||
        config.max <= config.min ||
        config.resolution <= 0.0f) {
        throw std::invalid_argument("invalid quantized float config");
    }
}

inline std::uint64_t quantized_float_steps(QuantizedFloatConfig config) {
    validate_quantized_float_config(config);
    const double range = static_cast<double>(config.max) - static_cast<double>(config.min);
    const double steps = std::ceil(range / static_cast<double>(config.resolution));
    if (steps > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        throw std::out_of_range("quantized float config requires more than 64 bits");
    }
    return static_cast<std::uint64_t>(steps);
}

inline std::size_t quantized_float_bits(QuantizedFloatConfig config) {
    return bits_for_max_value(quantized_float_steps(config));
}

inline std::uint64_t quantize_float(float value, QuantizedFloatConfig config) {
    const std::uint64_t steps = quantized_float_steps(config);
    const float clamped = std::min(std::max(value, config.min), config.max);
    const double normalized =
        (static_cast<double>(clamped) - static_cast<double>(config.min)) /
        static_cast<double>(config.resolution);
    const double rounded = std::round(normalized);
    if (rounded <= 0.0) {
        return 0U;
    }
    if (rounded >= static_cast<double>(steps)) {
        return steps;
    }
    return static_cast<std::uint64_t>(rounded);
}

inline float dequantize_float(std::uint64_t quantized, QuantizedFloatConfig config) {
    const std::uint64_t steps = quantized_float_steps(config);
    const std::uint64_t clamped = quantized > steps ? steps : quantized;
    const float value = config.min + static_cast<float>(clamped) * config.resolution;
    return std::min(std::max(value, config.min), config.max);
}

inline void serialize_quantized_float(ashiato::BitBuffer& out, float value, QuantizedFloatConfig config) {
    out.write_unsigned_bits(quantize_float(value, config), quantized_float_bits(config));
}

inline bool read_quantized_float(ashiato::BitBuffer& in, QuantizedFloatConfig config, float& out) {
    const std::size_t bits = quantized_float_bits(config);
    if (in.remaining_bits() < bits) {
        return false;
    }
    out = dequantize_float(in.read_unsigned_bits(bits), config);
    return true;
}

inline float deserialize_quantized_float(ashiato::BitBuffer& in, QuantizedFloatConfig config) {
    const std::size_t bits = quantized_float_bits(config);
    if (in.remaining_bits() < bits) {
        throw std::out_of_range("truncated quantized float");
    }
    return dequantize_float(in.read_unsigned_bits(bits), config);
}

inline std::uint64_t ordered_int(std::int64_t value) noexcept {
    return static_cast<std::uint64_t>(value) ^ (std::uint64_t{1} << 63U);
}

inline std::int64_t signed_from_ordered(std::uint64_t value) noexcept {
    const std::uint64_t raw = value ^ (std::uint64_t{1} << 63U);
    std::int64_t out = 0;
    std::memcpy(&out, &raw, sizeof(out));
    return out;
}

inline void validate_quantized_int_config(QuantizedIntConfig config) {
    if (config.max < config.min) {
        throw std::invalid_argument("invalid quantized integer config");
    }
}

inline std::uint64_t quantized_int_max_offset(QuantizedIntConfig config) {
    validate_quantized_int_config(config);
    return ordered_int(config.max) - ordered_int(config.min);
}

inline std::size_t quantized_int_bits(QuantizedIntConfig config) {
    return bits_for_max_value(quantized_int_max_offset(config));
}

inline std::uint64_t quantize_int(std::int64_t value, QuantizedIntConfig config) {
    const std::int64_t clamped = std::min(std::max(value, config.min), config.max);
    return ordered_int(clamped) - ordered_int(config.min);
}

inline std::int64_t dequantize_int(std::uint64_t quantized, QuantizedIntConfig config) {
    const std::uint64_t max_offset = quantized_int_max_offset(config);
    const std::uint64_t clamped = quantized > max_offset ? max_offset : quantized;
    return signed_from_ordered(ordered_int(config.min) + clamped);
}

inline void serialize_quantized_int(ashiato::BitBuffer& out, std::int64_t value, QuantizedIntConfig config) {
    out.write_unsigned_bits(quantize_int(value, config), quantized_int_bits(config));
}

inline bool read_quantized_int(ashiato::BitBuffer& in, QuantizedIntConfig config, std::int64_t& out) {
    const std::size_t bits = quantized_int_bits(config);
    if (in.remaining_bits() < bits) {
        return false;
    }
    out = dequantize_int(in.read_unsigned_bits(bits), config);
    return true;
}

inline bool read_quantized_int(detail::BitReader& in, QuantizedIntConfig config, std::int64_t& out) {
    const std::size_t bits = quantized_int_bits(config);
    std::uint64_t quantized = 0;
    if (!in.read_bits(bits, quantized)) {
        return false;
    }
    out = dequantize_int(quantized, config);
    return true;
}

inline std::int64_t deserialize_quantized_int(ashiato::BitBuffer& in, QuantizedIntConfig config) {
    const std::size_t bits = quantized_int_bits(config);
    if (in.remaining_bits() < bits) {
        throw std::out_of_range("truncated quantized integer");
    }
    return dequantize_int(in.read_unsigned_bits(bits), config);
}

inline void validate_variable_float_config(const VariableQuantizedFloatConfig& config) {
    validate_quantized_float_config(config.short_range);
    validate_quantized_float_config(config.long_range);
    if (config.short_range.resolution != config.long_range.resolution ||
        config.short_range.min < config.long_range.min ||
        config.short_range.max > config.long_range.max) {
        throw std::invalid_argument("invalid variable quantized float config");
    }
}

inline bool in_range(float value, QuantizedFloatConfig config) noexcept {
    return value >= config.min && value <= config.max;
}

inline bool variable_quantized_float_uses_long(float value, const VariableQuantizedFloatConfig& config) {
    validate_variable_float_config(config);
    return !in_range(value, config.short_range);
}

inline std::size_t variable_quantized_float_bits(float value, const VariableQuantizedFloatConfig& config) {
    return 1U + (variable_quantized_float_uses_long(value, config)
        ? quantized_float_bits(config.long_range)
        : quantized_float_bits(config.short_range));
}

inline void serialize_variable_quantized_float(
    ashiato::BitBuffer& out,
    float value,
    const VariableQuantizedFloatConfig& config) {
    const bool uses_long = variable_quantized_float_uses_long(value, config);
    out.write_bool(uses_long);
    serialize_quantized_float(out, value, uses_long ? config.long_range : config.short_range);
}

inline bool read_variable_quantized_float(
    ashiato::BitBuffer& in,
    const VariableQuantizedFloatConfig& config,
    float& out) {
    validate_variable_float_config(config);
    if (in.remaining_bits() < 1U) {
        return false;
    }
    const bool uses_long = in.read_bool();
    return read_quantized_float(in, uses_long ? config.long_range : config.short_range, out);
}

inline float deserialize_variable_quantized_float(
    ashiato::BitBuffer& in,
    const VariableQuantizedFloatConfig& config) {
    validate_variable_float_config(config);
    if (in.remaining_bits() < 1U) {
        throw std::out_of_range("truncated variable quantized float selector");
    }
    const bool uses_long = in.read_bool();
    return deserialize_quantized_float(in, uses_long ? config.long_range : config.short_range);
}

inline void validate_variable_int_config(VariableQuantizedIntConfig config) {
    validate_quantized_int_config(config.short_range);
    validate_quantized_int_config(config.long_range);
    if (config.short_range.min < config.long_range.min ||
        config.short_range.max > config.long_range.max) {
        throw std::invalid_argument("invalid variable quantized integer config");
    }
}

inline bool in_range(std::int64_t value, QuantizedIntConfig config) noexcept {
    return value >= config.min && value <= config.max;
}

inline bool variable_quantized_int_uses_long(std::int64_t value, VariableQuantizedIntConfig config) {
    validate_variable_int_config(config);
    return !in_range(value, config.short_range);
}

inline std::size_t variable_quantized_int_bits(std::int64_t value, VariableQuantizedIntConfig config) {
    return 1U + (variable_quantized_int_uses_long(value, config)
        ? quantized_int_bits(config.long_range)
        : quantized_int_bits(config.short_range));
}

inline void serialize_variable_quantized_int(
    ashiato::BitBuffer& out,
    std::int64_t value,
    VariableQuantizedIntConfig config) {
    const bool uses_long = variable_quantized_int_uses_long(value, config);
    out.write_bool(uses_long);
    serialize_quantized_int(out, value, uses_long ? config.long_range : config.short_range);
}

inline bool read_variable_quantized_int(
    ashiato::BitBuffer& in,
    VariableQuantizedIntConfig config,
    std::int64_t& out) {
    validate_variable_int_config(config);
    if (in.remaining_bits() < 1U) {
        return false;
    }
    const bool uses_long = in.read_bool();
    return read_quantized_int(in, uses_long ? config.long_range : config.short_range, out);
}

inline std::int64_t deserialize_variable_quantized_int(
    ashiato::BitBuffer& in,
    VariableQuantizedIntConfig config) {
    validate_variable_int_config(config);
    if (in.remaining_bits() < 1U) {
        throw std::out_of_range("truncated variable quantized integer selector");
    }
    const bool uses_long = in.read_bool();
    return deserialize_quantized_int(in, uses_long ? config.long_range : config.short_range);
}

inline void validate_varint2_config(VarInt2Config config) {
    validate_quantized_int_config(config.short_range);
    validate_quantized_int_config(config.full_range);
    if (config.short_range.min < config.full_range.min ||
        config.short_range.max > config.full_range.max) {
        throw std::invalid_argument("invalid varint2 config");
    }
}

inline void validate_varint3_config(VarInt3Config config) {
    validate_quantized_int_config(config.short_range);
    validate_quantized_int_config(config.medium_range);
    validate_quantized_int_config(config.full_range);
    if (config.short_range.min < config.medium_range.min ||
        config.short_range.max > config.medium_range.max ||
        config.medium_range.min < config.full_range.min ||
        config.medium_range.max > config.full_range.max) {
        throw std::invalid_argument("invalid varint3 config");
    }
}

inline bool varint2_uses_full(std::int64_t value, VarInt2Config config) {
    validate_varint2_config(config);
    return !in_range(value, config.short_range);
}

inline std::size_t varint2_bits(std::int64_t value, VarInt2Config config) {
    return 1U + (varint2_uses_full(value, config)
        ? quantized_int_bits(config.full_range)
        : quantized_int_bits(config.short_range));
}

inline void serialize_varint2(ashiato::BitBuffer& out, std::int64_t value, VarInt2Config config) {
    const bool uses_full = varint2_uses_full(value, config);
    out.write_bool(uses_full);
    serialize_quantized_int(out, value, uses_full ? config.full_range : config.short_range);
}

inline bool read_varint2(ashiato::BitBuffer& in, VarInt2Config config, std::int64_t& out) {
    validate_varint2_config(config);
    if (in.remaining_bits() < 1U) {
        return false;
    }
    const bool uses_full = in.read_bool();
    return read_quantized_int(in, uses_full ? config.full_range : config.short_range, out);
}

inline bool read_varint2(detail::BitReader& in, VarInt2Config config, std::int64_t& out) {
    validate_varint2_config(config);
    bool uses_full = false;
    if (!in.read_bits(1U, uses_full)) {
        return false;
    }
    return read_quantized_int(in, uses_full ? config.full_range : config.short_range, out);
}

inline std::int64_t deserialize_varint2(ashiato::BitBuffer& in, VarInt2Config config) {
    validate_varint2_config(config);
    if (in.remaining_bits() < 1U) {
        throw std::out_of_range("truncated varint2 selector");
    }
    const bool uses_full = in.read_bool();
    return deserialize_quantized_int(in, uses_full ? config.full_range : config.short_range);
}

inline std::size_t varint3_bits(std::int64_t value, VarInt3Config config) {
    validate_varint3_config(config);
    if (in_range(value, config.short_range)) {
        return 1U + quantized_int_bits(config.short_range);
    }
    if (in_range(value, config.medium_range)) {
        return 2U + quantized_int_bits(config.medium_range);
    }
    return 2U + quantized_int_bits(config.full_range);
}

inline void serialize_varint3(ashiato::BitBuffer& out, std::int64_t value, VarInt3Config config) {
    validate_varint3_config(config);
    if (in_range(value, config.short_range)) {
        out.write_bool(false);
        serialize_quantized_int(out, value, config.short_range);
        return;
    }
    out.write_bool(true);
    const bool uses_full = !in_range(value, config.medium_range);
    out.write_bool(uses_full);
    serialize_quantized_int(out, value, uses_full ? config.full_range : config.medium_range);
}

inline bool read_varint3(ashiato::BitBuffer& in, VarInt3Config config, std::int64_t& out) {
    validate_varint3_config(config);
    if (in.remaining_bits() < 1U) {
        return false;
    }
    const bool uses_wide = in.read_bool();
    if (!uses_wide) {
        return read_quantized_int(in, config.short_range, out);
    }
    if (in.remaining_bits() < 1U) {
        return false;
    }
    const bool uses_full = in.read_bool();
    return read_quantized_int(in, uses_full ? config.full_range : config.medium_range, out);
}

inline bool read_varint3(detail::BitReader& in, VarInt3Config config, std::int64_t& out) {
    validate_varint3_config(config);
    bool uses_wide = false;
    if (!in.read_bits(1U, uses_wide)) {
        return false;
    }
    if (!uses_wide) {
        return read_quantized_int(in, config.short_range, out);
    }
    bool uses_full = false;
    if (!in.read_bits(1U, uses_full)) {
        return false;
    }
    return read_quantized_int(in, uses_full ? config.full_range : config.medium_range, out);
}

inline std::int64_t deserialize_varint3(ashiato::BitBuffer& in, VarInt3Config config) {
    validate_varint3_config(config);
    if (in.remaining_bits() < 1U) {
        throw std::out_of_range("truncated varint3 selector");
    }
    const bool uses_wide = in.read_bool();
    if (!uses_wide) {
        return deserialize_quantized_int(in, config.short_range);
    }
    if (in.remaining_bits() < 1U) {
        throw std::out_of_range("truncated varint3 selector");
    }
    const bool uses_full = in.read_bool();
    return deserialize_quantized_int(in, uses_full ? config.full_range : config.medium_range);
}

inline std::size_t varint2_or_zero_bits(std::int64_t value, VarInt2Config config) {
    return value == 0 ? 1U : 1U + varint2_bits(value, config);
}

inline void serialize_varint2_or_zero(ashiato::BitBuffer& out, std::int64_t value, VarInt2Config config) {
    const bool has_value = value != 0;
    out.write_bool(has_value);
    if (has_value) {
        serialize_varint2(out, value, config);
    }
}

inline bool read_varint2_or_zero(ashiato::BitBuffer& in, VarInt2Config config, std::int64_t& out) {
    if (in.remaining_bits() < 1U) {
        return false;
    }
    const bool has_value = in.read_bool();
    if (!has_value) {
        out = 0;
        return true;
    }
    return read_varint2(in, config, out);
}

inline std::int64_t deserialize_varint2_or_zero(ashiato::BitBuffer& in, VarInt2Config config) {
    if (in.remaining_bits() < 1U) {
        throw std::out_of_range("truncated varint2_or_zero selector");
    }
    const bool has_value = in.read_bool();
    return has_value ? deserialize_varint2(in, config) : 0;
}

inline std::size_t varint3_or_zero_bits(std::int64_t value, VarInt3Config config) {
    return value == 0 ? 1U : 1U + varint3_bits(value, config);
}

inline void serialize_varint3_or_zero(ashiato::BitBuffer& out, std::int64_t value, VarInt3Config config) {
    const bool has_value = value != 0;
    out.write_bool(has_value);
    if (has_value) {
        serialize_varint3(out, value, config);
    }
}

inline bool read_varint3_or_zero(ashiato::BitBuffer& in, VarInt3Config config, std::int64_t& out) {
    if (in.remaining_bits() < 1U) {
        return false;
    }
    const bool has_value = in.read_bool();
    if (!has_value) {
        out = 0;
        return true;
    }
    return read_varint3(in, config, out);
}

inline std::int64_t deserialize_varint3_or_zero(ashiato::BitBuffer& in, VarInt3Config config) {
    if (in.remaining_bits() < 1U) {
        throw std::out_of_range("truncated varint3_or_zero selector");
    }
    const bool has_value = in.read_bool();
    return has_value ? deserialize_varint3(in, config) : 0;
}

inline std::size_t varint2_raw_bits(bool use_short, std::size_t short_bits, std::size_t full_bits) noexcept {
    return 1U + (use_short ? short_bits : full_bits);
}

inline void serialize_varint2_raw(
    ashiato::BitBuffer& out,
    bool use_short,
    std::uint64_t short_value,
    std::size_t short_bits,
    std::uint64_t full_value,
    std::size_t full_bits) {
    out.write_bool(!use_short);
    out.write_unsigned_bits(use_short ? short_value : full_value, use_short ? short_bits : full_bits);
}

inline bool read_varint2_raw(
    detail::BitReader& in,
    std::size_t short_bits,
    std::size_t full_bits,
    bool& used_short,
    std::uint64_t& out) {
    bool uses_full = false;
    if (!in.read_bits(1U, uses_full)) {
        return false;
    }
    used_short = !uses_full;
    return in.read_bits(uses_full ? full_bits : short_bits, out);
}

inline bool read_varint2_raw(
    ashiato::BitBuffer& in,
    std::size_t short_bits,
    std::size_t full_bits,
    bool& used_short,
    std::uint64_t& out) {
    detail::BitReader reader(in);
    return read_varint2_raw(reader, short_bits, full_bits, used_short, out);
}

inline void serialize_varint3_raw(
    ashiato::BitBuffer& out,
    int tier,
    std::uint64_t short_value,
    std::size_t short_bits,
    std::uint64_t medium_value,
    std::size_t medium_bits,
    std::uint64_t full_value,
    std::size_t full_bits) {
    if (tier < 0 || tier > 2) {
        throw std::invalid_argument("invalid varint3 tier");
    }
    if (tier == 0) {
        out.write_bool(false);
        out.write_unsigned_bits(short_value, short_bits);
        return;
    }
    out.write_bool(true);
    out.write_bool(tier != 1);
    out.write_unsigned_bits(tier == 1 ? medium_value : full_value, tier == 1 ? medium_bits : full_bits);
}

inline bool read_varint3_raw(
    detail::BitReader& in,
    std::size_t short_bits,
    std::size_t medium_bits,
    std::size_t full_bits,
    int& tier,
    std::uint64_t& out) {
    bool uses_wide = false;
    if (!in.read_bits(1U, uses_wide)) {
        return false;
    }
    if (!uses_wide) {
        tier = 0;
        return in.read_bits(short_bits, out);
    }
    bool uses_full = false;
    if (!in.read_bits(1U, uses_full)) {
        return false;
    }
    tier = uses_full ? 2 : 1;
    return in.read_bits(uses_full ? full_bits : medium_bits, out);
}

inline bool read_varint3_raw(
    ashiato::BitBuffer& in,
    std::size_t short_bits,
    std::size_t medium_bits,
    std::size_t full_bits,
    int& tier,
    std::uint64_t& out) {
    detail::BitReader reader(in);
    return read_varint3_raw(reader, short_bits, medium_bits, full_bits, tier, out);
}

inline std::size_t varint_bits(VarIntConfig config) {
    return quantized_int_bits(config);
}

inline void serialize_varint(ashiato::BitBuffer& out, std::int64_t value, VarIntConfig config) {
    serialize_quantized_int(out, value, config);
}

inline bool read_varint(ashiato::BitBuffer& in, VarIntConfig config, std::int64_t& out) {
    return read_quantized_int(in, config, out);
}

inline std::int64_t deserialize_varint(ashiato::BitBuffer& in, VarIntConfig config) {
    return deserialize_quantized_int(in, config);
}

inline std::size_t variable_varint_bits(std::int64_t value, VariableVarIntConfig config) {
    return varint2_bits(value, config);
}

inline void serialize_variable_varint(ashiato::BitBuffer& out, std::int64_t value, VariableVarIntConfig config) {
    serialize_varint2(out, value, config);
}

inline bool read_variable_varint(ashiato::BitBuffer& in, VariableVarIntConfig config, std::int64_t& out) {
    return read_varint2(in, config, out);
}

inline std::int64_t deserialize_variable_varint(ashiato::BitBuffer& in, VariableVarIntConfig config) {
    return deserialize_varint2(in, config);
}

}  // namespace ashiato::sync::serialization
