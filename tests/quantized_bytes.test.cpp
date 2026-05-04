#include "kage/sync/types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace {

std::array<std::uint8_t, kage::sync::QuantizedBytes::max_size> make_pattern() {
    std::array<std::uint8_t, kage::sync::QuantizedBytes::max_size> out{};
    for (std::size_t index = 0; index < out.size(); ++index) {
        out[index] = static_cast<std::uint8_t>((index * 13U + 7U) & 0xffU);
    }
    return out;
}

void require_prefix(
    const kage::sync::QuantizedBytes& bytes,
    const std::array<std::uint8_t, kage::sync::QuantizedBytes::max_size>& pattern,
    std::size_t size) {
    REQUIRE(bytes.size() >= size);
    for (std::size_t index = 0; index < size; ++index) {
        REQUIRE(bytes.data()[index] == pattern[index]);
    }
}

void require_matches(
    const kage::sync::QuantizedBytes& bytes,
    const std::array<std::uint8_t, kage::sync::QuantizedBytes::max_size>& pattern,
    std::size_t size) {
    REQUIRE(bytes.size() == size);
    require_prefix(bytes, pattern, size);
}

}  // namespace

TEST_CASE("quantized bytes stores inline and overflow payloads") {
    static_assert(kage::sync::QuantizedBytes::inline_capacity == 64);
    const auto pattern = make_pattern();

    for (std::size_t size : std::array<std::size_t, 6>{
             0U,
             16U,
             32U,
             64U,
             65U,
             kage::sync::QuantizedBytes::max_size}) {
        kage::sync::QuantizedBytes bytes;
        bytes.assign(pattern.data(), size);
        require_matches(bytes, pattern, size);
    }
}

TEST_CASE("quantized bytes copies only the active payload") {
    const auto pattern = make_pattern();
    kage::sync::QuantizedBytes source;
    source.assign(pattern.data(), 65U);

    kage::sync::QuantizedBytes copy(source);
    require_matches(copy, pattern, 65U);
    REQUIRE(copy == source);

    kage::sync::QuantizedBytes assigned;
    assigned.assign(pattern.data(), 16U);
    assigned = source;
    require_matches(assigned, pattern, 65U);
    REQUIRE(assigned == source);

    assigned = assigned;
    require_matches(assigned, pattern, 65U);
}

TEST_CASE("quantized bytes moves payloads and leaves source reusable") {
    const auto pattern = make_pattern();
    kage::sync::QuantizedBytes source;
    source.assign(pattern.data(), 1200U);

    kage::sync::QuantizedBytes moved(std::move(source));
    require_matches(moved, pattern, 1200U);
    REQUIRE(source.empty());

    source.assign(pattern.data(), 32U);
    require_matches(source, pattern, 32U);

    kage::sync::QuantizedBytes assigned;
    assigned = std::move(moved);
    require_matches(assigned, pattern, 1200U);
    REQUIRE(moved.empty());
}

TEST_CASE("quantized bytes preserves payload prefixes across inline and overflow resizes") {
    const auto pattern = make_pattern();
    kage::sync::QuantizedBytes bytes;

    bytes.assign(pattern.data(), 32U);
    bytes.resize(65U);
    require_prefix(bytes, pattern, 32U);
    REQUIRE(bytes.size() == 65U);

    bytes.assign(pattern.data(), 65U);
    bytes.resize(16U);
    require_matches(bytes, pattern, 16U);

    bytes.resize(1200U);
    require_prefix(bytes, pattern, 16U);
    REQUIRE(bytes.size() == 1200U);
}

TEST_CASE("quantized bytes rejects payloads above maximum size") {
    kage::sync::QuantizedBytes bytes;
    REQUIRE_THROWS_AS(bytes.resize(kage::sync::QuantizedBytes::max_size + 1U), std::length_error);
}
