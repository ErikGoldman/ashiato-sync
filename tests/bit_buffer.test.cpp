#include "ecs/bit_buffer.hpp"
#include "kage/sync/delta.hpp"
#include "kage/sync/detail/bit_reader.hpp"
#include "kage/sync/protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("bit buffer pushes and reads bits, bytes, and bools") {
    ecs::BitBuffer buffer;
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
    ecs::BitBuffer buffer;
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
    ecs::BitBuffer buffer;
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
    ecs::BitBuffer buffer;
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

TEST_CASE("bit buffer truncates written bits without reallocating forward") {
    ecs::BitBuffer buffer;
    buffer.push_bits(0b101, 3U);
    buffer.push_unsigned_bits(0xffU, 8U);
    buffer.push_bits(0b10, 2U);
    REQUIRE(buffer.bit_size() == 13U);

    buffer.truncate_bits(11U);
    REQUIRE(buffer.bit_size() == 11U);
    REQUIRE(buffer.byte_size() == 2U);
    REQUIRE(buffer.read_bits(3U) == 0b101);
    REQUIRE(buffer.read_unsigned_bits(8U) == 0xffU);
    REQUIRE(buffer.remaining_bits() == 0U);

    buffer.reset_read();
    buffer.truncate_bits(2U);
    REQUIRE(buffer.bit_size() == 2U);
    REQUIRE(buffer.byte_size() == 1U);
    REQUIRE(buffer.read_bits(2U) == 0b01);
    REQUIRE(buffer.remaining_bits() == 0U);
    REQUIRE_THROWS_AS(buffer.truncate_bits(3U), std::out_of_range);
}

