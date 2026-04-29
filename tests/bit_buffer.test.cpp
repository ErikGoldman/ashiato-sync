#include "kage/sync/bit_buffer.hpp"
#include "kage/sync/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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

TEST_CASE("bit buffer handles unaligned byte payloads") {
    kage::sync::BitBuffer buffer;
    buffer.push_bits(0b11, 2U);
    buffer.push_bytes("BC", 2U);

    REQUIRE(buffer.bit_size() == 18);
    REQUIRE(buffer.read_bits(2U) == 0b11);

    char bytes[2]{};
    buffer.read_bytes(bytes, 2U);
    REQUIRE(bytes[0] == 'B');
    REQUIRE(bytes[1] == 'C');
    REQUIRE(buffer.remaining_bits() == 0);
}

TEST_CASE("bit buffer handles unaligned 64-bit integer payloads") {
    kage::sync::BitBuffer buffer;
    buffer.push_bits(0b101, 3U);
    buffer.push_unsigned_bits(0xfedcba9876543210ULL, 64U);
    buffer.push_unsigned_bits(0x0123456789abcdefULL, 64U);
    buffer.push_bits(0b11, 2U);

    REQUIRE(buffer.read_bits(3U) == 0b101);
    REQUIRE(buffer.read_unsigned_bits(64U) == 0xfedcba9876543210ULL);
    REQUIRE(buffer.read_unsigned_bits(64U) == 0x0123456789abcdefULL);
    REQUIRE(buffer.read_bits(2U) == 0b11);
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit buffer reset and clear preserve expected offsets") {
    kage::sync::BitBuffer buffer;
    buffer.push_unsigned_bits(0xfeedfaceULL, 32U);
    REQUIRE(buffer.read_unsigned_bits(16U) == 0xfaceU);
    REQUIRE(buffer.read_offset_bits() == 16);

    buffer.reset_read();
    REQUIRE(buffer.read_unsigned_bits(32U) == 0xfeedfaceULL);

    buffer.clear();
    REQUIRE(buffer.empty());
    REQUIRE(buffer.bit_size() == 0);
    REQUIRE(buffer.read_offset_bits() == 0);
}

TEST_CASE("bit buffer validates invalid read and write requests") {
    kage::sync::BitBuffer buffer;

    REQUIRE_NOTHROW(buffer.push_bytes(nullptr, 0U));
    REQUIRE_THROWS_AS(buffer.push_bytes(nullptr, 1U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.push_bits(0, 65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.overwrite_unsigned_bits(0, 0, 65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.overwrite_unsigned_bits(0, 0, 1U), std::out_of_range);

    char out = 0;
    REQUIRE_NOTHROW(buffer.read_bytes(nullptr, 0U));
    REQUIRE_THROWS_AS(buffer.read_bytes(nullptr, 1U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_unsigned_bits(65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_bytes(&out, 1U), std::out_of_range);
}

TEST_CASE("bit buffer overwrites existing aligned and unaligned bit ranges") {
    kage::sync::BitBuffer aligned;
    aligned.push_bits(0, 32U);
    aligned.overwrite_unsigned_bits(0, 0xfeedfaceU, 32U);
    REQUIRE(aligned.read_unsigned_bits(32U) == 0xfeedfaceU);

    kage::sync::BitBuffer unaligned;
    unaligned.push_bits(0b101, 3U);
    const std::size_t patch_offset = unaligned.bit_size();
    unaligned.push_bits(0, 32U);
    unaligned.push_bits(0b11, 2U);

    REQUIRE(unaligned.read_bits(3U) == 0b101);
    REQUIRE(unaligned.read_offset_bits() == 3U);

    unaligned.overwrite_unsigned_bits(patch_offset, 0xcafebabeU, 32U);

    REQUIRE(unaligned.bit_size() == 37U);
    REQUIRE(unaligned.read_offset_bits() == 3U);
    REQUIRE(unaligned.read_unsigned_bits(32U) == 0xcafebabeU);
    REQUIRE(unaligned.read_bits(2U) == 0b11);
    REQUIRE(unaligned.remaining_bits() == 0U);
}

TEST_CASE("bit buffer appends source buffers bit-exactly") {
    kage::sync::BitBuffer source;
    source.push_bool(true);
    source.push_bits(0b010011, 6U);

    kage::sync::BitBuffer combined;
    combined.push_bool(false);
    combined.push_buffer_bits(source);

    REQUIRE_FALSE(combined.read_bool());
    REQUIRE(combined.read_bool());
    REQUIRE(combined.read_bits(6U) == 0b010011);
    REQUIRE(combined.remaining_bits() == 0);
}

TEST_CASE("protocol baseline frame encoding uses relative deltas when possible") {
    kage::sync::BitBuffer relative;
    kage::sync::protocol::write_baseline_frame(relative, 40U, 35U);
    REQUIRE(relative.bit_size() == 1U + kage::sync::protocol::baseline_frame_delta_bits);

    std::uint32_t decoded = 0;
    REQUIRE(kage::sync::protocol::read_baseline_frame(relative, 40U, decoded));
    REQUIRE(decoded == 35U);
    REQUIRE(relative.remaining_bits() == 0U);

    kage::sync::BitBuffer full;
    kage::sync::protocol::write_baseline_frame(
        full,
        40U,
        40U - kage::sync::protocol::max_baseline_frame_delta - 1U);
    REQUIRE(full.bit_size() == 33U);

    REQUIRE(kage::sync::protocol::read_baseline_frame(full, 40U, decoded));
    REQUIRE(decoded == 40U - kage::sync::protocol::max_baseline_frame_delta - 1U);
    REQUIRE(full.remaining_bits() == 0U);
}

TEST_CASE("protocol network entity id encoding uses compact tiers") {
    const std::uint32_t ids[] = {
        1U,
        kage::sync::protocol::network_entity_id_tier0_max(),
        kage::sync::protocol::network_entity_id_tier0_max() + 1U,
        kage::sync::protocol::network_entity_id_tier1_max,
        kage::sync::protocol::network_entity_id_tier1_max + 1U,
        0xffffffffU,
    };

    for (const std::uint32_t id : ids) {
        kage::sync::BitBuffer buffer;
        kage::sync::protocol::write_network_entity_id(buffer, id);
        REQUIRE(buffer.bit_size() == kage::sync::protocol::network_entity_id_encoded_bits(id));

        std::uint32_t decoded = 0;
        REQUIRE(kage::sync::protocol::read_network_entity_id(buffer, decoded));
        REQUIRE(decoded == id);
        REQUIRE(buffer.remaining_bits() == 0U);
    }

    constexpr std::size_t narrow_tier_bits = 8U;
    kage::sync::BitBuffer narrow;
    kage::sync::protocol::write_network_entity_id(narrow, 255U, narrow_tier_bits);
    REQUIRE(narrow.bit_size() == 9U);
    std::uint32_t decoded = 0;
    REQUIRE(kage::sync::protocol::read_network_entity_id(narrow, decoded, narrow_tier_bits));
    REQUIRE(decoded == 255U);
}
