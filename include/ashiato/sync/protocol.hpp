#pragma once

#include "ashiato/bit_buffer.hpp"
#include "ashiato/sync/detail/bit_reader.hpp"
#include "ashiato/sync/serialization.hpp"
#include "ashiato/sync/wire_format.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#ifndef ASHIATO_SYNC_BASELINE_FRAME_DELTA_BITS
#define ASHIATO_SYNC_BASELINE_FRAME_DELTA_BITS 5
#endif

namespace ashiato::sync::protocol {

inline constexpr std::size_t default_max_pending_packet_acks_per_client = 255U;
inline constexpr std::size_t network_entity_id_bits = 32U;
inline constexpr std::size_t client_id_bits = 8U;
inline constexpr std::size_t default_network_entity_id_tier0_bits = 11U;
inline constexpr std::size_t network_entity_id_tier1_bits = 23U;
inline constexpr std::uint32_t network_entity_id_tier1_max =
    (std::uint32_t{1} << network_entity_id_tier1_bits) - 1U;
inline constexpr std::size_t client_ack_header_bits = message_bits + ack_count_bits;
inline constexpr std::size_t client_connect_ack_bits = message_bits + client_id_bits;
inline constexpr std::size_t frame_subframe_bits = 16U;
inline constexpr std::uint32_t frame_subframe_scale = std::uint32_t{1} << frame_subframe_bits;
inline constexpr std::size_t client_ping_bits = message_bits + 32U;
inline constexpr std::size_t server_pong_bits = message_bits + 32U + 32U + frame_subframe_bits + 32U + frame_subframe_bits;
inline constexpr std::size_t baseline_frame_delta_bits = ASHIATO_SYNC_BASELINE_FRAME_DELTA_BITS;
inline constexpr std::size_t cue_frame_delta_bits = 7U;

static_assert(baseline_frame_delta_bits > 0U, "ASHIATO_SYNC_BASELINE_FRAME_DELTA_BITS must be at least 1");
static_assert(baseline_frame_delta_bits < 32U, "ASHIATO_SYNC_BASELINE_FRAME_DELTA_BITS must be less than 32");

inline constexpr std::uint32_t max_baseline_frame_delta =
    (std::uint32_t{1} << baseline_frame_delta_bits) - 1U;
inline constexpr std::uint32_t max_cue_frame_delta =
    (std::uint32_t{1} << cue_frame_delta_bits) - 1U;

struct Descriptor {
    std::size_t max_pending_packet_acks_per_client = default_max_pending_packet_acks_per_client;
    std::size_t network_entity_id_tier0_bits = default_network_entity_id_tier0_bits;
    std::size_t baseline_frame_delta_bits = protocol::baseline_frame_delta_bits;
};

inline constexpr Descriptor default_descriptor{};

inline constexpr std::size_t bytes_for_bits(std::size_t bits) noexcept {
    return (bits + 7U) / 8U;
}

inline constexpr std::size_t bits_for_range(std::size_t value_count) noexcept {
    if (value_count <= 1U) {
        return 1U;
    }
    std::size_t bits = 0;
    std::size_t values = value_count - 1U;
    while (values != 0U) {
        ++bits;
        values >>= 1U;
    }
    return bits;
}

inline constexpr std::size_t packet_id_bits_for_max_pending(std::size_t max_pending_packet_acks) noexcept {
    if (max_pending_packet_acks >= std::numeric_limits<std::uint32_t>::max()) {
        return 32U;
    }
    const std::size_t bits = bits_for_range(max_pending_packet_acks + 1U);
    return bits > 32U ? 32U : bits;
}

inline constexpr std::size_t packet_id_bits(const Descriptor& descriptor) noexcept {
    return packet_id_bits_for_max_pending(descriptor.max_pending_packet_acks_per_client);
}

inline constexpr std::uint32_t packet_id_mask(std::size_t packet_id_bits) noexcept {
    return packet_id_bits >= 32U
        ? std::numeric_limits<std::uint32_t>::max()
        : ((std::uint32_t{1} << packet_id_bits) - 1U);
}

inline constexpr std::size_t server_packet_id_bits =
    packet_id_bits_for_max_pending(default_max_pending_packet_acks_per_client);
inline constexpr std::size_t server_update_header_bits = message_bits + 32U + server_packet_id_bits + 32U + 16U;
inline constexpr std::size_t client_ack_record_bits = server_packet_id_bits;

inline constexpr std::size_t server_update_header_bits_for(const Descriptor& descriptor) noexcept {
    return message_bits + 32U + packet_id_bits(descriptor) + 32U + 16U;
}

inline constexpr std::size_t client_ack_record_bits_for(const Descriptor& descriptor) noexcept {
    return packet_id_bits(descriptor);
}

inline constexpr bool valid_network_entity_id_tier0_bits(std::size_t tier0_bits) noexcept {
    return tier0_bits > 0U && tier0_bits < network_entity_id_tier1_bits;
}

inline constexpr std::uint32_t network_entity_id_tier0_max(
    std::size_t tier0_bits = default_network_entity_id_tier0_bits) noexcept {
    return (std::uint32_t{1} << tier0_bits) - 1U;
}

inline constexpr std::size_t network_entity_id_encoded_bits(
    std::uint32_t network_id,
    std::size_t tier0_bits = default_network_entity_id_tier0_bits) noexcept {
    if (network_id <= network_entity_id_tier0_max(tier0_bits)) {
        return tier0_bits + 1U;
    }
    if (network_id <= network_entity_id_tier1_max) {
        return 25U;
    }
    return 34U;
}

inline void write_network_entity_id(
    ashiato::BitBuffer& out,
    std::uint32_t network_id,
    std::size_t tier0_bits = default_network_entity_id_tier0_bits) {
    const int tier = network_id <= network_entity_id_tier0_max(tier0_bits)
        ? 0
        : (network_id <= network_entity_id_tier1_max ? 1 : 2);
    serialization::serialize_varint3_raw(
        out,
        tier,
        network_id,
        tier0_bits,
        network_id,
        network_entity_id_tier1_bits,
        network_id,
        network_entity_id_bits);
}

inline bool read_network_entity_id(
    detail::BitReader& in,
    std::uint32_t& network_id,
    std::size_t tier0_bits = default_network_entity_id_tier0_bits) {
    int tier = 0;
    std::uint64_t value = 0;
    if (!serialization::read_varint3_raw(
            in,
            tier0_bits,
            network_entity_id_tier1_bits,
            network_entity_id_bits,
            tier,
            value)) {
        return false;
    }
    network_id = static_cast<std::uint32_t>(value);
    return true;
}

inline bool read_network_entity_id(
    ashiato::BitBuffer& in,
    std::uint32_t& network_id,
    std::size_t tier0_bits = default_network_entity_id_tier0_bits) {
    detail::BitReader reader(in);
    return read_network_entity_id(reader, network_id, tier0_bits);
}

inline void write_string(ashiato::BitBuffer& out, const std::string& value) {
    const std::size_t length = value.size() > std::numeric_limits<std::uint16_t>::max()
        ? std::numeric_limits<std::uint16_t>::max()
        : value.size();
    out.write_bits(static_cast<std::uint16_t>(length), 16U);
    out.write_bytes(value.data(), length);
}

inline bool read_string(ashiato::BitBuffer& in, std::string& value) {
    detail::BitReader reader(in);
    std::uint16_t length = 0;
    if (!reader.read_bits(16U, length)) {
        return false;
    }
    value.resize(length);
    if (!reader.read_bytes(value.data(), length)) {
        value.clear();
        return false;
    }
    return true;
}

inline void write_baseline_frame(ashiato::BitBuffer& out, std::uint32_t current_frame, std::uint32_t baseline_frame) {
    const bool can_use_delta = current_frame >= baseline_frame &&
        current_frame - baseline_frame <= max_baseline_frame_delta;
    const std::uint32_t delta = can_use_delta ? current_frame - baseline_frame : 0U;
    serialization::serialize_varint2_raw(
        out,
        can_use_delta,
        delta,
        baseline_frame_delta_bits,
        baseline_frame,
        32U);
}

inline bool read_baseline_frame(detail::BitReader& in, std::uint32_t current_frame, std::uint32_t& baseline_frame) {
    bool uses_delta = false;
    std::uint64_t value = 0;
    if (!serialization::read_varint2_raw(in, baseline_frame_delta_bits, 32U, uses_delta, value)) {
        return false;
    }
    if (!uses_delta) {
        baseline_frame = static_cast<std::uint32_t>(value);
        return true;
    }

    if (value > current_frame) {
        return false;
    }
    baseline_frame = current_frame - static_cast<std::uint32_t>(value);
    return true;
}

inline bool read_baseline_frame(ashiato::BitBuffer& in, std::uint32_t current_frame, std::uint32_t& baseline_frame) {
    detail::BitReader reader(in);
    return read_baseline_frame(reader, current_frame, baseline_frame);
}

inline void write_cue_frame(ashiato::BitBuffer& out, std::uint32_t transmitted_frame, std::uint32_t cue_frame) {
    const bool can_use_delta = transmitted_frame >= cue_frame &&
        transmitted_frame - cue_frame <= max_cue_frame_delta;
    const std::uint32_t delta = can_use_delta ? transmitted_frame - cue_frame : 0U;
    serialization::serialize_varint2_raw(
        out,
        can_use_delta,
        delta,
        cue_frame_delta_bits,
        cue_frame,
        32U);
}

inline bool read_cue_frame(detail::BitReader& in, std::uint32_t transmitted_frame, std::uint32_t& cue_frame) {
    bool uses_delta = false;
    std::uint64_t value = 0;
    if (!serialization::read_varint2_raw(in, cue_frame_delta_bits, 32U, uses_delta, value)) {
        return false;
    }
    if (!uses_delta) {
        cue_frame = static_cast<std::uint32_t>(value);
        return true;
    }
    if (value > transmitted_frame) {
        return false;
    }
    cue_frame = transmitted_frame - static_cast<std::uint32_t>(value);
    return true;
}

inline bool read_cue_frame(ashiato::BitBuffer& in, std::uint32_t transmitted_frame, std::uint32_t& cue_frame) {
    detail::BitReader reader(in);
    return read_cue_frame(reader, transmitted_frame, cue_frame);
}

}  // namespace ashiato::sync::protocol
