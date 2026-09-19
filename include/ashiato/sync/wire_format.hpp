#pragma once

#include <cstddef>
#include <cstdint>

namespace ashiato::sync::protocol {

inline constexpr std::size_t message_bits = 3U;
inline constexpr std::uint8_t server_update_message = 0;
inline constexpr std::uint8_t client_ack_message = 1;
inline constexpr std::uint8_t client_connect_request_message = 2;
inline constexpr std::uint8_t server_connect_response_message = 3;
inline constexpr std::uint8_t client_connect_ack_message = 4;
inline constexpr std::uint8_t client_ping_message = 5;
inline constexpr std::uint8_t server_pong_message = 6;
inline constexpr std::uint8_t client_input_message = 7;

inline constexpr std::size_t ack_count_bits = 5U;
inline constexpr std::uint16_t max_ack_count = (std::uint16_t{1} << ack_count_bits) - 1U;
inline constexpr std::size_t input_count_bits = 5U;
inline constexpr std::uint16_t max_input_count = (std::uint16_t{1} << input_count_bits) - 1U;
inline constexpr std::size_t max_cue_payload_bits = std::size_t{64U} * 8U;
inline constexpr std::size_t max_cue_value_bytes = 64U;

}  // namespace ashiato::sync::protocol