TEST_CASE("bit buffer truncation clamps read offset") {
    ecs::BitBuffer buffer;
    buffer.push_unsigned_bits(0xabU, 8U);
    REQUIRE(buffer.read_unsigned_bits(6U) == 0x2bU);
    REQUIRE(buffer.read_offset_bits() == 6U);

    buffer.truncate_bits(4U);
    REQUIRE(buffer.bit_size() == 4U);
    REQUIRE(buffer.read_offset_bits() == 4U);
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit buffer validates invalid read and write requests") {
    ecs::BitBuffer buffer;

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

TEST_CASE("bit reader guards reads and writes directly into typed outputs") {
    ecs::BitBuffer buffer;
    buffer.push_bool(true);
    buffer.push_unsigned_bits(0xabU, 8U);
    buffer.push_unsigned_bits(0xffU, 8U);
    buffer.push_unsigned_bits(0x7fU, 8U);
    buffer.push_unsigned_bits(0xffffffffULL, 32U);

    kage::sync::detail::BitReader reader(buffer);
    bool flag = false;
    std::uint8_t value = 0;
    std::int32_t negative_8 = 0;
    std::int32_t positive_8 = 0;
    std::int32_t negative_32 = 0;

    REQUIRE(reader.read_bits(1U, flag));
    REQUIRE(flag);
    REQUIRE(reader.read_bits(8U, value));
    REQUIRE(value == 0xabU);
    REQUIRE(reader.read_signed_bits(8U, negative_8));
    REQUIRE(negative_8 == -1);
    REQUIRE(reader.read_signed_bits(8U, positive_8));
    REQUIRE(positive_8 == 127);
    REQUIRE(reader.read_signed_bits(32U, negative_32));
    REQUIRE(negative_32 == -1);
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit reader failed reads do not advance the buffer") {
    ecs::BitBuffer buffer;
    buffer.push_bits(0b101, 3U);

    kage::sync::detail::BitReader reader(buffer);
    std::uint8_t value = 0;
    REQUIRE_FALSE(reader.read_bits(4U, value));
    REQUIRE(buffer.read_offset_bits() == 0U);
    REQUIRE(reader.read_bits(3U, value));
    REQUIRE(value == 0b101U);
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit buffer overwrites existing aligned and unaligned bit ranges") {
    ecs::BitBuffer aligned;
    aligned.push_bits(0, 32U);
    aligned.overwrite_unsigned_bits(0, 0xfeedfaceU, 32U);
    REQUIRE(aligned.read_unsigned_bits(32U) == 0xfeedfaceU);

    ecs::BitBuffer unaligned;
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
    ecs::BitBuffer source;
    source.push_bool(true);
    source.push_bits(0b010011, 6U);

    ecs::BitBuffer combined;
    combined.push_bool(false);
    combined.push_buffer_bits(source);

    REQUIRE_FALSE(combined.read_bool());
    REQUIRE(combined.read_bool());
    REQUIRE(combined.read_bits(6U) == 0b010011);
    REQUIRE(combined.remaining_bits() == 0);
}

TEST_CASE("bit buffer round-trips deterministic randomized unaligned integer fields") {
    std::mt19937 rng(12345);
    std::vector<std::pair<std::uint64_t, std::size_t>> fields;
    ecs::BitBuffer buffer;
    buffer.push_bits(0b101, 3U);

    for (int field = 0; field < 128; ++field) {
        const std::size_t bits = 1U + (rng() % 64U);
        const std::uint64_t mask = bits == 64U ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1U);
        const std::uint64_t value =
            ((static_cast<std::uint64_t>(rng()) << 32U) | static_cast<std::uint64_t>(rng())) & mask;
        fields.push_back({value, bits});
        buffer.push_unsigned_bits(value, bits);
    }

    REQUIRE(buffer.read_bits(3U) == 0b101);
    for (const auto& [value, bits] : fields) {
        REQUIRE(buffer.read_unsigned_bits(bits) == value);
    }
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("protocol baseline frame encoding uses relative deltas when possible") {
    ecs::BitBuffer relative;
    kage::sync::protocol::write_baseline_frame(relative, 40U, 35U);
    REQUIRE(relative.bit_size() == 1U + kage::sync::protocol::baseline_frame_delta_bits);

    std::uint32_t decoded = 0;
    REQUIRE(kage::sync::protocol::read_baseline_frame(relative, 40U, decoded));
    REQUIRE(decoded == 35U);
    REQUIRE(relative.remaining_bits() == 0U);

    ecs::BitBuffer full;
    kage::sync::protocol::write_baseline_frame(
        full,
        40U,
        40U - kage::sync::protocol::max_baseline_frame_delta - 1U);
    REQUIRE(full.bit_size() == 33U);

    REQUIRE(kage::sync::protocol::read_baseline_frame(full, 40U, decoded));
    REQUIRE(decoded == 40U - kage::sync::protocol::max_baseline_frame_delta - 1U);
    REQUIRE(full.remaining_bits() == 0U);
}

TEST_CASE("protocol baseline frame encoding round-trips sampled frame pairs") {
    for (std::uint32_t current = 0; current < 256U; current += 17U) {
        for (std::uint32_t baseline : {
                 0U,
                 current,
                 current > 3U ? current - 3U : 0U,
                 current + 11U,
                 current > kage::sync::protocol::max_baseline_frame_delta + 2U
                     ? current - kage::sync::protocol::max_baseline_frame_delta - 2U
                     : 0U}) {
            ecs::BitBuffer buffer;
            kage::sync::protocol::write_baseline_frame(buffer, current, baseline);
            std::uint32_t decoded = 0;
            REQUIRE(kage::sync::protocol::read_baseline_frame(buffer, current, decoded));
            REQUIRE(decoded == baseline);
            REQUIRE(buffer.remaining_bits() == 0U);
        }
    }
}

TEST_CASE("protocol network entity id encoding uses compact tiers") {
    for (const std::size_t tier0_bits : {8U, 11U, 15U}) {
        const std::uint32_t tier0_max = kage::sync::protocol::network_entity_id_tier0_max(tier0_bits);
        const std::uint32_t ids[] = {
            1U,
            tier0_max,
            tier0_max + 1U,
            kage::sync::protocol::network_entity_id_tier1_max,
            kage::sync::protocol::network_entity_id_tier1_max + 1U,
            0xffffffffU,
        };

        for (const std::uint32_t id : ids) {
            ecs::BitBuffer buffer;
            kage::sync::protocol::write_network_entity_id(buffer, id, tier0_bits);
            REQUIRE(buffer.bit_size() ==
                    kage::sync::protocol::network_entity_id_encoded_bits(id, tier0_bits));

            std::uint32_t decoded = 0;
            REQUIRE(kage::sync::protocol::read_network_entity_id(buffer, decoded, tier0_bits));
            REQUIRE(decoded == id);
            REQUIRE(buffer.remaining_bits() == 0U);
        }
    }
}

TEST_CASE("protocol network entity id encoding round-trips sampled ids across tier widths") {
    std::mt19937 rng(6789);
    for (const std::size_t tier0_bits : {1U, 4U, 8U, 11U, 22U}) {
        for (int sample = 0; sample < 128; ++sample) {
            const std::uint32_t id = rng();
            ecs::BitBuffer buffer;
            kage::sync::protocol::write_network_entity_id(buffer, id, tier0_bits);
            REQUIRE(buffer.bit_size() ==
                    kage::sync::protocol::network_entity_id_encoded_bits(id, tier0_bits));

            std::uint32_t decoded = 0;
            REQUIRE(kage::sync::protocol::read_network_entity_id(buffer, decoded, tier0_bits));
            REQUIRE(decoded == id);
            REQUIRE(buffer.remaining_bits() == 0U);
        }
    }
}

TEST_CASE("protocol strings round-trip and reject truncated payloads") {
    const std::vector<std::string> values = {"", "a", "hello", std::string(32, 'x')};
    for (const std::string& value : values) {
        ecs::BitBuffer buffer;
        kage::sync::protocol::write_string(buffer, value);
        std::string decoded;
        REQUIRE(kage::sync::protocol::read_string(buffer, decoded));
        REQUIRE(decoded == value);
        REQUIRE(buffer.remaining_bits() == 0U);
    }

    ecs::BitBuffer truncated_length;
    truncated_length.push_bits(1, 8U);
    std::string decoded;
    REQUIRE_FALSE(kage::sync::protocol::read_string(truncated_length, decoded));

    ecs::BitBuffer truncated_payload;
    truncated_payload.push_bits(4, 16U);
    truncated_payload.push_bytes("xy", 2U);
    REQUIRE_FALSE(kage::sync::protocol::read_string(truncated_payload, decoded));
}

TEST_CASE("delta helpers quantize floats and vectors with changed-value masks") {
    const kage::sync::delta::FloatConfig config{-10.0f, 10.0f, 0.25f};

    ecs::BitBuffer scalar;
    kage::sync::delta::write_float(scalar, 1.12f, config);
    REQUIRE(scalar.bit_size() == kage::sync::delta::float_bits(config));
    float scalar_out = 0.0f;
    REQUIRE(kage::sync::delta::read_float(scalar, config, scalar_out));
    REQUIRE(scalar_out == Catch::Approx(1.0f));

    ecs::BitBuffer delta;
    kage::sync::delta::write_delta_vec3(
        delta,
        kage::sync::delta::Vec3{1.0f, 2.0f, 3.0f},
        kage::sync::delta::Vec3{1.0f, 2.5f, 3.0f},
        config);
    REQUIRE(delta.bit_size() == 3U + kage::sync::delta::float_bits(config));

    kage::sync::delta::Vec3 decoded;
    REQUIRE(kage::sync::delta::read_delta_vec3(
        delta,
        kage::sync::delta::Vec3{1.0f, 2.0f, 3.0f},
        config,
        decoded));
    REQUIRE(decoded.x == Catch::Approx(1.0f));
    REQUIRE(decoded.y == Catch::Approx(2.5f));
    REQUIRE(decoded.z == Catch::Approx(3.0f));
}

TEST_CASE("delta helpers encode integer and quaternion deltas") {
    ecs::BitBuffer ints;
    kage::sync::delta::write_delta_int(ints, 100, 93, 8U);
    std::int64_t int_out = 0;
    REQUIRE(kage::sync::delta::read_delta_int(ints, 100, 8U, int_out));
    REQUIRE(int_out == 93);

    const kage::sync::delta::FloatConfig quat_config{-1.0f, 1.0f, 0.01f};
    ecs::BitBuffer quaternion;
    kage::sync::delta::write_delta_quaternion(
        quaternion,
        kage::sync::delta::Quaternion{0.0f, 0.0f, 0.0f, 1.0f},
        kage::sync::delta::Quaternion{0.0f, 0.25f, 0.0f, 0.97f},
        quat_config);

    kage::sync::delta::Quaternion out;
    REQUIRE(kage::sync::delta::read_delta_quaternion(
        quaternion,
        kage::sync::delta::Quaternion{0.0f, 0.0f, 0.0f, 1.0f},
        quat_config,
        out));
    REQUIRE(out.x == Catch::Approx(0.0f));
    REQUIRE(out.y == Catch::Approx(0.25f));
    REQUIRE(out.z == Catch::Approx(0.0f));
    REQUIRE(out.w == Catch::Approx(0.97f));
}
