#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
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

        for (std::size_t bit = 0; bit < num_bits; ++bit) {
            push_bool(((value >> bit) & 1U) != 0);
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
        for (std::size_t bit = 0; bit < source.bit_size_; ++bit) {
            const bool value =
                (source.bytes_[bit / 8U] & static_cast<std::uint8_t>(1U << (bit % 8U))) != 0;
            push_bool(value);
        }
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
        ensure_can_read(num_bits);

        std::uint64_t value = 0;
        for (std::size_t bit = 0; bit < num_bits; ++bit) {
            if (read_bool()) {
                value |= (std::uint64_t{1} << bit);
            }
        }
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
