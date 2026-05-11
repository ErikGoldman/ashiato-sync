#pragma once

#include "ecs/bit_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace kage::sync::detail {

class BitReader {
public:
    explicit BitReader(ecs::BitBuffer& buffer) noexcept
        : buffer_(buffer) {}

    bool read_bits(std::size_t bits, bool& out) {
        if (bits != 1U || buffer_.remaining_bits() < 1U) {
            return false;
        }
        out = buffer_.read_bool();
        return true;
    }

    template <typename T>
    bool read_bits(std::size_t bits, T& out) {
        static_assert(std::is_integral<T>::value, "read_bits requires an integral output type");
        static_assert(std::is_unsigned<T>::value, "read_bits reads unsigned bit fields");
        static_assert(!std::is_same<T, bool>::value, "bool outputs use the bool overload");

        if (bits > std::numeric_limits<T>::digits || buffer_.remaining_bits() < bits) {
            return false;
        }
        out = bits == 0U ? T{0} : static_cast<T>(buffer_.read_unsigned_bits(bits));
        return true;
    }

    template <typename T>
    bool read_signed_bits(std::size_t bits, T& out) {
        static_assert(std::is_integral<T>::value, "read_signed_bits requires an integral output type");
        static_assert(std::is_signed<T>::value, "read_signed_bits requires a signed output type");

        using Unsigned = typename std::make_unsigned<T>::type;
        constexpr std::size_t width = std::numeric_limits<Unsigned>::digits;
        if (bits > width || buffer_.remaining_bits() < bits) {
            return false;
        }
        if (bits == 0U) {
            out = T{0};
            return true;
        }

        Unsigned value = static_cast<Unsigned>(buffer_.read_unsigned_bits(bits));
        const Unsigned sign_bit = Unsigned{1} << (bits - 1U);
        if ((value & sign_bit) != 0U && bits < width) {
            value |= static_cast<Unsigned>(std::numeric_limits<Unsigned>::max() << bits);
        }
        std::memcpy(&out, &value, sizeof(out));
        return true;
    }

    bool read_bytes(char* out, std::size_t bytes) {
        if (out == nullptr && bytes != 0U) {
            return false;
        }
        if (bytes > (std::numeric_limits<std::size_t>::max() / 8U) ||
            buffer_.remaining_bits() < bytes * 8U) {
            return false;
        }
        buffer_.read_bytes(out, bytes);
        return true;
    }

    bool read_buffer_bits(ecs::BitBuffer& out, std::size_t bits) {
        if (buffer_.remaining_bits() < bits) {
            return false;
        }
        buffer_.read_buffer_bits(out, bits);
        return true;
    }

    ecs::BitBuffer& raw() noexcept {
        return buffer_;
    }

private:
    ecs::BitBuffer& buffer_;
};

}  // namespace kage::sync::detail
