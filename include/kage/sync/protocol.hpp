#pragma once

#include "kage/sync/bit_buffer.hpp"

#include <cstddef>
#include <cstdint>

#ifndef KAGE_SYNC_BASELINE_FRAME_DELTA_BITS
#define KAGE_SYNC_BASELINE_FRAME_DELTA_BITS 5
#endif

namespace kage::sync::protocol {

inline constexpr std::uint8_t server_update_message = 1;
inline constexpr std::uint8_t client_ack_message = 2;
inline constexpr std::uint8_t client_hello_message = 3;

inline constexpr std::size_t server_packet_id_bits = 32U;
inline constexpr std::size_t network_entity_id_bits = 32U;
inline constexpr std::size_t server_update_header_bits = 8U + 32U + server_packet_id_bits + 16U;
inline constexpr std::size_t client_ack_header_bits = 8U + 16U;
inline constexpr std::size_t client_ack_record_bits = server_packet_id_bits;
inline constexpr std::size_t baseline_frame_delta_bits = KAGE_SYNC_BASELINE_FRAME_DELTA_BITS;

static_assert(baseline_frame_delta_bits > 0U, "KAGE_SYNC_BASELINE_FRAME_DELTA_BITS must be at least 1");
static_assert(baseline_frame_delta_bits < 32U, "KAGE_SYNC_BASELINE_FRAME_DELTA_BITS must be less than 32");

inline constexpr std::uint32_t max_baseline_frame_delta =
    (std::uint32_t{1} << baseline_frame_delta_bits) - 1U;

inline std::size_t bytes_for_bits(std::size_t bits) noexcept {
    return (bits + 7U) / 8U;
}

inline std::size_t bits_for_range(std::size_t value_count) noexcept {
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
