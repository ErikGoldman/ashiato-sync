#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace kage::sync {

class BitBuffer {
public:
    void clear() noexcept {
        bytes_.clear();
        bit_size_ = 0;
        read_bit_ = 0;
    }

    bool empty() const noexcept {
        return bit_size_ == 0;
    }

    std::size_t bit_size() const noexcept {
        return bit_size_;
    }

    std::size_t byte_size() const noexcept {
        return (bit_size_ + 7U) / 8U;
    }

    std::size_t size() const noexcept {
        return byte_size();
    }

    const std::vector<std::uint8_t>& bytes() const noexcept {
        return bytes_;
    }

    const std::uint8_t* data() const noexcept {
        return bytes_.data();
    }

    void reserve_bytes(std::size_t capacity) {
        bytes_.reserve(capacity);
    }

    std::size_t read_offset_bits() const noexcept {
        return read_bit_;
    }

    std::size_t remaining_bits() const noexcept {
        return bit_size_ - read_bit_;
    }

    void reset_read() noexcept {
        read_bit_ = 0;
    }

    void push_bool(bool value) {
        if ((bit_size_ % 8U) == 0) {
            bytes_.push_back(0);
        }
        if (value) {
            bytes_[bit_size_ / 8U] |= static_cast<std::uint8_t>(1U << (bit_size_ % 8U));
        }
        ++bit_size_;
    }

    void push_bits(std::int64_t value, std::size_t num_bits) {
        push_unsigned_bits(static_cast<std::uint64_t>(value), num_bits);
    }

    void push_unsigned_bits(std::uint64_t value, std::size_t num_bits) {
        if (num_bits > 64U) {
            throw std::invalid_argument("bit buffer cannot push more than 64 bits at once");
        }
        if (num_bits == 0U) {
            return;
        }

        if ((bit_size_ % 8U) == 0 && (num_bits % 8U) == 0) {
            for (std::size_t byte = 0; byte < num_bits / 8U; ++byte) {
                bytes_.push_back(static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xFFU));
            }
            bit_size_ += num_bits;
            return;
        }

        const std::size_t bit_shift = bit_size_ % 8U;
        const std::size_t byte_offset = bit_size_ / 8U;
        const std::size_t touched_bytes = (bit_shift + num_bits + 7U) / 8U;
        const std::size_t new_bit_size = bit_size_ + num_bits;
        const std::size_t new_byte_size = (new_bit_size + 7U) / 8U;
        if (bytes_.size() < new_byte_size) {
            bytes_.resize(new_byte_size, 0U);
        }

