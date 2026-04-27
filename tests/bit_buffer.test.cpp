#include "kage/sync/bit_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

TEST_CASE("bit buffer pushes and reads bits, bytes, and bools") {
    kage::sync::BitBuffer buffer;
    buffer.push_bool(true);
    buffer.push_bits(0b101, 3U);
    buffer.push_bytes("AZ", 2U);

    REQUIRE(buffer.bit_size() == 20);
    REQUIRE(buffer.byte_size() == 3);
    REQUIRE(buffer.read_bool());
    REQUIRE(buffer.read_bits(3U) == 0b101);

    char bytes[2]{};
    buffer.read_bytes(bytes, 2U);
    REQUIRE(bytes[0] == 'A');
    REQUIRE(bytes[1] == 'Z');
    REQUIRE_THROWS_AS(buffer.read_bool(), std::out_of_range);
}
