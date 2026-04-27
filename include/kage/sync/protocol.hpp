#pragma once

#include <cstddef>
#include <cstdint>

namespace kage::sync::protocol {

inline constexpr std::uint8_t server_update_message = 1;
inline constexpr std::uint8_t client_ack_message = 2;
inline constexpr std::uint8_t client_hello_message = 3;

inline constexpr std::size_t server_update_header_bits = 8U + 32U + 16U;
inline constexpr std::size_t client_ack_header_bits = 8U + 16U;
inline constexpr std::size_t client_ack_record_bits = 1U + 32U + 64U;

inline std::size_t bytes_for_bits(std::size_t bits) noexcept {
    return (bits + 7U) / 8U;
}

}  // namespace kage::sync::protocol
