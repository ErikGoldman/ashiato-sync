#pragma once

#include "kage/sync/bit_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#ifndef KAGE_SYNC_BASELINE_FRAME_DELTA_BITS
#define KAGE_SYNC_BASELINE_FRAME_DELTA_BITS 5
#endif

namespace kage::sync::protocol {

inline constexpr std::uint8_t server_update_message = 1;
inline constexpr std::uint8_t client_ack_message = 2;
inline constexpr std::uint8_t client_connect_request_message = 3;
inline constexpr std::uint8_t server_connect_response_message = 4;
inline constexpr std::uint8_t client_connect_ack_message = 5;
inline constexpr std::uint8_t client_ping_message = 6;
inline constexpr std::uint8_t server_pong_message = 7;

inline constexpr std::size_t default_max_pending_packet_acks_per_client = 255U;
inline constexpr std::size_t network_entity_id_bits = 32U;
inline constexpr std::uint32_t network_entity_id_tier0_max = (std::uint32_t{1} << 15U) - 1U;
inline constexpr std::uint32_t network_entity_id_tier1_max = (std::uint32_t{1} << 23U) - 1U;
inline constexpr std::size_t client_ack_header_bits = 8U + 16U;
inline constexpr std::size_t client_connect_ack_bits = 8U + 64U;
inline constexpr std::size_t client_ping_bits = 8U + 32U + 32U;
inline constexpr std::size_t server_pong_bits = 8U + 32U + 32U;
inline constexpr std::size_t baseline_frame_delta_bits = KAGE_SYNC_BASELINE_FRAME_DELTA_BITS;

static_assert(baseline_frame_delta_bits > 0U, "KAGE_SYNC_BASELINE_FRAME_DELTA_BITS must be at least 1");
static_assert(baseline_frame_delta_bits < 32U, "KAGE_SYNC_BASELINE_FRAME_DELTA_BITS must be less than 32");

inline constexpr std::uint32_t max_baseline_frame_delta =
    (std::uint32_t{1} << baseline_frame_delta_bits) - 1U;

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

inline constexpr std::uint32_t packet_id_mask(std::size_t packet_id_bits) noexcept {
    return packet_id_bits >= 32U
        ? std::numeric_limits<std::uint32_t>::max()
        : ((std::uint32_t{1} << packet_id_bits) - 1U);
}

inline constexpr std::size_t server_packet_id_bits =
    packet_id_bits_for_max_pending(default_max_pending_packet_acks_per_client);
inline constexpr std::size_t server_update_header_bits = 8U + 32U + server_packet_id_bits + 16U;
inline constexpr std::size_t client_ack_record_bits = server_packet_id_bits;

inline constexpr std::size_t network_entity_id_encoded_bits(std::uint32_t network_id) noexcept {
    if (network_id <= network_entity_id_tier0_max) {
        return 16U;
    }
    if (network_id <= network_entity_id_tier1_max) {
        return 25U;
    }
    return 34U;
}

inline void write_network_entity_id(BitBuffer& out, std::uint32_t network_id) {
    out.push_bits(network_id & network_entity_id_tier0_max, 15U);
    const bool needs_tier1 = network_id > network_entity_id_tier0_max;
    out.push_bool(needs_tier1);
    if (!needs_tier1) {
        return;
    }

    out.push_bits((network_id >> 15U) & 0xffU, 8U);
    const bool needs_tier2 = network_id > network_entity_id_tier1_max;
    out.push_bool(needs_tier2);
    if (needs_tier2) {
        out.push_bits(network_id >> 23U, 9U);
    }
}

inline bool read_network_entity_id(BitBuffer& in, std::uint32_t& network_id) {
    network_id = static_cast<std::uint32_t>(in.read_bits(15U));
    if (!in.read_bool()) {
        return true;
    }

    network_id |= static_cast<std::uint32_t>(in.read_bits(8U)) << 15U;
    if (in.read_bool()) {
        network_id |= static_cast<std::uint32_t>(in.read_bits(9U)) << 23U;
    }
    return true;
}

inline void write_string(BitBuffer& out, const std::string& value) {
    const std::size_t length = value.size() > std::numeric_limits<std::uint16_t>::max()
        ? std::numeric_limits<std::uint16_t>::max()
        : value.size();
    out.push_bits(static_cast<std::uint16_t>(length), 16U);
    out.push_bytes(value.data(), length);
}

inline bool read_string(BitBuffer& in, std::string& value) {
    if (in.remaining_bits() < 16U) {
        return false;
    }
    const auto length = static_cast<std::uint16_t>(in.read_bits(16U));
    if (in.remaining_bits() < static_cast<std::size_t>(length) * 8U) {
        return false;
    }
    value.resize(length);
    if (length != 0U) {
        in.read_bytes(value.data(), length);
    }
    return true;
}

inline void write_baseline_frame(BitBuffer& out, std::uint32_t current_frame, std::uint32_t baseline_frame) {
    const bool can_use_delta = current_frame >= baseline_frame &&
        current_frame - baseline_frame <= max_baseline_frame_delta;
    out.push_bool(can_use_delta);
    if (can_use_delta) {
        out.push_bits(current_frame - baseline_frame, baseline_frame_delta_bits);
    } else {
        out.push_bits(baseline_frame, 32U);
    }
}

inline bool read_baseline_frame(BitBuffer& in, std::uint32_t current_frame, std::uint32_t& baseline_frame) {
    const bool uses_delta = in.read_bool();
    if (!uses_delta) {
        baseline_frame = static_cast<std::uint32_t>(in.read_bits(32U));
        return true;
    }

    const auto delta = static_cast<std::uint32_t>(in.read_bits(baseline_frame_delta_bits));
    if (delta > current_frame) {
        return false;
    }
    baseline_frame = current_frame - delta;
    return true;
}

}  // namespace kage::sync::protocol
