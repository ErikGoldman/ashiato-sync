#include "ashiato/bit_buffer.hpp"
#include "ashiato/sync/delta.hpp"
#include "ashiato/sync/detail/bit_reader.hpp"
#include "ashiato/sync/protocol.hpp"
#include "ashiato/sync/serialization.hpp"
#include "ashiato/sync/tracing.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("bit buffer pushes and reads bits, bytes, and bools") {
    ashiato::BitBuffer buffer;
    buffer.write_bool(true);
    buffer.write_bits(0b101, 3U);
    buffer.write_bytes("AZ", 2U);

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
    ashiato::BitBuffer buffer;
    buffer.write_bits(0b11, 2U);
    buffer.write_bytes("BC", 2U);

    REQUIRE(buffer.bit_size() == 18);
    REQUIRE(buffer.read_bits(2U) == 0b11);

    char bytes[2]{};
    buffer.read_bytes(bytes, 2U);
    REQUIRE(bytes[0] == 'B');
    REQUIRE(bytes[1] == 'C');
    REQUIRE(buffer.remaining_bits() == 0);
}

TEST_CASE("bit buffer handles unaligned 64-bit integer payloads") {
    ashiato::BitBuffer buffer;
    buffer.write_bits(0b101, 3U);
    buffer.write_unsigned_bits(0xfedcba9876543210ULL, 64U);
    buffer.write_unsigned_bits(0x0123456789abcdefULL, 64U);
    buffer.write_bits(0b11, 2U);

    REQUIRE(buffer.read_bits(3U) == 0b101);
    REQUIRE(buffer.read_unsigned_bits(64U) == 0xfedcba9876543210ULL);
    REQUIRE(buffer.read_unsigned_bits(64U) == 0x0123456789abcdefULL);
    REQUIRE(buffer.read_bits(2U) == 0b11);
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit buffer reset and clear preserve expected offsets") {
    ashiato::BitBuffer buffer;
    buffer.write_unsigned_bits(0xfeedfaceULL, 32U);
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
    ashiato::BitBuffer buffer;
    buffer.write_bits(0b101, 3U);
    buffer.write_unsigned_bits(0xffU, 8U);
    buffer.write_bits(0b10, 2U);
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
    ashiato::BitBuffer buffer;
    buffer.write_unsigned_bits(0xabU, 8U);
    REQUIRE(buffer.read_unsigned_bits(6U) == 0x2bU);
    REQUIRE(buffer.read_offset_bits() == 6U);

    buffer.truncate_bits(4U);
    REQUIRE(buffer.bit_size() == 4U);
    REQUIRE(buffer.read_offset_bits() == 4U);
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit buffer validates invalid read and write requests") {
    ashiato::BitBuffer buffer;

    REQUIRE_NOTHROW(buffer.write_bytes(nullptr, 0U));
    REQUIRE_THROWS_AS(buffer.write_bytes(nullptr, 1U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.write_bits(0, 65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.overwrite_unsigned_bits(0, 0, 65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.overwrite_unsigned_bits(0, 0, 1U), std::out_of_range);

    char out = 0;
    REQUIRE_NOTHROW(buffer.read_bytes(nullptr, 0U));
    REQUIRE_THROWS_AS(buffer.read_bytes(nullptr, 1U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_unsigned_bits(65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_bytes(&out, 1U), std::out_of_range);
}

TEST_CASE("bit reader guards reads and writes directly into typed outputs") {
    ashiato::BitBuffer buffer;
    buffer.write_bool(true);
    buffer.write_unsigned_bits(0xabU, 8U);
    buffer.write_unsigned_bits(0xffU, 8U);
    buffer.write_unsigned_bits(0x7fU, 8U);
    buffer.write_unsigned_bits(0xffffffffULL, 32U);

    ashiato::sync::detail::BitReader reader(buffer);
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
    ashiato::BitBuffer buffer;
    buffer.write_bits(0b101, 3U);

    ashiato::sync::detail::BitReader reader(buffer);
    std::uint8_t value = 0;
    REQUIRE_FALSE(reader.read_bits(4U, value));
    REQUIRE(buffer.read_offset_bits() == 0U);
    REQUIRE(reader.read_bits(3U, value));
    REQUIRE(value == 0b101U);
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit buffer overwrites existing aligned and unaligned bit ranges") {
    ashiato::BitBuffer aligned;
    aligned.write_bits(0, 32U);
    aligned.overwrite_unsigned_bits(0, 0xfeedfaceU, 32U);
    REQUIRE(aligned.read_unsigned_bits(32U) == 0xfeedfaceU);

    ashiato::BitBuffer unaligned;
    unaligned.write_bits(0b101, 3U);
    const std::size_t patch_offset = unaligned.bit_size();
    unaligned.write_bits(0, 32U);
    unaligned.write_bits(0b11, 2U);

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
    ashiato::BitBuffer source;
    source.write_bool(true);
    source.write_bits(0b010011, 6U);

    ashiato::BitBuffer combined;
    combined.write_bool(false);
    combined.write_buffer_bits(source);

    REQUIRE_FALSE(combined.read_bool());
    REQUIRE(combined.read_bool());
    REQUIRE(combined.read_bits(6U) == 0b010011);
    REQUIRE(combined.remaining_bits() == 0);
}

TEST_CASE("bit buffer round-trips deterministic randomized unaligned integer fields") {
    std::mt19937 rng(12345);
    std::vector<std::pair<std::uint64_t, std::size_t>> fields;
    ashiato::BitBuffer buffer;
    buffer.write_bits(0b101, 3U);

    for (int field = 0; field < 128; ++field) {
        const std::size_t bits = 1U + (rng() % 64U);
        const std::uint64_t mask = bits == 64U ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1U);
        const std::uint64_t value =
            ((static_cast<std::uint64_t>(rng()) << 32U) | static_cast<std::uint64_t>(rng())) & mask;
        fields.push_back({value, bits});
        buffer.write_unsigned_bits(value, bits);
    }

    REQUIRE(buffer.read_bits(3U) == 0b101);
    for (const auto& [value, bits] : fields) {
        REQUIRE(buffer.read_unsigned_bits(bits) == value);
    }
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("protocol baseline frame encoding uses relative deltas when possible") {
    ashiato::BitBuffer relative;
    ashiato::sync::protocol::write_baseline_frame(relative, 40U, 35U);
    REQUIRE(relative.bit_size() == 1U + ashiato::sync::protocol::baseline_frame_delta_bits);
    REQUIRE(relative.read_bool() == false);

    relative.reset_read();
    std::uint32_t decoded = 0;
    REQUIRE(ashiato::sync::protocol::read_baseline_frame(relative, 40U, decoded));
    REQUIRE(decoded == 35U);
    REQUIRE(relative.remaining_bits() == 0U);

    ashiato::BitBuffer full;
    ashiato::sync::protocol::write_baseline_frame(
        full,
        40U,
        40U - ashiato::sync::protocol::max_baseline_frame_delta - 1U);
    REQUIRE(full.bit_size() == 33U);
    REQUIRE(full.read_bool() == true);

    full.reset_read();
    REQUIRE(ashiato::sync::protocol::read_baseline_frame(full, 40U, decoded));
    REQUIRE(decoded == 40U - ashiato::sync::protocol::max_baseline_frame_delta - 1U);
    REQUIRE(full.remaining_bits() == 0U);
}

TEST_CASE("protocol baseline frame encoding round-trips sampled frame pairs") {
    for (std::uint32_t current = 0; current < 256U; current += 17U) {
        for (std::uint32_t baseline : {
                 0U,
                 current,
                 current > 3U ? current - 3U : 0U,
                 current + 11U,
                 current > ashiato::sync::protocol::max_baseline_frame_delta + 2U
                     ? current - ashiato::sync::protocol::max_baseline_frame_delta - 2U
                     : 0U}) {
            ashiato::BitBuffer buffer;
            ashiato::sync::protocol::write_baseline_frame(buffer, current, baseline);
            std::uint32_t decoded = 0;
            REQUIRE(ashiato::sync::protocol::read_baseline_frame(buffer, current, decoded));
            REQUIRE(decoded == baseline);
            REQUIRE(buffer.remaining_bits() == 0U);
        }
    }
}

TEST_CASE("protocol cue frame encoding uses zero-prefixed relative frames when possible") {
    ashiato::BitBuffer relative;
    ashiato::sync::protocol::write_cue_frame(relative, 200U, 194U);
    REQUIRE(relative.bit_size() == 1U + ashiato::sync::protocol::cue_frame_delta_bits);
    REQUIRE(relative.read_bool() == false);
    REQUIRE(relative.read_bits(ashiato::sync::protocol::cue_frame_delta_bits) == 6U);

    relative.reset_read();
    std::uint32_t decoded = 0;
    REQUIRE(ashiato::sync::protocol::read_cue_frame(relative, 200U, decoded));
    REQUIRE(decoded == 194U);
    REQUIRE(relative.remaining_bits() == 0U);
}

TEST_CASE("protocol cue frame encoding uses one-prefixed full frames when needed") {
    for (const std::uint32_t cue_frame : {72U, 201U}) {
        ashiato::BitBuffer full;
        ashiato::sync::protocol::write_cue_frame(full, 200U, cue_frame);
        REQUIRE(full.bit_size() == 33U);
        REQUIRE(full.read_bool() == true);
        REQUIRE(full.read_bits(32U) == cue_frame);

        full.reset_read();
        std::uint32_t decoded = 0;
        REQUIRE(ashiato::sync::protocol::read_cue_frame(full, 200U, decoded));
        REQUIRE(decoded == cue_frame);
        REQUIRE(full.remaining_bits() == 0U);
    }
}

TEST_CASE("protocol network entity id encoding uses compact tiers") {
    for (const std::size_t tier0_bits : {8U, 11U, 15U}) {
        const std::uint32_t tier0_max = ashiato::sync::protocol::network_entity_id_tier0_max(tier0_bits);
        const std::uint32_t ids[] = {
            1U,
            tier0_max,
            tier0_max + 1U,
            ashiato::sync::protocol::network_entity_id_tier1_max,
            ashiato::sync::protocol::network_entity_id_tier1_max + 1U,
            0xffffffffU,
        };

        for (const std::uint32_t id : ids) {
            ashiato::BitBuffer buffer;
            ashiato::sync::protocol::write_network_entity_id(buffer, id, tier0_bits);
            REQUIRE(buffer.bit_size() ==
                    ashiato::sync::protocol::network_entity_id_encoded_bits(id, tier0_bits));

            std::uint32_t decoded = 0;
            REQUIRE(ashiato::sync::protocol::read_network_entity_id(buffer, decoded, tier0_bits));
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
            ashiato::BitBuffer buffer;
            ashiato::sync::protocol::write_network_entity_id(buffer, id, tier0_bits);
            REQUIRE(buffer.bit_size() ==
                    ashiato::sync::protocol::network_entity_id_encoded_bits(id, tier0_bits));

            std::uint32_t decoded = 0;
            REQUIRE(ashiato::sync::protocol::read_network_entity_id(buffer, decoded, tier0_bits));
            REQUIRE(decoded == id);
            REQUIRE(buffer.remaining_bits() == 0U);
        }
    }
}

TEST_CASE("protocol strings round-trip and reject truncated payloads") {
    const std::vector<std::string> values = {"", "a", "hello", std::string(32, 'x')};
    for (const std::string& value : values) {
        ashiato::BitBuffer buffer;
        ashiato::sync::protocol::write_string(buffer, value);
        std::string decoded;
        REQUIRE(ashiato::sync::protocol::read_string(buffer, decoded));
        REQUIRE(decoded == value);
        REQUIRE(buffer.remaining_bits() == 0U);
    }

    ashiato::BitBuffer truncated_length;
    truncated_length.write_bits(1, 8U);
    std::string decoded;
    REQUIRE_FALSE(ashiato::sync::protocol::read_string(truncated_length, decoded));

    ashiato::BitBuffer truncated_payload;
    truncated_payload.write_bits(4, 16U);
    truncated_payload.write_bytes("xy", 2U);
    REQUIRE_FALSE(ashiato::sync::protocol::read_string(truncated_payload, decoded));
}

TEST_CASE("delta helpers quantize floats and vectors with changed-value masks") {
    const ashiato::sync::delta::FloatConfig config{-10.0f, 10.0f, 0.25f};

    ashiato::BitBuffer scalar;
    ashiato::sync::delta::write_float(scalar, 1.12f, config);
    REQUIRE(scalar.bit_size() == ashiato::sync::delta::float_bits(config));
    float scalar_out = 0.0f;
    REQUIRE(ashiato::sync::delta::read_float(scalar, config, scalar_out));
    REQUIRE(scalar_out == Catch::Approx(1.0f));

    ashiato::BitBuffer delta;
    ashiato::sync::delta::write_delta_vec3(
        delta,
        ashiato::sync::delta::Vec3{1.0f, 2.0f, 3.0f},
        ashiato::sync::delta::Vec3{1.0f, 2.5f, 3.0f},
        config);
    REQUIRE(delta.bit_size() == 3U + ashiato::sync::delta::float_bits(config));

    ashiato::sync::delta::Vec3 decoded;
    REQUIRE(ashiato::sync::delta::read_delta_vec3(
        delta,
        ashiato::sync::delta::Vec3{1.0f, 2.0f, 3.0f},
        config,
        decoded));
    REQUIRE(decoded.x == Catch::Approx(1.0f));
    REQUIRE(decoded.y == Catch::Approx(2.5f));
    REQUIRE(decoded.z == Catch::Approx(3.0f));
}

TEST_CASE("delta helpers encode integer and quaternion deltas") {
    ashiato::BitBuffer ints;
    ashiato::sync::delta::write_delta_int(ints, 100, 93, 8U);
    std::int64_t int_out = 0;
    REQUIRE(ashiato::sync::delta::read_delta_int(ints, 100, 8U, int_out));
    REQUIRE(int_out == 93);

    const ashiato::sync::delta::FloatConfig quat_config{-1.0f, 1.0f, 0.01f};
    ashiato::BitBuffer quaternion;
    ashiato::sync::delta::write_delta_quaternion(
        quaternion,
        ashiato::sync::delta::Quaternion{0.0f, 0.0f, 0.0f, 1.0f},
        ashiato::sync::delta::Quaternion{0.0f, 0.25f, 0.0f, 0.97f},
        quat_config);

    ashiato::sync::delta::Quaternion out;
    REQUIRE(ashiato::sync::delta::read_delta_quaternion(
        quaternion,
        ashiato::sync::delta::Quaternion{0.0f, 0.0f, 0.0f, 1.0f},
        quat_config,
        out));
    REQUIRE(out.x == Catch::Approx(0.0f));
    REQUIRE(out.y == Catch::Approx(0.25f));
    REQUIRE(out.z == Catch::Approx(0.0f));
    REQUIRE(out.w == Catch::Approx(0.97f));
}

TEST_CASE("serialization helpers quantize bounded floats and integers") {
    const ashiato::sync::serialization::QuantizedFloatConfig float_config{-2.0f, 2.0f, 0.25f};
    REQUIRE(ashiato::sync::serialization::quantized_float_bits(float_config) == 5U);

    ashiato::BitBuffer floats;
    ashiato::sync::serialization::serialize_quantized_float(floats, 1.12f, float_config);
    REQUIRE(floats.bit_size() == 5U);

    float float_out = 0.0f;
    REQUIRE(ashiato::sync::serialization::read_quantized_float(floats, float_config, float_out));
    REQUIRE(float_out == Catch::Approx(1.0f));

    const ashiato::sync::serialization::QuantizedIntConfig int_config{-5, 10};
    REQUIRE(ashiato::sync::serialization::quantized_int_bits(int_config) == 4U);

    ashiato::BitBuffer ints;
    ashiato::sync::serialization::serialize_quantized_int(ints, -3, int_config);
    ashiato::sync::serialization::serialize_quantized_int(ints, 99, int_config);
    REQUIRE(ints.bit_size() == 8U);

    std::int64_t int_out = 0;
    REQUIRE(ashiato::sync::serialization::read_quantized_int(ints, int_config, int_out));
    REQUIRE(int_out == -3);
    REQUIRE(ashiato::sync::serialization::read_quantized_int(ints, int_config, int_out));
    REQUIRE(int_out == 10);
}

TEST_CASE("serialization trace macros support expression-style delta decoding") {
    ashiato::ComponentSerializationContext context;
    (void)context;

    ashiato::BitBuffer floats;
    SERIALIZE_QUANTIZED_FLOAT(15.25f - 10.0f, floats, -20.0f, 20.0f, 0.5f, "float_delta");
    const float decoded_float = 10.0f + DESERIALIZE_QUANTIZED_FLOAT(floats, -20.0f, 20.0f, 0.5f, "float_delta");
    REQUIRE(decoded_float == Catch::Approx(15.5f));

    ashiato::BitBuffer ints;
    SERIALIZE_QUANTIZED_INT(42 - 50, ints, -16, 16, "int_delta");
    const std::int64_t decoded_int = 50 + DESERIALIZE_QUANTIZED_INT(ints, -16, 16, "int_delta");
    REQUIRE(decoded_int == 42);
}

TEST_CASE("variable quantized serializers use short and long ranges") {
    const ashiato::sync::serialization::VariableQuantizedFloatConfig float_config{
        ashiato::sync::serialization::QuantizedFloatConfig{-1.0f, 1.0f, 0.25f},
        ashiato::sync::serialization::QuantizedFloatConfig{-10.0f, 10.0f, 0.25f}};

    ashiato::BitBuffer short_float;
    ashiato::sync::serialization::serialize_variable_quantized_float(short_float, 0.5f, float_config);
    REQUIRE(short_float.bit_size() == 5U);
    REQUIRE_FALSE(short_float.read_bool());
    short_float.reset_read();
    REQUIRE(ashiato::sync::serialization::deserialize_variable_quantized_float(short_float, float_config) ==
            Catch::Approx(0.5f));

    ashiato::BitBuffer long_float;
    ashiato::sync::serialization::serialize_variable_quantized_float(long_float, 5.5f, float_config);
    REQUIRE(long_float.bit_size() == 8U);
    REQUIRE(long_float.read_bool());
    long_float.reset_read();
    REQUIRE(ashiato::sync::serialization::deserialize_variable_quantized_float(long_float, float_config) ==
            Catch::Approx(5.5f));

    const ashiato::sync::serialization::VariableQuantizedIntConfig int_config{
        ashiato::sync::serialization::QuantizedIntConfig{-3, 3},
        ashiato::sync::serialization::QuantizedIntConfig{-100, 100}};

    ashiato::ComponentSerializationContext context;
    (void)context;

    ashiato::BitBuffer short_int;
    SERIALIZE_VARIABLE_QUANTIZED_INT(2, short_int, -3, 3, -100, 100, "short_int");
    REQUIRE(short_int.bit_size() == 4U);
    REQUIRE(DESERIALIZE_VARIABLE_QUANTIZED_INT(short_int, -3, 3, -100, 100, "short_int") == 2);

    ashiato::BitBuffer long_int;
    SERIALIZE_VARIABLE_QUANTIZED_INT(42, long_int, -3, 3, -100, 100, "long_int");
    REQUIRE(long_int.bit_size() == 9U);
    REQUIRE(DESERIALIZE_VARIABLE_VARINT(long_int, -3, 3, -100, 100, "long_int") == 42);
}

TEST_CASE("varint2 uses short and full prefix forms") {
    const ashiato::sync::serialization::VarInt2Config config{
        ashiato::sync::serialization::QuantizedIntConfig{-3, 3},
        ashiato::sync::serialization::QuantizedIntConfig{-100, 100}};

    ashiato::BitBuffer short_value;
    ashiato::sync::serialization::serialize_varint2(short_value, 2, config);
    REQUIRE(short_value.bit_size() == 4U);
    REQUIRE(short_value.read_bool() == false);
    short_value.reset_read();
    REQUIRE(ashiato::sync::serialization::deserialize_varint2(short_value, config) == 2);

    ashiato::BitBuffer full_value;
    ashiato::sync::serialization::serialize_varint2(full_value, 42, config);
    REQUIRE(full_value.bit_size() == 9U);
    REQUIRE(full_value.read_bool() == true);
    full_value.reset_read();
    REQUIRE(ashiato::sync::serialization::deserialize_varint2(full_value, config) == 42);
}

TEST_CASE("varint3 uses short medium and full prefix forms") {
    const ashiato::sync::serialization::VarInt3Config config{
        ashiato::sync::serialization::QuantizedIntConfig{0, 3},
        ashiato::sync::serialization::QuantizedIntConfig{0, 31},
        ashiato::sync::serialization::QuantizedIntConfig{0, 255}};

    ashiato::BitBuffer short_value;
    ashiato::sync::serialization::serialize_varint3(short_value, 2, config);
    REQUIRE(short_value.bit_size() == 3U);
    REQUIRE(short_value.read_bool() == false);
    short_value.reset_read();
    REQUIRE(ashiato::sync::serialization::deserialize_varint3(short_value, config) == 2);

    ashiato::BitBuffer medium_value;
    ashiato::sync::serialization::serialize_varint3(medium_value, 17, config);
    REQUIRE(medium_value.bit_size() == 7U);
    REQUIRE(medium_value.read_bool() == true);
    REQUIRE(medium_value.read_bool() == false);
    medium_value.reset_read();
    REQUIRE(ashiato::sync::serialization::deserialize_varint3(medium_value, config) == 17);

    ashiato::BitBuffer full_value;
    ashiato::sync::serialization::serialize_varint3(full_value, 200, config);
    REQUIRE(full_value.bit_size() == 10U);
    REQUIRE(full_value.read_bool() == true);
    REQUIRE(full_value.read_bool() == true);
    full_value.reset_read();
    REQUIRE(ashiato::sync::serialization::deserialize_varint3(full_value, config) == 200);
}

TEST_CASE("varint or zero helpers reserve the zero prefix") {
    const ashiato::sync::serialization::VarInt2Config varint2_config{
        ashiato::sync::serialization::QuantizedIntConfig{1, 3},
        ashiato::sync::serialization::QuantizedIntConfig{1, 15}};
    const ashiato::sync::serialization::VarInt3Config varint3_config{
        ashiato::sync::serialization::QuantizedIntConfig{1, 3},
        ashiato::sync::serialization::QuantizedIntConfig{1, 15},
        ashiato::sync::serialization::QuantizedIntConfig{1, 255}};

    ashiato::BitBuffer zero;
    ashiato::sync::serialization::serialize_varint2_or_zero(zero, 0, varint2_config);
    REQUIRE(zero.bit_size() == 1U);
    REQUIRE(zero.read_bool() == false);
    zero.reset_read();
    REQUIRE(ashiato::sync::serialization::deserialize_varint2_or_zero(zero, varint2_config) == 0);

    ashiato::BitBuffer short_value;
    ashiato::sync::serialization::serialize_varint2_or_zero(short_value, 2, varint2_config);
    REQUIRE(short_value.bit_size() == 4U);
    REQUIRE(short_value.read_bool() == true);
    REQUIRE(short_value.read_bool() == false);
    short_value.reset_read();
    REQUIRE(ashiato::sync::serialization::deserialize_varint2_or_zero(short_value, varint2_config) == 2);

    ashiato::BitBuffer medium_value;
    ashiato::sync::serialization::serialize_varint3_or_zero(medium_value, 9, varint3_config);
    REQUIRE(medium_value.bit_size() == 7U);
    REQUIRE(medium_value.read_bool() == true);
    REQUIRE(medium_value.read_bool() == true);
    REQUIRE(medium_value.read_bool() == false);
    medium_value.reset_read();
    REQUIRE(ashiato::sync::serialization::deserialize_varint3_or_zero(medium_value, varint3_config) == 9);
}