        for (std::size_t byte = 0; byte < touched_bytes; ++byte) {
            const int source_start = static_cast<int>(byte * 8U) - static_cast<int>(bit_shift);
            std::uint8_t bits = 0;
            if (source_start >= 0) {
                if (source_start < 64) {
                    bits = static_cast<std::uint8_t>((value >> static_cast<unsigned>(source_start)) & 0xFFU);
                }
            } else {
                bits = static_cast<std::uint8_t>((value << static_cast<unsigned>(-source_start)) & 0xFFU);
            }

            std::uint8_t mask = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const int source_bit = source_start + bit;
                if (source_bit >= 0 && static_cast<std::size_t>(source_bit) < num_bits) {
                    mask |= static_cast<std::uint8_t>(1U << bit);
                }
            }
            std::uint8_t& target = bytes_[byte_offset + byte];
            target = static_cast<std::uint8_t>((target & ~mask) | (bits & mask));
        }
        bit_size_ = new_bit_size;
    }

    void overwrite_unsigned_bits(std::size_t bit_offset, std::uint64_t value, std::size_t num_bits) {
        if (num_bits > 64U) {
            throw std::invalid_argument("bit buffer cannot overwrite more than 64 bits at once");
        }
        if (bit_offset > bit_size_ || num_bits > bit_size_ - bit_offset) {
            throw std::out_of_range("bit buffer overwrite past end");
        }
        if (num_bits == 0U) {
            return;
        }

        const std::size_t bit_shift = bit_offset % 8U;
        const std::size_t byte_offset = bit_offset / 8U;
        if (bit_shift == 0U && (num_bits % 8U) == 0U) {
            for (std::size_t byte = 0; byte < num_bits / 8U; ++byte) {
                bytes_[byte_offset + byte] = static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xFFU);
            }
            return;
        }

        const std::size_t touched_bytes = (bit_shift + num_bits + 7U) / 8U;
        if (touched_bytes <= sizeof(std::uint64_t)) {
            std::uint64_t window = 0;
            for (std::size_t byte = 0; byte < touched_bytes; ++byte) {
                window |= std::uint64_t{bytes_[byte_offset + byte]} << (byte * 8U);
            }
            const std::uint64_t value_mask = num_bits == 64U
                ? std::numeric_limits<std::uint64_t>::max()
                : ((std::uint64_t{1} << num_bits) - 1U);
            const std::uint64_t mask = value_mask << bit_shift;
            window = (window & ~mask) | ((value & value_mask) << bit_shift);
            for (std::size_t byte = 0; byte < touched_bytes; ++byte) {
                bytes_[byte_offset + byte] = static_cast<std::uint8_t>((window >> (byte * 8U)) & 0xFFU);
            }
            return;
        }

        for (std::size_t bit = 0; bit < num_bits; ++bit) {
            const std::size_t target = bit_offset + bit;
            const auto mask = static_cast<std::uint8_t>(1U << (target % 8U));
            if (((value >> bit) & 1U) != 0) {
                bytes_[target / 8U] |= mask;
            } else {
                bytes_[target / 8U] &= static_cast<std::uint8_t>(~mask);
            }
        }
    }

    void push_bytes(const char* data, std::size_t num_bytes) {
        if (data == nullptr && num_bytes != 0) {
            throw std::invalid_argument("bit buffer cannot push bytes from null data");
        }
        if (num_bytes == 0) {
            return;
        }

        if ((bit_size_ % 8U) == 0) {
            const auto* begin = reinterpret_cast<const std::uint8_t*>(data);
            bytes_.insert(bytes_.end(), begin, begin + num_bytes);
            bit_size_ += num_bytes * 8U;
            return;
        }

        for (std::size_t index = 0; index < num_bytes; ++index) {
            push_bits(static_cast<unsigned char>(data[index]), 8U);
        }
    }

    void push_buffer_bits(const BitBuffer& source) {
        if (source.bit_size_ == 0) {
            return;
        }

        if ((bit_size_ % 8U) == 0) {
            bytes_.insert(bytes_.end(), source.bytes_.begin(), source.bytes_.end());
            bit_size_ += source.bit_size_;
            return;
        }

        for (std::size_t bit = 0; bit < source.bit_size_; ++bit) {
            const bool value =
                (source.bytes_[bit / 8U] & static_cast<std::uint8_t>(1U << (bit % 8U))) != 0;
            push_bool(value);
        }
    }

    void read_buffer_bits(BitBuffer& out, std::size_t num_bits) {
        ensure_can_read(num_bits);
        if (num_bits == 0) {
            return;
        }

        if ((read_bit_ % 8U) == 0 && (out.bit_size_ % 8U) == 0 && (num_bits % 8U) == 0) {
            const std::size_t num_bytes = num_bits / 8U;
            out.push_bytes(reinterpret_cast<const char*>(bytes_.data() + (read_bit_ / 8U)), num_bytes);
            read_bit_ += num_bits;
            return;
        }

        for (std::size_t bit = 0; bit < num_bits; ++bit) {
            out.push_bool(read_bool());
        }
    }

    void skip_bits(std::size_t num_bits) {
        ensure_can_read(num_bits);
        read_bit_ += num_bits;
    }

    bool read_bool() {
        ensure_can_read(1U);
        const bool value = (bytes_[read_bit_ / 8U] & static_cast<std::uint8_t>(1U << (read_bit_ % 8U))) != 0;
        ++read_bit_;
        return value;
    }

    std::int64_t read_bits(std::size_t num_bits) {
        return static_cast<std::int64_t>(read_unsigned_bits(num_bits));
    }

    std::uint64_t read_unsigned_bits(std::size_t num_bits) {
        if (num_bits > 64U) {
            throw std::invalid_argument("bit buffer cannot read more than 64 bits at once");
        }
        if (num_bits == 0U) {
            return 0;
        }
        ensure_can_read(num_bits);

        if ((read_bit_ % 8U) == 0 && (num_bits % 8U) == 0) {
            std::uint64_t value = 0;
            for (std::size_t byte = 0; byte < num_bits / 8U; ++byte) {
                value |= std::uint64_t{bytes_[(read_bit_ / 8U) + byte]} << (byte * 8U);
            }
            read_bit_ += num_bits;
            return value;
        }

        const std::size_t bit_shift = read_bit_ % 8U;
        const std::size_t byte_offset = read_bit_ / 8U;
        std::uint64_t window = 0;
        const std::size_t available_bytes = bytes_.size() - byte_offset;
        const std::size_t low_bytes = available_bytes < sizeof(std::uint64_t)
            ? available_bytes
            : sizeof(std::uint64_t);
        for (std::size_t byte = 0; byte < low_bytes; ++byte) {
            window |= std::uint64_t{bytes_[byte_offset + byte]} << (byte * 8U);
        }

        std::uint64_t value = window >> bit_shift;
        const std::size_t bits_from_window = 64U - bit_shift;
        if (num_bits > bits_from_window) {
            value |= std::uint64_t{bytes_[byte_offset + sizeof(std::uint64_t)]} << bits_from_window;
        }
        if (num_bits < 64U) {
            value &= (std::uint64_t{1} << num_bits) - 1U;
        }
        read_bit_ += num_bits;
        return value;
    }

    void read_bytes(char* out, std::size_t num_bytes) {
        if (out == nullptr && num_bytes != 0) {
            throw std::invalid_argument("bit buffer cannot read bytes into null data");
        }
        if (num_bytes == 0) {
            return;
        }
        ensure_can_read(num_bytes * 8U);

        if ((read_bit_ % 8U) == 0) {
            std::memcpy(out, bytes_.data() + (read_bit_ / 8U), num_bytes);
            read_bit_ += num_bytes * 8U;
            return;
        }

        for (std::size_t index = 0; index < num_bytes; ++index) {
            out[index] = static_cast<char>(read_bits(8U));
        }
    }

    friend bool operator==(const BitBuffer& lhs, const BitBuffer& rhs) noexcept {
        return lhs.bit_size_ == rhs.bit_size_ && lhs.bytes_ == rhs.bytes_;
    }

    friend bool operator!=(const BitBuffer& lhs, const BitBuffer& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    void ensure_can_read(std::size_t num_bits) const {
        if (num_bits > remaining_bits()) {
            throw std::out_of_range("bit buffer read past end");
        }
    }

    std::vector<std::uint8_t> bytes_;
    std::size_t bit_size_ = 0;
    std::size_t read_bit_ = 0;
};

}  // namespace kage::sync
